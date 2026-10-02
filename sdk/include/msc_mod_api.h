/*
 * Strikers-WiiCompiled native plugin API.
 *
 * A mod package may ship a native plugin ([plugin] in mod.toml). The runtime loads it at launch,
 * once the player has allowed that mod's native code on the F10 Mods page, and calls its
 * msc_mod_init with this API. Everything a plugin does to the game goes through the function table
 * below, so a plugin built against this header keeps working with later runtimes: the table only
 * ever grows (check `size` before using a field added after MSC_MOD_API_VERSION 1).
 *
 * Threading: init runs on the main thread before the game starts. Hooks and frame events run on
 * the game's thread, with the game paused around them; keep them short.
 *
 * Guest addresses are the game's own (0x80000000.. for MEM1, 0x90000000.. for MEM2). Values in guest
 * memory are big-endian; the read and write functions convert. An invalid address is reported to
 * the log and reads return 0, rather than crashing the game.
 *
 * Docs: docs/modding/plugins.md.
 */
#ifndef MSC_MOD_API_H
#define MSC_MOD_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MSC_MOD_API_VERSION 1

#if defined(_WIN32)
#define MSC_MOD_EXPORT __declspec(dllexport)
#else
#define MSC_MOD_EXPORT __attribute__((visibility("default")))
#endif

typedef struct MscMod MscMod; /* this plugin's handle, passed back to the runtime */
typedef struct MscCpu MscCpu; /* a guest CPU register file (inside hooks and events) */

typedef enum MscLogLevel {
    MSC_LOG_INFO = 0,
    MSC_LOG_WARNING = 1,
    MSC_LOG_ERROR = 2
} MscLogLevel;

typedef enum MscEvent {
    MSC_EVENT_FRAME = 1 /* once per game frame */
} MscEvent;

/* An event callback. `cpu` is a private register file the callback may use for guest calls. */
typedef void (*MscEventFn)(MscCpu* cpu, void* user);

/*
 * A hook: runs instead of the guest function at its address, with the function's arguments in
 * r3.. (f1.. for floats). Call `call_original(cpu)` to run the original (or the next plugin's hook);
 * leave results in r3 / f1. Not calling it skips the original.
 */
typedef void (*MscHookFn)(MscCpu* cpu, void* user);

typedef struct MscModApi {
    uint32_t version; /* MSC_MOD_API_VERSION of the running runtime */
    uint32_t size;    /* sizeof(MscModApi) in the running runtime */

    /* --- Logging (console.log, prefixed with the mod id) --- */
    void (*log)(MscMod* mod, MscLogLevel level, const char* message);

    /* --- Guest memory --- */
    int (*is_valid)(uint32_t address, uint32_t length); /* 1 if the whole range is mapped */
    uint8_t (*read8)(uint32_t address);
    uint16_t (*read16)(uint32_t address);
    uint32_t (*read32)(uint32_t address);
    float (*read_f32)(uint32_t address);
    void (*write8)(uint32_t address, uint8_t value);
    void (*write16)(uint32_t address, uint16_t value);
    void (*write32)(uint32_t address, uint32_t value);
    void (*write_f32)(uint32_t address, float value);
    void (*read_bytes)(uint32_t address, void* out, uint32_t length);        /* raw bytes, no swapping */
    /* Raw bytes, no swapping. Unlike the scalar writes (which behave like the game's own stores),
       this also tells the renderer the range changed: use it to rewrite textures or display lists. */
    void (*write_bytes)(uint32_t address, const void* data, uint32_t length);
    /* A block of the game's own heap (nlMalloc), kept for the session. 0 on failure. */
    uint32_t (*alloc)(MscCpu* cpu, uint32_t size, uint32_t alignment);

    /* --- Registers --- */
    uint32_t (*get_gpr)(MscCpu* cpu, int index); /* r0-r31 */
    void (*set_gpr)(MscCpu* cpu, int index, uint32_t value);
    double (*get_fpr)(MscCpu* cpu, int index);   /* f0-f31 */
    void (*set_fpr)(MscCpu* cpu, int index, double value);

    /* --- Calling the game --- */
    /* Calls the guest function at `address` with up to 8 integer arguments (r3..r10); returns r3.
       Float arguments: set f1.. with set_fpr first. */
    uint32_t (*call)(MscCpu* cpu, uint32_t address, const uint32_t* args, int count);

    /* --- Hooks (only during msc_mod_init) --- */
    /* 1 if `address` can be hooked in this build (see the list in docs/modding/plugins.md). */
    int (*is_hookable)(uint32_t address);
    /* Hooks the guest function at `address`. 0 on success, -1 if it isn't hookable or init is over.
       Several plugins may hook one function: the mod loaded later runs first. */
    int (*hook)(MscMod* mod, uint32_t address, MscHookFn fn, void* user);
    /* Inside a hook: runs the original function (or the next hook) with the current registers. */
    void (*call_original)(MscCpu* cpu);

    /* --- Events (only during msc_mod_init) --- */
    int (*subscribe)(MscMod* mod, MscEvent event, MscEventFn fn, void* user);

    /* --- Settings ---
       Shown under the mod on the F10 Mods page and saved in Config.toml. Declare them in
       msc_mod_init; reading the value is cheap enough to do every frame. */
    void (*declare_bool)(MscMod* mod, const char* key, const char* label, const char* help, int default_value);
    int (*get_bool)(MscMod* mod, const char* key);

    /* --- Mod package --- */
    const char* (*mod_id)(MscMod* mod);
    const char* (*mod_directory)(MscMod* mod); /* the package folder, UTF-8 */
} MscModApi;

/*
 * The plugin's entry points. msc_mod_init must be exported; return 0 on success, anything else
 * to report a failure (the plugin stays loaded but its hooks and events are dropped).
 */
typedef int (*MscModInitFn)(const MscModApi* api, MscMod* mod);
#define MSC_MOD_INIT_SYMBOL "msc_mod_init"

#ifdef __cplusplus
}
#endif

#endif /* MSC_MOD_API_H */
