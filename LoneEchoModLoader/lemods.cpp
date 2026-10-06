#include "lemods.h"
#include "lemods_core.h"
#include <cmath>
#include <cstring>
#include <mutex>

// Static addresses in loneecho.exe (image base 0x140000000) are written as RVAs (VA - 0x140000000).
namespace LeMods
{
	BYTE* g_exe = NULL;
	VOID(*g_log)(const CHAR* format, ...) = NULL;
	LeModApi g_api = {};
	static BYTE* volatile g_game = NULL;

	static const DWORD GAME_VTABLE = 0xB8E4B8, SLOT_UPDATE = 0x140;   // CR14Game::Update(game, phase)
	static const DWORD FRAME_TIMER_VTABLE = 0xBB6410;                 // slot 1: Tick(timer); timer+0x38 = time scale
	static const DWORD FIND_SPACE = 0x2DDA70;                         // FindGameSpace(game, levelHash)
	static const DWORD FIND_ENGINE_SYSTEM = 0xAB2A0;                  // EngineComponentSystem(space, typeHash)
	static const DWORD NAV_HEAD = 0x510C50, NAV_HAND = 0x510540;      // player nav: head / hand world transforms
	static const DWORD SP_GLOBAL_LEVEL = 0x14A7088;                   // UINT64 hash of r14_glb_global (Jack's level)
	static const DWORD PHYSICS_HANDLE = 0x2A0480, RESOLVE_BODY = 0x157F20, SET_BODY = 0x181C80;
	static const UINT64 HASH_PLAYER_NAV = 0x559BE58A8EB1033CULL, HASH_PHYSICS = 0x5B8CC538E22AD937ULL;

	typedef UINT64(__fastcall* UpdateFn)(BYTE* game, UINT64 phase);
	typedef VOID(*TickFn)(BYTE* timer);
	typedef BYTE* (__fastcall* FindSpaceFn)(BYTE* game, UINT64 levelHash);
	typedef BYTE* (__fastcall* FindSystemFn)(BYTE* space, UINT64 typeHash);
	typedef Trs* (__fastcall* NavHeadFn)(BYTE* cs, Trs* out, UINT16 index);
	typedef Trs* (__fastcall* NavHandFn)(BYTE* cs, Trs* out, UINT16 index, INT hand, INT flag);
	typedef BYTE* (__fastcall* PhysicsHandleFn)(BYTE* cs, BYTE* out, UINT16 index);
	typedef BYTE* (__fastcall* ResolveBodyFn)(BYTE* handle);
	typedef VOID(__fastcall* SetBodyFn)(BYTE* body, const FLOAT* position, const FLOAT* rotation);

	static UpdateFn g_originalUpdate = NULL;
	static TickFn g_originalTick = NULL;
	static BYTE* volatile g_timer = NULL;

	// ---- Game access ----
	static UINT16 Count(BYTE* cs) { return cs == NULL ? 0 : *(UINT16*)(cs + 0xE4); }

	BYTE* FindSystem(BYTE* space, UINT64 typeHash)
	{
		return space == NULL ? NULL : ((FindSystemFn)(g_exe + FIND_ENGINE_SYSTEM))(space, typeHash);
	}

	UINT64 SpaceLevel(BYTE* space) { return space == NULL ? 0 : *(UINT64*)(space + 0xD8); }

	BYTE* GlobalSpace()
	{
		BYTE* game = g_game;
		return game == NULL ? NULL : ((FindSpaceFn)(g_exe + FIND_SPACE))(game, *(UINT64*)(g_exe + SP_GLOBAL_LEVEL));
	}

	BYTE* PlayerNav()
	{
		BYTE* nav = FindSystem(GlobalSpace(), HASH_PLAYER_NAV);
		return Count(nav) > 0 ? nav : NULL;
	}

	/// Jack's record: the nav system's index map (+0xb8, 4-byte entries, slot in the low word) into its data (+0xe8).
	BYTE* PlayerEntry()
	{
		BYTE* nav = PlayerNav();
		if (nav == NULL)
			return NULL;
		UINT16 slot = *(UINT16*)*(BYTE**)(nav + 0xB8);
		return *(BYTE**)(nav + 0xE8) + slot * NAV_ENTRY_SIZE;
	}

	BOOL Head(Trs* out)
	{
		BYTE* nav = PlayerNav();
		if (nav == NULL)
			return FALSE;
		((NavHeadFn)(g_exe + NAV_HEAD))(nav, out, 0);
		return TRUE;
	}

	BOOL Hand(int hand, Trs* out)
	{
		BYTE* nav = PlayerNav();
		if (nav == NULL)
			return FALSE;
		((NavHandFn)(g_exe + NAV_HAND))(nav, out, 0, hand != 0 ? 1 : 0, 1);
		return TRUE;
	}

	/// Class name (RTTI) of an object with a vtable, or NULL.
	const CHAR* ClassOf(BYTE* object)
	{
		__try
		{
			BYTE* vtable = *(BYTE**)object;
			BYTE* col = *(BYTE**)(vtable - 8);
			return (const CHAR*)(g_exe + *(DWORD*)(col + 12) + 16);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
	}

	BYTE* ResolveBody(BYTE* physics, UINT16 index)
	{
		BYTE handle[0x40] = {};
		BYTE* h = ((PhysicsHandleFn)(g_exe + PHYSICS_HANDLE))(physics, handle, index);
		return h == NULL ? NULL : ((ResolveBodyFn)(g_exe + RESOLVE_BODY))(h);
	}

	static BYTE* SpaceAt(UINT64 i)
	{
		BYTE* game = g_game;
		if (game == NULL || i >= *(UINT64*)(game + 0x658) || i >= 64)
			return NULL;
		return *(BYTE**)(*(BYTE**)(game + 0x630) + i * 16 + 8);
	}

	/// Calls visit(space) for every loaded game space (the game's sorted list: game+0x630, 16-byte entries, count +0x658).
	VOID ForEachSpace(const std::function<VOID(BYTE* space)>& visit)
	{
		BYTE* game = g_game;
		if (game == NULL)
			return;
		UINT64 count = *(UINT64*)(game + 0x658);
		for (UINT64 i = 0; i < count && i < 64; i++)
		{
			BYTE* space = SpaceAt(i);
			if (space != NULL)
				visit(space);
		}
	}

	// ---- API for mods ----
	static void* ApiGame() { return g_game; }
	static int ApiHead(LeModTransform* out) { return Head((Trs*)out); }
	static int ApiHand(int hand, LeModTransform* out) { return Hand(hand, (Trs*)out); }

	static BYTE* SafeResolveBody(BYTE* physics, UINT16 index)
	{
		__try { return ResolveBody(physics, index); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
	}

	static void* ApiBodyNear(const float position[3], float radius)
	{
		BYTE* global = GlobalSpace();
		BYTE* best = NULL;
		FLOAT bestDistance = radius;
		ForEachSpace([&](BYTE* space) {
			if (space == global)
				return;
			BYTE* physics = FindSystem(space, HASH_PHYSICS);
			UINT16 n = Count(physics);
			for (UINT16 i = 0; i < n; i++)
			{
				BYTE* body = SafeResolveBody(physics, i);
				if (body == NULL)
					continue;
				FLOAT* p = (FLOAT*)(body + 0x938);
				FLOAT d = sqrtf((p[0] - position[0]) * (p[0] - position[0]) + (p[1] - position[1]) * (p[1] - position[1]) +
					(p[2] - position[2]) * (p[2] - position[2]));
				if (d < bestDistance)
				{
					bestDistance = d;
					best = body;
				}
			}
		});
		return best;
	}

	// Rigid body fields: +0x938 position, +0x950 linear velocity, +0xc00 orientation (quaternion).
	static int ApiBodyGet(void* body, float position[3], float rotation[4], float velocity[3])
	{
		BYTE* b = (BYTE*)body;
		if (b == NULL)
			return 0;
		if (position) memcpy(position, b + 0x938, 12);
		if (rotation) memcpy(rotation, b + 0xC00, 16);
		if (velocity) memcpy(velocity, b + 0x950, 12);
		return 1;
	}

	static void ApiBodySet(void* body, const float position[3], const float rotation[4], const float velocity[3])
	{
		BYTE* b = (BYTE*)body;
		if (b == NULL)
			return;
		FLOAT p[3], r[4];
		memcpy(p, position ? position : (const float*)(b + 0x938), sizeof(p));
		memcpy(r, rotation ? rotation : (const float*)(b + 0xC00), sizeof(r));
		if (position || rotation)
			((SetBodyFn)(g_exe + SET_BODY))(b, p, r);
		if (velocity)
			memcpy(b + 0x950, velocity, 12);
	}

	// Time scale: the frame timer's multiplier (timer+0x38), written before every tick while a mod has set one.
	static volatile FLOAT g_wantedTimeScale = 1.0f;
	static volatile LONG g_timeScaleSet = 0;
	FLOAT TimeScale() { BYTE* t = g_timer; return t == NULL ? 1.0f : *(FLOAT*)(t + 0x38); }
	VOID SetTimeScale(FLOAT scale)
	{
		g_wantedTimeScale = scale;
		InterlockedExchange(&g_timeScaleSet, 1);
	}
	VOID ResetTimeScale()
	{
		InterlockedExchange(&g_timeScaleSet, 0);
		BYTE* t = g_timer;
		if (t != NULL)
			*(FLOAT*)(t + 0x38) = 1.0f;
	}
	static float ApiTimeScale() { return TimeScale(); }
	static void ApiSetTimeScale(float scale) { SetTimeScale(scale); }
	static void ApiRefreshWindow() { RefreshWindow(); }
	static uint8_t* ApiPlayer() { return PlayerEntry(); }
	static int ApiSpaceCount() { BYTE* game = g_game; return game == NULL ? 0 : (int)min(*(UINT64*)(game + 0x658), 64ULL); }
	static void* ApiSpace(int index) { return index < 0 ? NULL : SpaceAt((UINT64)index); }
	static void* ApiFindSystem(void* space, uint64_t typeHash) { return FindSystem((BYTE*)space, typeHash); }
	static void ApiRunOnGameThread(void (*fn)(void*), void* arg) { RunOnGameThread([fn, arg]() { fn(arg); }); }

	// ---- Mods ----
	static std::vector<LoadedMod> g_mods;
	static std::mutex g_queueLock;
	static std::vector<std::function<VOID()>> g_queue;  // work from the window, run on the game thread
	static volatile LONG g_ready = 0;  // set once every mod DLL is loaded (on the window thread)

	std::vector<LoadedMod>& Mods() { return g_mods; }

	VOID RunOnGameThread(std::function<VOID()> work)
	{
		std::lock_guard<std::mutex> lock(g_queueLock);
		g_queue.push_back(work);
	}

	static BOOL SafeCall(void (*fn)(void))
	{
		__try { fn(); return TRUE; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; }
	}
	static BOOL SafeCallInt(void (*fn)(int), int value)
	{
		__try { fn(value); return TRUE; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; }
	}
	static BOOL SafeFrame(void (*frame)(void*), void* game)
	{
		__try { frame(game); return TRUE; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; }
	}
	static int SafeRegister(LeModRegisterFn reg, LeMod* mods, int capacity)
	{
		__try { return reg(&g_api, mods, capacity); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
	}
	int SafeCount(int (*count)(void))
	{
		__try { return count(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	}
	const CHAR* SafeItem(const char* (*item)(int), int index)
	{
		__try { return item(index); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
	}
	const CHAR* SafeStatus(const char* (*status)(void))
	{
		__try { return status(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
	}
	int SafeActive(int (*active)(int), int index)
	{
		__try { return active(index); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	}

	/// On the game thread.
	static VOID SetEnabled(size_t index, BOOL enabled)
	{
		LoadedMod& m = g_mods[index];
		if ((m.enabled != 0) == (enabled != 0))
			return;
		InterlockedExchange(&m.enabled, enabled ? 1 : 0);
		void (*fn)(void) = enabled ? m.mod.enable : m.mod.disable;
		if (fn != NULL && !SafeCall(fn))
			g_log("[MODS] %s: exception while switching %s", m.mod.name, enabled ? "on" : "off");
		g_log("[MODS] %s %s", m.mod.name, enabled ? "on" : "off");
		RefreshWindow();
	}

	VOID RequestEnable(size_t index, BOOL enabled)
	{
		if (index < g_mods.size())
			RunOnGameThread([index, enabled]() { SetEnabled(index, enabled); });
	}

	VOID RequestButton(size_t index, int button)
	{
		if (index >= g_mods.size() || button < 0 || button >= LEMOD_MAX_BUTTONS)
			return;
		RunOnGameThread([index, button]() {
			LoadedMod& m = g_mods[index];
			void (*pressed)(void) = m.mod.buttons[button].pressed;
			if (pressed != NULL && !SafeCall(pressed))
				g_log("[MODS] %s: exception in button '%s'", m.mod.name, m.mod.buttons[button].label);
			RefreshWindow();
		});
	}

	VOID RequestListAction(size_t index, int item)
	{
		if (index >= g_mods.size() || item < 0)
			return;
		RunOnGameThread([index, item]() {
			LoadedMod& m = g_mods[index];
			if (m.mod.listActivate != NULL && !SafeCallInt(m.mod.listActivate, item))
				g_log("[MODS] %s: exception in its list action", m.mod.name);
			RefreshWindow();
		});
	}

	static VOID RunFrame(BYTE* game)
	{
		if (!g_ready)
			return;
		std::vector<std::function<VOID()>> work;
		{
			std::lock_guard<std::mutex> lock(g_queueLock);
			work.swap(g_queue);
		}
		for (auto& w : work)
			w();
		for (LoadedMod& m : g_mods)
			if (m.enabled && m.mod.frame != NULL && !SafeFrame(m.mod.frame, game))
			{
				g_log("[MODS] %s: exception in its frame; switched off", m.mod.name);
				InterlockedExchange(&m.enabled, 0);
				if (m.mod.disable != NULL)
					SafeCall(m.mod.disable);
				RefreshWindow();
			}
	}

	static UINT64 __fastcall HookedUpdate(BYTE* game, UINT64 phase)
	{
		if (g_game != game)
		{
			g_game = game;
			g_log("[MODS] game object %p", game);
		}
		UINT64 result = g_originalUpdate(game, phase);
		if (phase == 1)
			RunFrame(game);
		return result;
	}

	static VOID HookedTick(BYTE* timer)
	{
		g_timer = timer;
		if (g_timeScaleSet)
			*(FLOAT*)(timer + 0x38) = g_wantedTimeScale;
		g_originalTick(timer);
	}

	/// DLL mods: bin\win7\mods\*.dll exporting LeModRegister.
	static VOID LoadModDlls()
	{
		CHAR folder[MAX_PATH];
		GetModuleFileNameA(NULL, folder, MAX_PATH);
		CHAR* slash = strrchr(folder, '\\');
		if (slash != NULL)
			*(slash + 1) = 0;
		strcat_s(folder, "mods\\");
		CreateDirectoryA(folder, NULL);
		std::string pattern = std::string(folder) + "*.dll";
		WIN32_FIND_DATAA found;
		HANDLE search = FindFirstFileA(pattern.c_str(), &found);
		if (search == INVALID_HANDLE_VALUE)
			return;
		do
		{
			std::string path = std::string(folder) + found.cFileName;
			HMODULE module = LoadLibraryA(path.c_str());
			LeModRegisterFn reg = module == NULL ? NULL : (LeModRegisterFn)GetProcAddress(module, "LeModRegister");
			if (reg == NULL)
			{
				g_log("[MODS] %s: not a mod (no LeModRegister export)", found.cFileName);
				continue;
			}
			LeMod mods[16] = {};
			int n = SafeRegister(reg, mods, 16);
			if (n < 0)
			{
				n = 0;
				g_log("[MODS] %s: exception in LeModRegister", found.cFileName);
			}
			for (int i = 0; i < n && i < 16; i++)
				g_mods.push_back({ mods[i], 0, found.cFileName });
			g_log("[MODS] %s: %d mod(s)", found.cFileName, n);
		} while (FindNextFileA(search, &found));
		FindClose(search);
	}

	/// Runs on the window thread before the window shows: mod DLLs load here rather than in Install, which runs
	/// inside DllMain (the loader lock).
	static VOID LoadAndStart()
	{
		LoadModDlls();
		g_log("[MODS] mod loader: %zu mods (window: Lone Echo Mods)", g_mods.size());
		InterlockedExchange(&g_ready, 1);
	}

	static VOID PatchVtableSlot(VOID** slot, VOID* hook, VOID** original)
	{
		DWORD old;
		VirtualProtect(slot, sizeof(VOID*), PAGE_READWRITE, &old);
		*original = *slot;
		*slot = hook;
		VirtualProtect(slot, sizeof(VOID*), old, &old);
	}

	VOID Install(BYTE* exe, VOID(*log)(const CHAR* format, ...))
	{
		g_exe = exe;
		g_log = log;
		g_api.version = LEMOD_API_VERSION;
		g_api.exe = exe;
		g_api.log = log;
		g_api.game = ApiGame;
		g_api.head = ApiHead;
		g_api.hand = ApiHand;
		g_api.bodyNear = ApiBodyNear;
		g_api.bodyGet = ApiBodyGet;
		g_api.bodySet = ApiBodySet;
		g_api.timeScale = ApiTimeScale;
		g_api.setTimeScale = ApiSetTimeScale;
		g_api.refreshWindow = ApiRefreshWindow;
		g_api.player = ApiPlayer;
		g_api.spaceCount = ApiSpaceCount;
		g_api.space = ApiSpace;
		g_api.findSystem = ApiFindSystem;
		g_api.runOnGameThread = ApiRunOnGameThread;

		PatchVtableSlot((VOID**)(exe + GAME_VTABLE + SLOT_UPDATE), (VOID*)HookedUpdate, (VOID**)&g_originalUpdate);
		PatchVtableSlot((VOID**)(exe + FRAME_TIMER_VTABLE + 8), (VOID*)HookedTick, (VOID**)&g_originalTick);
		RegisterBuiltIns();
		StartWindow(LoadAndStart);
	}
}
