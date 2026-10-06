#pragma once
// Shared inside the mod loader: the core (lemods.cpp), the built-in mods (builtin_mods.cpp) and the window (ui.cpp).
#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include "lemod_api.h"

namespace LeMods
{
	extern BYTE* g_exe;
	extern VOID(*g_log)(const CHAR* format, ...);
	extern LeModApi g_api;

	/// Static addresses in one build of loneecho.exe, as RVAs (VA - 0x140000000). The builds differ only in where
	/// things are; the code and the data layouts the loader uses are the same.
	struct GameBuild
	{
		DWORD timestamp;          // PE header TimeDateStamp
		const CHAR* name;
		DWORD gameVtable;         // CR14Game (slot 0x140: Update(game, phase))
		DWORD frameTimerVtable;   // CClock (slot 1: Tick(timer); timer+0x38 = time scale)
		DWORD findSpace;          // FindGameSpace(game, levelHash)
		DWORD findEngineSystem;   // EngineComponentSystem(space, typeHash)
		DWORD navHead, navHand;   // player nav: head / hand world transforms
		DWORD globalLevel;        // UINT64 hash of r14_glb_global (Jack's level)
		DWORD physicsHandle, resolveBody, setBody;
		DWORD movementTuning;     // the 9 movement floats (boost accel ... boost recharge time)
		DWORD controllerStep, controllerSettle, controllerReadBack, bodyOrigin;
		DWORD pressStarted, press, fullyDepressed;  // button events (cs, componentIndex)
		DWORD activateTool;       // tool ability manager: (manager, component, type, state)
		DWORD kill, causeNames, symbolName;
		DWORD runJob;             // every per-frame job: (task, data)
	};
	extern const GameBuild* g_build;

	// ---- Engine layouts (see J:\LE1store\coop_re_notes.md) ----
	struct Trs { FLOAT rot[4]; FLOAT pos[3]; FLOAT scale[3]; BYTE pad[8]; };
	static const SIZE_T NAV_ENTRY_SIZE = 0x988;

	BYTE* GlobalSpace();                 // Jack's level (r14_glb_global)
	BYTE* PlayerNav();                   // its R14PlayerNav component system, or NULL
	BYTE* PlayerEntry();                 // Jack's R14PlayerNav record, or NULL
	BYTE* FindSystem(BYTE* space, UINT64 typeHash);
	VOID ForEachSpace(const std::function<VOID(BYTE* space)>& visit);
	const CHAR* ClassOf(BYTE* object);   // RTTI class name, or NULL
	BYTE* ResolveBody(BYTE* physics, UINT16 index);
	UINT64 SpaceLevel(BYTE* space);      // the level's hash
	BOOL Head(Trs* out);
	BOOL Hand(int hand, Trs* out);

	// ---- Mods ----
	struct LoadedMod
	{
		LeMod mod;
		volatile LONG enabled;
		std::string source;  // "built in" or the DLL's file name
	};
	std::vector<LoadedMod>& Mods();
	VOID RunOnGameThread(std::function<VOID()> work);
	VOID RequestEnable(size_t index, BOOL enabled);   // any thread
	VOID RequestButton(size_t index, int button);     // any thread
	VOID RequestListAction(size_t index, int item);   // any thread
	VOID RefreshWindow();                             // any thread

	// Guarded calls into mods for the window thread.
	int SafeCount(int (*count)(void));
	const CHAR* SafeItem(const char* (*item)(int), int index);
	const CHAR* SafeStatus(const char* (*status)(void));
	int SafeActive(int (*active)(int), int index);

	VOID RegisterBuiltIns();             // builtin_mods.cpp
	VOID StartWindow(VOID(*beforeShow)());  // ui.cpp: the window thread; beforeShow runs on it first
	VOID SetTimeScale(FLOAT scale);      // lemods.cpp
	VOID ResetTimeScale();
	FLOAT TimeScale();
}
