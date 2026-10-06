# Lone Echo Mod Loader

A mod loader for **Lone Echo** (PC VR). It installs as `dinput8.dll` next to the game, opens a **Lone Echo Mods**
window on your desktop while you play, and comes with a set of built-in mods. You can also add your own as DLLs.

## Required game version

The mod loader only works with **one build of `loneecho.exe`**: the **March 2019** build. Every game address it uses
was found in that exe, so it switches itself off on any other build.

| | |
|---|---|
| File | `Lone Echo\bin\win7\loneecho.exe` |
| PE timestamp | `0x5C9D6E49` (built 29 March 2019, 01:00:57 UTC) |
| Size | 22,336,512 bytes |
| SHA-256 | `d7497a6c74a7f83117e5e0b3d48f9ae56b64a3fa00b788f3a817f25a26f47775` |

To check your copy, start the game with the mod loader installed and open `bin\win7\lemods.log`:

- `Lone Echo Mod Loader starting` means your build is supported.
- `this loneecho.exe isn't the March 2019 build (timestamp ...); mods are off` means it isn't. The game still runs
  normally, just without mods.

Mods are also off on an EchoRelay dedicated server (`-mp` on the command line) and when you start the game with
`-nomods`.

## Install

1. Build the project (see below) or get a built `dinput8.dll`.
2. Copy `dinput8.dll` into `Lone Echo\bin\win7\`, next to `loneecho.exe`. If a `dinput8.dll` is already there, back it
   up first.
3. Start Lone Echo. The **Lone Echo Mods** window opens on the desktop.

To uninstall, delete `dinput8.dll` from `bin\win7`.

How it loads: `loneecho.exe` imports `DirectInput8Create` from `dinput8.dll`, and Windows looks in the exe's folder
first. The mod loader passes every DirectInput call through to the real `dinput8.dll` in System32. If another program
loads it, it does nothing else.

## Built-in mods

Switch each one on in the mods window. Its buttons appear once it's on.

| Mod | What it does |
|---|---|
| **No clip** | Fly through walls, floors and everything else using your hand thrusters. Thruster speed 1x / 4x / 10x / 25x, plus *Stop moving*. |
| **Doors & buttons** | Lists every button in the loaded levels (door panels, airlocks, consoles), nearest first. Pressing one here does the same as pressing it by hand. |
| **Boosts** | Faster hand thrusters with no power cost, and a bigger back boost with no cooldown. Also unlocks the boost before the story gives it to you. |
| **Tools** | Unlocks every ability the story hands out later, and lets you take out the cutter, scanner or snapshot camera at any time. |
| **Immunity** | No deaths at all, or no radiation deaths only. Each option has its own on/off button. |
| **Time** | Game speed from 0.1x to 4x. Your head and hands stay real-time. |
| **Freeze** | Pin the loose object at your right hand in mid-air, or stop the animations of robots and people near your right hand. |

## Writing your own mods

A mod is a DLL in `Lone Echo\bin\win7\mods\` that exports:

```c
extern "C" __declspec(dllexport) int LeModRegister(const LeModApi* api, LeMod* mods, int capacity);
```

Include [`lemod_api.h`](LoneEchoModLoader/lemod_api.h). Fill in up to `capacity` `LeMod` entries (name, description,
enable/disable/frame callbacks, up to 12 buttons, an optional list and a status line) and return how many you filled.
The API gives you Jack's head and hands, his player record, rigid bodies, game time, the game's levels and component
systems, and a way to run code on the game thread. The header explains which callbacks run on which thread.

Game addresses in the API use image base `0x140000000`, so a static address `A` is `api->exe + (A - 0x140000000)`.

## Building

- Visual Studio 2026 (platform toolset v145) with the **Desktop development with C++** workload
- Windows 10 SDK
- [Microsoft Detours](https://github.com/microsoft/Detours) 4.0.1, from NuGet (`packages.config`). Visual Studio
  restores it on the first build.

Open `LoneEchoModLoader.sln`, pick **Release | x64** and build. The output is `dinput8.dll`.

## Log

Everything the mod loader does (start-up, hooks it installs, mods switched on and off, buttons pressed) goes to
`Lone Echo\bin\win7\lemods.log`. Check it first if something doesn't work.
