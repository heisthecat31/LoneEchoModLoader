#pragma once
/*
 * Lone Echo mod API (loneecho.exe: the March 2019 and April 2020 builds).
 *
 * A mod is a DLL in Lone Echo\bin\win7\mods\ that exports:
 *
 *     extern "C" __declspec(dllexport) int LeModRegister(const LeModApi* api, LeMod* mods, int capacity);
 *
 * Fill in up to `capacity` LeMod entries and return how many you filled. The loader calls it once at start-up.
 * Mods show up in the Lone Echo Mods window, where players switch them on and off and use their buttons and list.
 *
 * Threads: enable, disable, frame, button presses and list actions run on the game thread, so they may touch game
 * memory. status, buttonActive, listCount and listItem are called by the window (another thread, several times a
 * second): return cached values from them and don't touch the game there.
 *
 * Addresses: loneecho.exe is relocated at run time; api->exe is its base, so a static address A (as in IDA/Ghidra
 * with image base 0x140000000) is api->exe + (A - 0x140000000). Static addresses differ between builds: check
 * api->exeTimestamp (0x5C9D6E49 March 2019, 0x5E87A5F2 April 2020) before using your own.
 */
#include <stdint.h>

#define LEMOD_API_VERSION 3
#define LEMOD_MAX_BUTTONS 12

typedef struct LeModTransform
{
    float rotation[4];  /* quaternion x, y, z, w */
    float position[3];
    float scale[3];
    uint8_t reserved[8];
} LeModTransform;

typedef struct LeModApi
{
    int version;                                   /* LEMOD_API_VERSION */
    uint8_t* exe;                                  /* loneecho.exe's base address */
    void (*log)(const char* format, ...);          /* a line in bin\win7\lemods.log */
    void* (*game)(void);                           /* the CR14Game object, or 0 before the game runs */

    /* Jack (world space). Each returns 0 if there's no player yet. hand: 0 left, 1 right. */
    int (*head)(LeModTransform* out);
    int (*hand)(int hand, LeModTransform* out);

    /* Loose objects (rigid bodies). Handles stay valid while their level is loaded. */
    void* (*bodyNear)(const float position[3], float radius);   /* nearest rigid body outside Jack's own level */
    int (*bodyGet)(void* body, float position[3], float rotation[4], float velocity[3]);
    void (*bodySet)(void* body, const float position[3], const float rotation[4], const float velocity[3]);

    /* Game time. 1 is normal speed. */
    float (*timeScale)(void);
    void (*setTimeScale)(float scale);

    /* Redraws the mod window (after a mod's list or status changed). Any thread. */
    void (*refreshWindow)(void);

    /* ---- version 2 ---- */
    uint8_t* (*player)(void);                      /* Jack's player record (R14PlayerNav entry, 0x988 bytes), or 0 */
    int (*spaceCount)(void);                       /* loaded game spaces (levels) */
    void* (*space)(int index);
    void* (*findSystem)(void* space, uint64_t typeHash);   /* a space's engine component system, or 0 */
    void (*runOnGameThread)(void (*fn)(void* arg), void* arg);  /* from any thread; runs at the next frame */

    /* ---- version 3 ---- */
    uint32_t exeTimestamp;                         /* loneecho.exe's PE timestamp: which build is running */
} LeModApi;

typedef struct LeModButton
{
    const char* label;
    void (*pressed)(void);
} LeModButton;

typedef struct LeMod
{
    const char* name;
    const char* description;
    void (*enable)(void);              /* optional */
    void (*disable)(void);             /* optional */
    void (*frame)(void* game);         /* optional: every frame while enabled */
    LeModButton buttons[LEMOD_MAX_BUTTONS];  /* optional: shown while enabled; unused entries have label 0 */
    /* Optional list (for example the doors): count, item text, and an action on the selected item. */
    int (*listCount)(void);
    const char* (*listItem)(int index);
    const char* listAction;            /* the action button's label */
    void (*listActivate)(int index);
    /* ---- version 2 (all optional) ---- */
    int (*buttonActive)(int index);    /* nonzero: draw that button as the current choice */
    const char* (*status)(void);       /* a short live status line shown under the mod's name */
    const char* listTitle;             /* heading over the list */
} LeMod;

typedef int (*LeModRegisterFn)(const LeModApi* api, LeMod* mods, int capacity);
