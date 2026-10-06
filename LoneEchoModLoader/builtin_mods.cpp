// The mods that come with the loader: No clip, Doors & buttons, Boosts, Tools, Time, Freeze.
#include "lemods_core.h"
#include <detours.h>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <algorithm>

namespace LeMods
{
	// ---- Status lines (written on the game thread, read by the window) ----
	struct StatusText
	{
		CHAR text[2][192] = {};
		volatile LONG current = 0;
		VOID Set(const CHAR* format, ...)
		{
			CHAR line[192];
			va_list args;
			va_start(args, format);
			vsnprintf(line, sizeof(line), format, args);
			va_end(args);
			if (strcmp(line, text[current]) == 0)
				return;
			LONG next = 1 - current;
			strcpy_s(text[next], line);
			InterlockedExchange(&current, next);
			RefreshWindow();
		}
		const CHAR* Get() const { return text[current]; }
	};

	static VOID Attach(VOID** original, VOID* hook, const CHAR* what)
	{
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach(original, hook);
		g_log("[MODS] hook %s: %s", what, DetourTransactionCommit() == NO_ERROR ? "installed" : "FAILED");
	}

	// ---- Movement tuning: the player-movement globals the nav code reads every frame (0x140515bb0 fills them from
	// the movement settings: boost accel/power_cost/time/max_vel/recharge_time, iron_man = hand thrusters). ----
	enum Tunable { BOOST_ACCEL, BOOST_POWER_COST, BOOST_TIME, BOOST_MAX_VEL, HAND_COST, HAND_MAX_VEL_ONE, HAND_MAX_VEL_TWO,
		HAND_ACCEL, BOOST_RECHARGE, TUNABLES };
	static const DWORD TUNE_RVA[TUNABLES] = { 0x14C84E4, 0x14C84E8, 0x14C84EC, 0x14C84F0, 0x14C84F4, 0x14C84F8, 0x14C84FC,
		0x14C8500, 0x14C8504 };
	static FLOAT g_tuneBase[TUNABLES];
	static BOOL g_tuneCaptured = FALSE;
	static BOOL g_noclipOn = FALSE, g_boostsOn = FALSE;
	static FLOAT g_flySpeed = 4.0f;    // No clip: hand thruster multiplier
	static FLOAT g_handSpeed = 3.0f;   // Boosts: hand thruster multiplier
	static int g_boostLevel = 1;       // Boosts: 0 normal, 1 big, 2 huge

	static FLOAT& Tune(Tunable t) { return *(FLOAT*)(g_exe + TUNE_RVA[t]); }

	static VOID ApplyTuning()
	{
		if (!g_tuneCaptured)
		{
			for (int t = 0; t < TUNABLES; t++)
				g_tuneBase[t] = Tune((Tunable)t);
			g_tuneCaptured = TRUE;
			g_log("[MODS] movement settings: boost accel %.2f max %.2f time %.3f recharge %.2f; hand thrust max %.2f/%.2f accel %.2f",
				g_tuneBase[BOOST_ACCEL], g_tuneBase[BOOST_MAX_VEL], g_tuneBase[BOOST_TIME], g_tuneBase[BOOST_RECHARGE],
				g_tuneBase[HAND_MAX_VEL_ONE], g_tuneBase[HAND_MAX_VEL_TWO], g_tuneBase[HAND_ACCEL]);
		}
		FLOAT hand = (g_boostsOn ? g_handSpeed : 1.0f) * (g_noclipOn ? g_flySpeed : 1.0f);
		Tune(HAND_MAX_VEL_ONE) = g_tuneBase[HAND_MAX_VEL_ONE] * hand;
		Tune(HAND_MAX_VEL_TWO) = g_tuneBase[HAND_MAX_VEL_TWO] * hand;
		Tune(HAND_ACCEL) = g_tuneBase[HAND_ACCEL] * hand;
		Tune(HAND_COST) = g_boostsOn ? 0.0f : g_tuneBase[HAND_COST];
		int level = g_boostsOn ? g_boostLevel : 0;
		static const FLOAT speed[3] = { 1.0f, 3.0f, 7.0f }, length[3] = { 1.0f, 2.0f, 3.0f };
		Tune(BOOST_ACCEL) = g_tuneBase[BOOST_ACCEL] * speed[level];
		Tune(BOOST_MAX_VEL) = g_tuneBase[BOOST_MAX_VEL] * speed[level];
		Tune(BOOST_TIME) = g_tuneBase[BOOST_TIME] * length[level];
		Tune(BOOST_POWER_COST) = g_boostsOn ? 0.0f : g_tuneBase[BOOST_POWER_COST];
		Tune(BOOST_RECHARGE) = g_boostsOn ? 0.05f : g_tuneBase[BOOST_RECHARGE];
	}

	// ---- Ability locks: 13 bytes in Jack's record (+0x8f4), each a mask of the reasons the story has switched that
	// ability off (R14EnableMovementNode / R14EnableInputNode -> 0x14059ac50). 0 is everything (cutscenes); 4 the hand
	// thrusters; 10 the back boost (checked at 0x14050b216). ----
	static const DWORD NAV_LOCKS = 0x8F4;
	static const int LOCK_COUNT = 13, LOCK_HAND_THRUST = 4, LOCK_BOOST = 10;

	static VOID ClearLock(BYTE* player, int lock) { if (player != NULL) player[NAV_LOCKS + lock] = 0; }

	// ==== No clip ====
	// Jack's collision is his head controller (player record +8: position +0x60, velocity +0x70), a mirror of the head
	// that drives a physics proxy. Before physics the nav job (0x14051b000) copies the head into it and steps it
	// (0x1406d89c0: position += velocity * dt, proxy teleported there). After physics the second nav job (0x1405197a0)
	// settles it (0x1406d90d0: velocity = how the proxy actually moved), reads the proxy's solved position back
	// (0x1406d8680) and moves Jack by however far that is from his head, velocity included. That readback is the
	// collision. With No clip on, the settle and the readback get the stepped position and velocity back, so Jack goes
	// wherever his velocity takes him.
	typedef UINT64(__fastcall* ControllerStepFn)(BYTE* controller, FLOAT dt, VOID* a3, VOID* a4, VOID* a5);
	typedef UINT64(__fastcall* ControllerSettleFn)(BYTE* controller, VOID* a2, VOID* a3, FLOAT dt, VOID* a5);
	typedef VOID(__fastcall* ControllerReadBackFn)(BYTE* controller);
	static const DWORD CONTROLLER_STEP = 0x6D89C0, CONTROLLER_SETTLE = 0x6D90D0, CONTROLLER_READ_BACK = 0x6D8680;
	static ControllerStepFn g_originalStep = NULL;
	static ControllerSettleFn g_originalSettle = NULL;
	static ControllerReadBackFn g_originalReadBack = NULL;
	static BYTE* volatile g_noclipController = NULL;
	static FLOAT g_flyPosition[3], g_flyVelocity[3];
	static volatile LONG g_flyStepped = 0;  // the step ran this frame and the readback hasn't used it yet
	static StatusText g_noclipStatus;

	static BOOL IsNoclipController(BYTE* controller) { return controller != NULL && controller == g_noclipController; }

	static UINT64 __fastcall HookedStep(BYTE* controller, FLOAT dt, VOID* a3, VOID* a4, VOID* a5)
	{
		UINT64 result = g_originalStep(controller, dt, a3, a4, a5);
		if (IsNoclipController(controller))
		{
			memcpy(g_flyPosition, controller + 0x60, sizeof(g_flyPosition));
			memcpy(g_flyVelocity, controller + 0x70, sizeof(g_flyVelocity));
			InterlockedExchange(&g_flyStepped, 1);
		}
		return result;
	}

	static UINT64 __fastcall HookedSettle(BYTE* controller, VOID* a2, VOID* a3, FLOAT dt, VOID* a5)
	{
		if (!IsNoclipController(controller) || !g_flyStepped)
			return g_originalSettle(controller, a2, a3, dt, a5);
		// +0x164: the game teleported Jack this frame (the settle stops him); leave that alone.
		BOOL teleported = *(DWORD*)(controller + 0x164) != 0;
		UINT64 result = g_originalSettle(controller, a2, a3, dt, a5);
		if (teleported)
			InterlockedExchange(&g_flyStepped, 0);
		else
			memcpy(controller + 0x70, g_flyVelocity, sizeof(g_flyVelocity));
		return result;
	}

	static VOID __fastcall HookedReadBack(BYTE* controller)
	{
		if (IsNoclipController(controller) && InterlockedExchange(&g_flyStepped, 0))
			memcpy(controller + 0x60, g_flyPosition, sizeof(g_flyPosition));
		else
			g_originalReadBack(controller);
	}

	static VOID NoclipEnable()
	{
		if (g_originalStep == NULL)
		{
			g_originalStep = (ControllerStepFn)(g_exe + CONTROLLER_STEP);
			Attach((VOID**)&g_originalStep, (VOID*)HookedStep, "player controller step");
			g_originalSettle = (ControllerSettleFn)(g_exe + CONTROLLER_SETTLE);
			Attach((VOID**)&g_originalSettle, (VOID*)HookedSettle, "player controller settle");
			g_originalReadBack = (ControllerReadBackFn)(g_exe + CONTROLLER_READ_BACK);
			Attach((VOID**)&g_originalReadBack, (VOID*)HookedReadBack, "player controller read back");
		}
		g_noclipOn = TRUE;
		ApplyTuning();
	}

	static VOID NoclipDisable()
	{
		g_noclipController = NULL;
		InterlockedExchange(&g_flyStepped, 0);
		g_noclipOn = FALSE;
		ApplyTuning();
	}

	static VOID NoclipFrame(void*)
	{
		BYTE* player = PlayerEntry();
		g_noclipController = player == NULL ? NULL : *(BYTE**)(player + 8);
		ApplyTuning();
		ClearLock(player, LOCK_HAND_THRUST);
		if (player == NULL)
			g_noclipStatus.Set("Waiting for the player");
		else
		{
			FLOAT* v = (FLOAT*)(player + 0x500);
			g_noclipStatus.Set("Flying through walls  -  %.1f m/s, thrusters x%g", sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), g_flySpeed);
		}
	}

	static VOID NoclipStop()
	{
		BYTE* player = PlayerEntry();
		if (player == NULL)
			return;
		memset(player + 0x500, 0, 12);
		BYTE* controller = *(BYTE**)(player + 8);
		if (controller != NULL)
			memset(controller + 0x70, 0, 12);
	}

	static VOID FlySpeed(FLOAT speed) { g_flySpeed = speed; ApplyTuning(); }
	static VOID Fly1() { FlySpeed(1.0f); }
	static VOID Fly4() { FlySpeed(4.0f); }
	static VOID Fly10() { FlySpeed(10.0f); }
	static VOID Fly25() { FlySpeed(25.0f); }
	static int NoclipActive(int i)
	{
		static const FLOAT speeds[] = { 1.0f, 4.0f, 10.0f, 25.0f };
		return i < 4 && g_flySpeed == speeds[i];
	}
	static const char* NoclipStatus() { return g_noclipStatus.Get(); }

	// ==== Doors & buttons ====
	// Every button (CR14ButtonInteractCS, doors' panels included) in the loaded levels. Pressing one here does what a
	// real press does: the button's OnPressStarted / OnPress / OnFullyDepressed events go to the level scripts.
	static const UINT64 HASH_BUTTON = 0xF5574A730BBB0428ULL;
	static const DWORD PRESS_STARTED = 0x5847C0, PRESS = 0x584740, FULLY_DEPRESSED = 0x5846C0;  // (cs, componentIndex)
	typedef VOID(__fastcall* ButtonEventFn)(BYTE* cs, UINT64 index);

	struct FoundButton { BYTE* cs; UINT16 index; UINT64 level; FLOAT pos[3]; FLOAT distance; };
	static std::vector<FoundButton> g_found;          // game thread
	static std::mutex g_labelLock;
	static std::vector<std::string> g_labels;         // for the window
	static StatusText g_doorStatus;
	static ULONGLONG g_lastLabelUpdate = 0;

	/// The world position of button i of a button system (record stride 0x190 at +0x100, index map +0xd0, count +0xf8,
	/// position +0xf0 in the record).
	static BOOL ButtonPosition(BYTE* cs, UINT16 i, FLOAT* out)
	{
		__try
		{
			UINT16 slot = *(UINT16*)(*(BYTE**)(cs + 0xD0) + i * 4);
			memcpy(out, *(BYTE**)(cs + 0x100) + slot * 0x190 + 0xF0, 12);
			return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]) && (out[0] != 0 || out[1] != 0 || out[2] != 0);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; }
	}

	static FLOAT Distance(const FLOAT* a, const FLOAT* b)
	{
		FLOAT dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
		return sqrtf(dx * dx + dy * dy + dz * dz);
	}

	static VOID UpdateLabels()
	{
		std::vector<std::string> labels;
		CHAR line[160];
		for (size_t i = 0; i < g_found.size(); i++)
		{
			const FoundButton& b = g_found[i];
			snprintf(line, sizeof(line), "%5.1f m   button %u   (level %04X)", b.distance, b.index, (unsigned)(b.level & 0xFFFF));
			labels.push_back(line);
		}
		std::lock_guard<std::mutex> lock(g_labelLock);
		g_labels.swap(labels);
	}

	static VOID ScanButtons()
	{
		Trs head = {};
		BOOL haveHead = Head(&head);
		g_found.clear();
		ForEachSpace([&](BYTE* space) {
			BYTE* cs = FindSystem(space, HASH_BUTTON);
			if (cs == NULL)
				return;
			UINT16 n = *(UINT16*)(cs + 0xF8);
			for (UINT16 i = 0; i < n && i < 2048; i++)
			{
				FoundButton b = { cs, i, SpaceLevel(space) };
				if (!ButtonPosition(cs, i, b.pos))
					continue;
				b.distance = haveHead ? Distance(b.pos, head.pos) : 0;
				g_found.push_back(b);
			}
		});
		std::sort(g_found.begin(), g_found.end(), [](const FoundButton& a, const FoundButton& b) { return a.distance < b.distance; });
		UpdateLabels();
		g_doorStatus.Set("%zu buttons in the loaded levels", g_found.size());
		RefreshWindow();
	}

	/// The button is still there (its level hasn't been unloaded).
	static BOOL StillLoaded(const FoundButton& b)
	{
		BOOL found = FALSE;
		ForEachSpace([&](BYTE* space) {
			if (SpaceLevel(space) == b.level && FindSystem(space, HASH_BUTTON) == b.cs)
				found = TRUE;
		});
		return found && b.index < *(UINT16*)(b.cs + 0xF8);
	}

	static BOOL SafePress(BYTE* cs, UINT16 index)
	{
		__try
		{
			((ButtonEventFn)(g_exe + PRESS_STARTED))(cs, index);
			((ButtonEventFn)(g_exe + PRESS))(cs, index);
			((ButtonEventFn)(g_exe + FULLY_DEPRESSED))(cs, index);
			return TRUE;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; }
	}

	static VOID PressButton(const FoundButton& b)
	{
		if (!StillLoaded(b))
		{
			g_log("[MODS] Doors: that button's level isn't loaded any more; refresh the list");
			g_doorStatus.Set("That button's level was unloaded - press Refresh");
			return;
		}
		BOOL ok = SafePress(b.cs, b.index);
		g_log("[MODS] Doors: pressed button %u of level %016llX at (%.1f %.1f %.1f)%s", b.index, (unsigned long long)b.level,
			b.pos[0], b.pos[1], b.pos[2], ok ? "" : " (exception)");
		g_doorStatus.Set("Pressed button %u (%.1f m away)", b.index, b.distance);
	}

	static VOID DoorsEnable() { ScanButtons(); }
	static VOID DoorsRefresh() { ScanButtons(); }
	static VOID DoorsPressNearest()
	{
		ScanButtons();
		if (!g_found.empty())
			PressButton(g_found[0]);
	}
	static VOID DoorsPressNearby()
	{
		ScanButtons();
		int n = 0;
		for (const FoundButton& b : g_found)
			if (b.distance <= 6.0f)
			{
				PressButton(b);
				n++;
			}
		g_doorStatus.Set("Pressed %d buttons within 6 m", n);
	}

	/// Distances refresh twice a second; the order changes only on Refresh, so the selection stays put.
	static VOID DoorsFrame(void*)
	{
		ULONGLONG now = GetTickCount64();
		if (now - g_lastLabelUpdate < 500)
			return;
		g_lastLabelUpdate = now;
		Trs head = {};
		if (!Head(&head))
			return;
		for (FoundButton& b : g_found)
			b.distance = Distance(b.pos, head.pos);
		UpdateLabels();
		RefreshWindow();
	}

	static int DoorsCount()
	{
		std::lock_guard<std::mutex> lock(g_labelLock);
		return (int)g_labels.size();
	}
	static const char* DoorsItem(int index)
	{
		static std::string copy;  // the window thread is the only caller
		std::lock_guard<std::mutex> lock(g_labelLock);
		copy = index >= 0 && index < (int)g_labels.size() ? g_labels[index] : "";
		return copy.c_str();
	}
	static VOID DoorsActivate(int index)
	{
		if (index >= 0 && index < (int)g_found.size())
			PressButton(g_found[index]);
	}
	static const char* DoorsStatus() { return g_doorStatus.Get(); }

	// ==== Boosts ====
	static StatusText g_boostStatus;
	static VOID BoostsEnable() { g_boostsOn = TRUE; ApplyTuning(); }
	static VOID BoostsDisable() { g_boostsOn = FALSE; ApplyTuning(); }
	static VOID BoostsFrame(void*)
	{
		BYTE* player = PlayerEntry();
		ClearLock(player, LOCK_HAND_THRUST);
		ClearLock(player, LOCK_BOOST);
		ApplyTuning();
		static const CHAR* levels[3] = { "normal", "big", "huge" };
		g_boostStatus.Set("Hand thrusters x%g, no power cost  -  back boost %s, no cooldown", g_handSpeed, levels[g_boostLevel]);
	}
	static VOID HandSpeed(FLOAT speed) { g_handSpeed = speed; ApplyTuning(); }
	static VOID Hand1() { HandSpeed(1.0f); }
	static VOID Hand3() { HandSpeed(3.0f); }
	static VOID Hand6() { HandSpeed(6.0f); }
	static VOID Hand12() { HandSpeed(12.0f); }
	static VOID Boost(int level) { g_boostLevel = level; ApplyTuning(); }
	static VOID BoostNormal() { Boost(0); }
	static VOID BoostBig() { Boost(1); }
	static VOID BoostHuge() { Boost(2); }
	static int BoostsActive(int i)
	{
		static const FLOAT hands[] = { 1.0f, 3.0f, 6.0f, 12.0f };
		if (i < 4)
			return g_handSpeed == hands[i];
		return i - 4 == g_boostLevel;
	}
	static const char* BoostsStatus() { return g_boostStatus.Get(); }

	// ==== Tools ====
	// Tools are run by Jack's tool ability manager (CR14ToolAbilityManagerCS): R14ActivateToolNode calls 0x1407cfc10
	// (manager, component, tool type, state) with type 4 cutter, 5 scanner, 6 snapshot camera (0 puts the tool away).
	static const UINT64 HASH_TOOL_MANAGER = 0xE4E954CFA69AFF02ULL;
	static const DWORD ACTIVATE_TOOL = 0x7CFC10;
	typedef VOID(__fastcall* ActivateToolFn)(BYTE* manager, UINT64 component, UINT64 type, UINT64 stateHash);
	static const UINT64 STATE_CUTTER = 0x8576B6E26DD67601ULL;    // CR14CutterToolActiveState
	static const UINT64 STATE_SCANNER = 0xFB507E87EF0AECDFULL;   // CR14ScanningToolPointScanState
	static const UINT64 STATE_SNAPSHOT = 0xD98E5871FF5C950BULL;  // CR14SnapshotToolActiveState
	static StatusText g_toolStatus;
	static int g_toolChoice = -1;

	static BOOL SafeActivateTool(BYTE* manager, int type, UINT64 state)
	{
		__try { ((ActivateToolFn)(g_exe + ACTIVATE_TOOL))(manager, 0, (UINT64)type, state); return TRUE; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; }
	}

	static VOID ActivateTool(int type, UINT64 state, const CHAR* name)
	{
		BYTE* manager = FindSystem(GlobalSpace(), HASH_TOOL_MANAGER);
		if (manager == NULL)
		{
			g_toolStatus.Set("No tool manager yet (load into the game first)");
			return;
		}
		BOOL ok = SafeActivateTool(manager, type, state);
		g_log("[MODS] Tools: %s%s", name, ok ? "" : " (exception)");
		g_toolChoice = type;
	}

	static VOID UnlockAll()
	{
		BYTE* player = PlayerEntry();
		for (int i = 0; i < LOCK_COUNT; i++)
			ClearLock(player, i);
		g_log("[MODS] Tools: every ability unlocked");
	}

	static VOID ToolsFrame(void*)
	{
		BYTE* player = PlayerEntry();
		if (player == NULL)
		{
			g_toolStatus.Set("Waiting for the player");
			return;
		}
		// Keep 1..12 unlocked; 0 (everything) is what cutscenes use, so it's only cleared by "Unlock everything now".
		for (int i = 1; i < LOCK_COUNT; i++)
			ClearLock(player, i);
		g_toolStatus.Set(player[NAV_LOCKS] != 0 ? "Abilities unlocked (the story is holding you for a scene)" :
			"All abilities unlocked: boost, thrusters, tools");
	}

	static VOID EquipCutter() { ActivateTool(4, STATE_CUTTER, "cutter"); }
	static VOID EquipScanner() { ActivateTool(5, STATE_SCANNER, "scanner"); }
	static VOID EquipSnapshot() { ActivateTool(6, STATE_SNAPSHOT, "snapshot camera"); }
	static VOID PutAway() { ActivateTool(0, 0, "tools put away"); }
	static int ToolsActive(int i)
	{
		static const int types[] = { -2, 4, 5, 6, 0 };
		return i >= 1 && i < 5 && types[i] == g_toolChoice;
	}
	static const char* ToolsStatus() { return g_toolStatus.Get(); }

	// ==== Immunity ====
	// Every player death goes through Kill(navCS, index, cause, bodyPart, ...) (0x1405154d0; the deferred path queues
	// the same call). cause indexes the damage-type names at 0x1414c82a0 ("radiation", "crushing", "electrical"...),
	// read with 0x14008d1f0. Immunity skips Kill for Jack: for every cause, or only for radiation.
	static const DWORD KILL = 0x5154D0, CAUSE_NAMES = 0x14C82A0, SYMBOL_NAME = 0x8D1F0;
	typedef UINT64(__fastcall* KillFn)(BYTE* cs, UINT64 index, UINT64 cause, UINT64 part, UINT64 a5, UINT64 a6, UINT64 a7, UINT64 a8);
	typedef const CHAR* (__fastcall* SymbolNameFn)(VOID* symbol);
	static KillFn g_originalKill = NULL;
	static volatile LONG g_immuneOn = 0, g_noDeath = 1, g_noRadiation = 1, g_blocked = 0;
	static StatusText g_immunityStatus;
	static CHAR g_lastCause[48] = "";

	static const CHAR* CauseName(UINT64 cause)
	{
		if (cause > 64)
			return "?";
		__try
		{
			const CHAR* name = ((SymbolNameFn)(g_exe + SYMBOL_NAME))(g_exe + CAUSE_NAMES + cause * 8);
			return name != NULL ? name : "?";
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return "?"; }
	}

	static UINT64 __fastcall HookedKill(BYTE* cs, UINT64 index, UINT64 cause, UINT64 part, UINT64 a5, UINT64 a6, UINT64 a7, UINT64 a8)
	{
		if (g_immuneOn && cs == PlayerNav())
		{
			const CHAR* name = CauseName((UINT32)cause);
			BOOL radiation = strstr(name, "radiation") != NULL;
			if (g_noDeath || (g_noRadiation && radiation))
			{
				InterlockedIncrement(&g_blocked);
				strncpy_s(g_lastCause, name, _TRUNCATE);
				g_log("[MODS] Immunity: blocked a death (cause %u: %s)", (UINT32)cause, name);
				return 0;
			}
		}
		return g_originalKill(cs, index, cause, part, a5, a6, a7, a8);
	}

	static VOID UpdateImmunityStatus()
	{
		const CHAR* what = g_noDeath ? "Can't die" : (g_noRadiation ? "Radiation can't kill you" : "Nothing blocked (pick an option)");
		if (g_blocked > 0)
			g_immunityStatus.Set("%s  -  %ld deaths blocked (last: %s)", what, g_blocked, g_lastCause);
		else
			g_immunityStatus.Set("%s", what);
	}

	static VOID ImmunityEnable()
	{
		if (g_originalKill == NULL)
		{
			g_originalKill = (KillFn)(g_exe + KILL);
			Attach((VOID**)&g_originalKill, (VOID*)HookedKill, "player death");
		}
		InterlockedExchange(&g_immuneOn, 1);
		UpdateImmunityStatus();
	}
	static VOID ImmunityDisable() { InterlockedExchange(&g_immuneOn, 0); }
	static VOID ImmunityFrame(void*) { UpdateImmunityStatus(); }
	static VOID ToggleNoDeath() { InterlockedExchange(&g_noDeath, !g_noDeath); UpdateImmunityStatus(); }
	static VOID ToggleNoRadiation() { InterlockedExchange(&g_noRadiation, !g_noRadiation); UpdateImmunityStatus(); }
	static int ImmunityActive(int i) { return i == 0 ? g_noDeath != 0 : (i == 1 ? g_noRadiation != 0 : 0); }
	static const char* ImmunityStatus() { return g_immunityStatus.Get(); }

	// ==== Time ====
	static FLOAT g_timeChoice = 0.5f;
	static StatusText g_timeStatus;
	static VOID TimeSet(FLOAT scale)
	{
		g_timeChoice = scale;
		SetTimeScale(scale);
		g_timeStatus.Set("Game speed %gx", scale);
		g_log("[MODS] Time: %.2fx", scale);
	}
	static VOID TimeEnable() { TimeSet(g_timeChoice); }
	static VOID TimeDisable() { ResetTimeScale(); }
	static VOID Time01() { TimeSet(0.1f); }
	static VOID Time025() { TimeSet(0.25f); }
	static VOID Time05() { TimeSet(0.5f); }
	static VOID Time1() { TimeSet(1.0f); }
	static VOID Time2() { TimeSet(2.0f); }
	static VOID Time4() { TimeSet(4.0f); }
	static int TimeActive(int i)
	{
		static const FLOAT scales[] = { 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f };
		return i < 6 && scales[i] == g_timeChoice;
	}
	static const char* TimeStatus() { return g_timeStatus.Get(); }

	// ==== Freeze ====
	// Loose objects: pinned where they were (put back, still, every frame). Characters (robots, people): their level's
	// animation systems stop (every character in that level freezes).
	static const DWORD RUN_JOB = 0x2D6A80;  // every per-frame job: (task, data); data+8 -> job entry, first field the CS
	static const UINT64 HASH_PHYSICS = 0x5B8CC538E22AD937ULL;
	typedef UINT64(__fastcall* RunJobFn)(BYTE* task, BYTE* data, UINT64 a3, UINT64 a4);
	static RunJobFn g_originalRunJob = NULL;
	struct Pinned { void* body; FLOAT pos[3]; FLOAT rot[4]; };
	static std::vector<Pinned> g_pinned;
	static std::vector<BYTE*> g_frozenSpaces;
	static StatusText g_freezeStatus;

	static BOOL IsAnimationSystem(BYTE* cs)
	{
		static BYTE* known[512]; static BOOL anim[512]; static LONG count = 0;
		BYTE* vtable = *(BYTE**)cs;
		for (LONG i = 0; i < count; i++)
			if (known[i] == vtable)
				return anim[i];
		const CHAR* name = ClassOf(cs);
		BOOL a = name != NULL && (strstr(name, "CCharacterAnimationCS@") != NULL || strstr(name, "CAnimationCS@") != NULL);
		if (count < ARRAYSIZE(known))
		{
			known[count] = vtable; anim[count] = a; count++;
		}
		return a;
	}

	static UINT64 __fastcall FilteredRunJob(BYTE* task, BYTE* data, UINT64 a3, UINT64 a4)
	{
		if (!g_frozenSpaces.empty())
		{
			BYTE* cs = NULL;
			__try { cs = **(BYTE***)(data + 8); }
			__except (EXCEPTION_EXECUTE_HANDLER) { cs = NULL; }
			if (cs != NULL)
			{
				BYTE* space = NULL;
				__try { space = *(BYTE**)(cs + 0x80); }
				__except (EXCEPTION_EXECUTE_HANDLER) { space = NULL; }
				for (BYTE* frozen : g_frozenSpaces)
					if (frozen == space && IsAnimationSystem(cs))
						return 0;
			}
		}
		return g_originalRunJob(task, data, a3, a4);
	}

	static VOID UpdateFreezeStatus()
	{
		g_freezeStatus.Set("%zu objects frozen, animations stopped in %zu levels", g_pinned.size(), g_frozenSpaces.size());
	}

	static VOID FreezeObject()
	{
		LeModTransform hand = {};
		if (!g_api.hand(1, &hand))
			return;
		void* body = g_api.bodyNear(hand.position, 0.6f);
		if (body == NULL)
		{
			g_freezeStatus.Set("No loose object within 0.6 m of your right hand");
			return;
		}
		for (Pinned& p : g_pinned)
			if (p.body == body)
				return;
		Pinned p = { body };
		g_api.bodyGet(body, p.pos, p.rot, NULL);
		g_pinned.push_back(p);
		UpdateFreezeStatus();
	}

	static BYTE* SafeBody(BYTE* physics, UINT16 i)
	{
		__try { return ResolveBody(physics, i); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
	}

	/// The level of the nearest physics body within 3 m of the right hand.
	static VOID FreezeAnimations()
	{
		Trs hand = {};
		if (!Hand(1, &hand))
			return;
		BYTE* global = GlobalSpace();
		BYTE* bestSpace = NULL;
		FLOAT best = 3.0f;
		ForEachSpace([&](BYTE* space) {
			if (space == global)
				return;
			BYTE* physics = FindSystem(space, HASH_PHYSICS);
			UINT16 n = physics == NULL ? 0 : *(UINT16*)(physics + 0xE4);
			for (UINT16 i = 0; i < n; i++)
			{
				BYTE* body = SafeBody(physics, i);
				if (body == NULL)
					continue;
				FLOAT d = Distance((FLOAT*)(body + 0x938), hand.pos);
				if (d < best)
				{
					best = d;
					bestSpace = space;
				}
			}
		});
		if (bestSpace == NULL)
		{
			g_freezeStatus.Set("Nothing within 3 m of your right hand");
			return;
		}
		if (g_originalRunJob == NULL)
		{
			g_originalRunJob = (RunJobFn)(g_exe + RUN_JOB);
			Attach((VOID**)&g_originalRunJob, (VOID*)FilteredRunJob, "job filter");
		}
		if (std::find(g_frozenSpaces.begin(), g_frozenSpaces.end(), bestSpace) == g_frozenSpaces.end())
			g_frozenSpaces.push_back(bestSpace);
		UpdateFreezeStatus();
	}

	static VOID UnfreezeAll()
	{
		g_pinned.clear();
		g_frozenSpaces.clear();
		UpdateFreezeStatus();
	}

	static VOID FreezeFrame(void*)
	{
		const FLOAT still[3] = { 0, 0, 0 };
		for (Pinned& p : g_pinned)
		{
			__try { g_api.bodySet(p.body, p.pos, p.rot, still); }
			__except (EXCEPTION_EXECUTE_HANDLER) { p.body = NULL; }
		}
		size_t before = g_pinned.size();
		g_pinned.erase(std::remove_if(g_pinned.begin(), g_pinned.end(), [](const Pinned& p) { return p.body == NULL; }), g_pinned.end());
		if (g_pinned.size() != before)
			UpdateFreezeStatus();
	}
	static const char* FreezeStatus() { return g_freezeStatus.Get(); }

	// ==== Registration ====
	static VOID Add(const LeMod& mod) { Mods().push_back({ mod, 0, "built in" }); }

	VOID RegisterBuiltIns()
	{
		g_noclipStatus.Set("Off");
		g_doorStatus.Set("Switch on to list every button");
		g_boostStatus.Set("Off");
		g_toolStatus.Set("Off");
		g_timeStatus.Set("Normal speed");
		UpdateFreezeStatus();
		UpdateImmunityStatus();

		LeMod noclip = {};
		noclip.name = "No clip";
		noclip.description = "Fly through walls, floors and everything else. Move with your hand thrusters (or boost); "
			"pick how fast the thrusters push you.";
		noclip.enable = NoclipEnable;
		noclip.disable = NoclipDisable;
		noclip.frame = NoclipFrame;
		noclip.buttons[0] = { "Thrusters 1x", Fly1 };
		noclip.buttons[1] = { "Thrusters 4x", Fly4 };
		noclip.buttons[2] = { "Thrusters 10x", Fly10 };
		noclip.buttons[3] = { "Thrusters 25x", Fly25 };
		noclip.buttons[4] = { "Stop moving", NoclipStop };
		noclip.buttonActive = NoclipActive;
		noclip.status = NoclipStatus;
		Add(noclip);

		LeMod doors = {};
		doors.name = "Doors & buttons";
		doors.description = "Every button in the loaded levels (door panels, airlocks, consoles), nearest first. "
			"Pressing one here does exactly what pressing it by hand does, so doors open from anywhere.";
		doors.enable = DoorsEnable;
		doors.frame = DoorsFrame;
		doors.buttons[0] = { "Refresh list", DoorsRefresh };
		doors.buttons[1] = { "Press nearest", DoorsPressNearest };
		doors.buttons[2] = { "Press all within 6 m", DoorsPressNearby };
		doors.listCount = DoorsCount;
		doors.listItem = DoorsItem;
		doors.listAction = "Press";
		doors.listActivate = DoorsActivate;
		doors.listTitle = "Buttons (double-click to press)";
		doors.status = DoorsStatus;
		Add(doors);

		LeMod boosts = {};
		boosts.name = "Boosts";
		boosts.description = "Infinite, faster hand thrusters and a bigger back boost with no cooldown. Also unlocks the "
			"boost if the story hasn't given it to you yet.";
		boosts.enable = BoostsEnable;
		boosts.disable = BoostsDisable;
		boosts.frame = BoostsFrame;
		boosts.buttons[0] = { "Hands 1x", Hand1 };
		boosts.buttons[1] = { "Hands 3x", Hand3 };
		boosts.buttons[2] = { "Hands 6x", Hand6 };
		boosts.buttons[3] = { "Hands 12x", Hand12 };
		boosts.buttons[4] = { "Boost normal", BoostNormal };
		boosts.buttons[5] = { "Boost big", BoostBig };
		boosts.buttons[6] = { "Boost huge", BoostHuge };
		boosts.buttonActive = BoostsActive;
		boosts.status = BoostsStatus;
		Add(boosts);

		LeMod tools = {};
		tools.name = "Tools";
		tools.description = "Unlocks every ability the story hands out later (back boost, thrusters, tools) and lets you "
			"take out the cutter, scanner or snapshot camera whenever you like.";
		tools.frame = ToolsFrame;
		tools.buttons[0] = { "Unlock everything now", UnlockAll };
		tools.buttons[1] = { "Take out cutter", EquipCutter };
		tools.buttons[2] = { "Take out scanner", EquipScanner };
		tools.buttons[3] = { "Take out snapshot camera", EquipSnapshot };
		tools.buttons[4] = { "Put tools away", PutAway };
		tools.buttonActive = ToolsActive;
		tools.status = ToolsStatus;
		Add(tools);

		LeMod immunity = {};
		immunity.name = "Immunity";
		immunity.description = "Stop dying. Two options, each switched on or off by its button: no deaths at all (crushing, "
			"electrical, everything), or only no radiation deaths.";
		immunity.enable = ImmunityEnable;
		immunity.disable = ImmunityDisable;
		immunity.frame = ImmunityFrame;
		immunity.buttons[0] = { "No death (anything)", ToggleNoDeath };
		immunity.buttons[1] = { "No radiation death", ToggleNoRadiation };
		immunity.buttonActive = ImmunityActive;
		immunity.status = ImmunityStatus;
		Add(immunity);

		LeMod time = {};
		time.name = "Time";
		time.description = "Slow motion or fast forward for the whole game. Your head and hands stay real-time.";
		time.enable = TimeEnable;
		time.disable = TimeDisable;
		time.buttons[0] = { "0.1x", Time01 };
		time.buttons[1] = { "0.25x", Time025 };
		time.buttons[2] = { "0.5x", Time05 };
		time.buttons[3] = { "1x", Time1 };
		time.buttons[4] = { "2x", Time2 };
		time.buttons[5] = { "4x", Time4 };
		time.buttonActive = TimeActive;
		time.status = TimeStatus;
		Add(time);

		LeMod freeze = {};
		freeze.name = "Freeze";
		freeze.description = "Freeze the loose object at your right hand in mid-air, or stop the animations of the robots "
			"and people near your right hand.";
		freeze.disable = UnfreezeAll;
		freeze.frame = FreezeFrame;
		freeze.buttons[0] = { "Freeze object at right hand", FreezeObject };
		freeze.buttons[1] = { "Freeze animations near hand", FreezeAnimations };
		freeze.buttons[2] = { "Unfreeze all", UnfreezeAll };
		freeze.status = FreezeStatus;
		Add(freeze);
	}
}
