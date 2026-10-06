#pragma once
#include <windows.h>

/// <summary>
/// Lone Echo mod loader (loneecho.exe, single-player). Runs the built-in mods and the DLL mods in bin\win7\mods\
/// (see lemod_api.h) on the game thread, and opens the mod window where they're switched on and off.
/// </summary>
namespace LeMods
{
	/// The name of the loneecho.exe build with this PE timestamp ("March 2019", "April 2020"), or NULL if the mod loader
	/// has no addresses for it.
	const CHAR* BuildName(DWORD exeTimestamp);

	/// Installs the loader. Call once, for a supported build (BuildName isn't NULL) when it isn't a server.
	VOID Install(BYTE* exe, DWORD exeTimestamp, VOID(*log)(const CHAR* format, ...));
}
