#pragma once
#include <windows.h>

/// <summary>
/// Lone Echo mod loader (loneecho.exe, single-player). Runs the built-in mods and the DLL mods in bin\win7\mods\
/// (see lemod_api.h) on the game thread, and opens the mod window where they're switched on and off.
/// </summary>
namespace LeMods
{
	/// Installs the loader. Call once, from the rad14 patch set, for the Lone Echo build when it isn't a server.
	VOID Install(BYTE* exe, VOID(*log)(const CHAR* format, ...));
}
