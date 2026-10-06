// Lone Echo Mod Loader: built as dinput8.dll and put next to loneecho.exe (Lone Echo\bin\win7). loneecho.exe imports
// DirectInput8Create from dinput8.dll, and Windows looks in the exe's folder first, so this DLL loads with the game.
// Its exports go to the real dinput8.dll in System32; once it's loaded into loneecho.exe it starts the mod loader.
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <share.h>
#include "lemods.h"

static HMODULE g_real = NULL;
static FILE* g_log = NULL;

static VOID Log(const CHAR* format, ...)
{
	if (g_log == NULL)
	{
		CHAR path[MAX_PATH];
		GetModuleFileNameA(NULL, path, MAX_PATH);
		CHAR* slash = strrchr(path, '\\');
		if (slash != NULL)
			*(slash + 1) = 0;
		strcat_s(path, "lemods.log");
		g_log = _fsopen(path, "a", _SH_DENYNO);
		if (g_log == NULL)
			return;
	}
	SYSTEMTIME t;
	GetLocalTime(&t);
	fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
	va_list args;
	va_start(args, format);
	vfprintf(g_log, format, args);
	va_end(args);
	fputc('\n', g_log);
	fflush(g_log);
}

/// The real dinput8.dll (System32), loaded the first time one of its functions is called.
static FARPROC Real(const CHAR* name)
{
	if (g_real == NULL)
	{
		CHAR path[MAX_PATH];
		GetSystemDirectoryA(path, MAX_PATH);
		strcat_s(path, "\\dinput8.dll");
		g_real = LoadLibraryA(path);
	}
	return g_real == NULL ? NULL : GetProcAddress(g_real, name);
}

extern "C"
{
	HRESULT WINAPI ForwardDirectInput8Create(HINSTANCE instance, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer)
	{
		typedef HRESULT(WINAPI* Fn)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
		Fn f = (Fn)Real("DirectInput8Create");
		return f == NULL ? E_FAIL : f(instance, version, riid, out, outer);
	}
	HRESULT WINAPI ForwardDllCanUnloadNow()
	{
		typedef HRESULT(WINAPI* Fn)();
		Fn f = (Fn)Real("DllCanUnloadNow");
		return f == NULL ? S_FALSE : f();
	}
	HRESULT WINAPI ForwardDllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out)
	{
		typedef HRESULT(WINAPI* Fn)(REFCLSID, REFIID, LPVOID*);
		Fn f = (Fn)Real("DllGetClassObject");
		return f == NULL ? E_FAIL : f(clsid, riid, out);
	}
	HRESULT WINAPI ForwardDllRegisterServer()
	{
		typedef HRESULT(WINAPI* Fn)();
		Fn f = (Fn)Real("DllRegisterServer");
		return f == NULL ? E_FAIL : f();
	}
	HRESULT WINAPI ForwardDllUnregisterServer()
	{
		typedef HRESULT(WINAPI* Fn)();
		Fn f = (Fn)Real("DllUnregisterServer");
		return f == NULL ? E_FAIL : f();
	}
	LPCVOID WINAPI ForwardGetdfDIJoystick()
	{
		typedef LPCVOID(WINAPI* Fn)();
		Fn f = (Fn)Real("GetdfDIJoystick");
		return f == NULL ? NULL : f();
	}
}

static DWORD ExeTimestamp(BYTE* exe)
{
	IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)exe;
	IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(exe + dos->e_lfanew);
	return nt->FileHeader.TimeDateStamp;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason != DLL_PROCESS_ATTACH)
		return TRUE;
	DisableThreadLibraryCalls(module);
	BYTE* exe = (BYTE*)GetModuleHandleA(NULL);
	CHAR name[MAX_PATH];
	GetModuleFileNameA(NULL, name, MAX_PATH);
	const CHAR* file = strrchr(name, '\\');
	file = file != NULL ? file + 1 : name;
	if (_stricmp(file, "loneecho.exe") != 0)
		return TRUE;  // some other program loaded this dinput8.dll: just pass DirectInput through
	DWORD timestamp = ExeTimestamp(exe);
	const CHAR* build = LeMods::BuildName(timestamp);
	if (build == NULL)
	{
		Log("Lone Echo Mod Loader: unsupported loneecho.exe build (timestamp %08lX; supported: March 2019 5C9D6E49, "
			"April 2020 5E87A5F2); mods are off", timestamp);
		return TRUE;
	}
	// A dedicated server (EchoRelay's -mp) has no player: no mods there.
	if (wcsstr(GetCommandLineW(), L" -mp") != NULL || wcsstr(GetCommandLineW(), L"-nomods") != NULL)
		return TRUE;
	Log("Lone Echo Mod Loader starting (loneecho.exe %s build)", build);
	LeMods::Install(exe, timestamp, Log);
	return TRUE;
}
