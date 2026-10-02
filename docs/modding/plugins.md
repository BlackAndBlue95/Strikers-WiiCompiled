# Native plugins

A plugin is a shared library a mod package ships for what files can't do: reacting to the game
frame by frame, changing what a game function does, adding settings. It is ordinary native code
running inside the game's process with the player's permissions, so:

- the runtime only loads it once the player allows that mod's native code on the F10 Mods page;
- players should only allow mods they trust, and you should publish your plugin's source.

Everything a plugin does to the game goes through one C header, [`sdk/include/msc_mod_api.h`](../../sdk/include/msc_mod_api.h).
Plugins don't link against the runtime and don't need its source.

## Building one

[`sdk/examples/hello`](../../sdk/examples/hello) is a complete plugin with a CMake file:

```sh
cmake -S sdk/examples/hello -B build-hello
cmake --build build-hello
```

That gives `hello.dylib` (macOS), `hello.dll` (Windows) or `hello.so` (Linux). Put it in the mod folder
next to `mod.toml`, which names the library per platform:

```toml
[plugin]
macos = "hello.dylib"
windows = "hello.dll"
linux = "hello.so"
```

Build each platform's library on (or for) that platform. Any language that can export a C function
works; the example is C.

## Lifecycle

The runtime calls the plugin's exported entry point once, on the main thread, before the game starts:

```c
MSC_MOD_EXPORT int msc_mod_init(const MscModApi* api, MscMod* mod);
```

Keep `api` and `mod`: every later call takes them. In `msc_mod_init` you declare settings, subscribe
to events and install hooks; those calls are refused after it returns. Return `0` for success. Any
other value is reported as a failure, and the runtime drops the hooks and events the plugin set up.

`api->version` is the runtime's API version and `api->size` the size of its function table. The table
only grows, so a plugin built for version 1 works with every later runtime. Before using a function
added in a later version, check that `api->size` covers it.

## Guest memory and calls

The game's addresses (`0x80000000…` for MEM1, `0x90000000…` for MEM2) are used as they are. The
`read*`/`write*` functions convert from the console's big-endian byte order; `read_bytes` and
`write_bytes` copy raw bytes. A bad address doesn't crash the game: it's logged, reads return 0 and
writes are skipped. `is_valid` checks a range first.

`call(cpu, address, args, count)` runs a game function with up to 8 integer arguments (`r3`…`r10`) and
returns `r3`. For float arguments, set `f1`… with `set_fpr` first. `alloc` takes memory from the game's
own heap (kept for the session). Both need a `cpu`, which events and hooks provide; neither works
during `msc_mod_init`, because the game hasn't started yet.

Addresses for this game are in the decompilation (`config/R4QE01/symbols.txt` in the Mario Strikers
Charged decomp); the runtime's mods code (`runtime/src/mods`) names the ones the framework uses.

## Events

```c
api->subscribe(mod, MSC_EVENT_FRAME, OnFrame, user);
```

`MSC_EVENT_FRAME` runs once per game frame, on the game's thread, with the game paused around it. The
callback gets a private register file to make guest calls with.

## Hooks

```c
static void MyHook(MscCpu* cpu, void* user) {
    /* arguments are in r3.. (and f1..) */
    api->call_original(cpu);         /* run the game's function (or the next plugin's hook) */
    /* its result is in r3 / f1; change it if you like */
}
...
api->hook(mod, 0x8027F9D4, MyHook, NULL);
```

A hook runs instead of the game function at that address. Calling `call_original` runs the original,
with whatever registers you've set. Not calling it skips the original, so set the result registers
yourself. When several plugins hook one function, the mod loaded later runs first, and its
`call_original` runs the next mod's hook.

**Which functions can be hooked.** The recompiled game calls its functions directly, with no table to
patch at run time, so only *wrap points* can be hooked: functions the runtime builds with a hook check
around the original. `api->is_hookable(address)` says whether an address is one. The list grows as
the framework wraps more of the game. If your mod needs a function that isn't hookable, open an issue
naming it, and it can be made a wrap point in a later build.

Every function the framework itself wraps is a wrap point. Arguments are in r3 onwards (r3 = `this`
for methods); results in r3.

| Address | Function | What it does |
| --- | --- | --- |
| `0x8027F9D4` | `const char* NisPlayer::GetTargetFilter(NisTarget, NisWinnerType) const` | The name a cutscene is picked by (`<filter>_goal_winner_high_0` and so on): a captain's or the scorer's internal name, `goalie`, a stadium. |
| `0x8027FD04` | `void NisPlayer::Load(const char* type, NisTarget, NisUseStadiumOffset, NisUseFilter, NisWinnerType, int, int)` | Picks and plays a cutscene of a type. |
| `0x8027D710` | `void NisPlayer::LoadTriggers(Nis&)` | Runs a cutscene's trigger script. |
| `0x8027EDCC` | `void NisPlayer::PrepareNisCue(ulong cue)` | Starts a cutscene's audio cue (r4 = `nlStringLowerHash` of its name). |
| `0x802E7DC4` | `EmissionController* fn_802E7DC4(EmissionManager*, const char* name, int view, bool, ushort)` | Starts an effect by name (cutscene triggers). |
| `0x802E7CDC` | `EffectsGroup* EmissionManager::GetEffectsGroup(const char* name)` | An effect group by name. |
| `0x802D064C` | `PlatTexture* glx_GetTex(ulong handle)` | A texture by name hash, from every resource pool. |
| `0x80301E6C` | `void TLComponent::SetActiveSlide(ulong hash, bool, bool)` | Shows a front-end component's slide by name hash. |
| `0x80301DA0` | `void TLComponent::SetActiveSlide(const char* name, bool, bool)` | The same, by name. |
| `0x8030677C` | `TLInstance* FEFindInstance(FEPresentation*, ulong ×6)` | A front-end instance by a path of up to six name hashes. |
| `0x803068F8` | `TLInstance* FEFindInstanceRecursive(TLInstance*, ulong ×6)` | The same, from an instance. |
| `0x801CF6E8` | `void MatchLoadingScene::SetTeamLogo(int side, CharacterInfo)` | The match loading screen's team portrait. |
| `0x801E0B8C` | `void FECharacterPDAComponent::ApplyCaptainColours(int captain, int opponent)` | Captain select's stats panel colours. |
| `0x801EE6A8` | `PausePostGameScene::BuildStoryArticle` | The post-match newspaper story. |
| `0x801F79C8` | `StrikerTimesOverlay::SceneCreated` | The Striker Times screen. |
| `0x8000BA00` | `CharacterLoader::fn_8000BA00()` | Creates the current loader entry's player. |
| `0x8000A418` | `CharacterLoader::fn_8000A418()` | Starts loading a character's effects bundles. |
| `0x8000A5D8` | `bool CharacterLoader::fn_8000A5D8()` | Registers them once loaded (false: still loading). |
| `0x8000A790` | `CharacterLoader::fn_8000A790()` | Registers a captain's team textures (`ExtraTextures.rlt`). |
| `0x80026370` | `void DestroyCharacters()` | Frees the match's players and character templates. |
| `0x800447C0` | `bool cFielder::fn_800447C0(ushort direction)` | Starts a deke (picks its animation and direction by class). |
| `0x800349DC` | `void cFielder::CleanUpAction(eFielderActionState next)` | Ends a player's current action. |
| `0x800395C0` | `void fn_800395C0(cFielder*)` | A deke's "disappear" trigger: the teleport. |

### Making a wrap point (runtime developers)

In the runtime, `PPC_NATIVE_WRAP(ADDR, name)` (`runtime/include/hle_stubs.h`) wraps the function at
`ADDR`: `name(CpuContext*)` runs for every caller, after the plugin hooks, and calls the translated
original as `func_ADDR`. Pass `func_ADDR` itself as `name` for a wrap point with no runtime logic of its
own. The address must be upper-case hex. Adding or removing one needs
a full rebuild (`tools/rebuild.sh`): the translator keeps the original, sends callers through the
wrapper, and stops inlining the original into them.

## Settings

```c
api->declare_bool(mod, "log_score", "Log the score", "Writes each match's score to console.log.", 1);
...
if (api->get_bool(mod, "log_score")) { ... }
```

Declared settings appear under the mod on the F10 Mods page and are saved in `Config.toml` under
`[mod_settings."<mod id>"]`. Reading one is cheap enough to do every frame.

## Logging

`api->log(mod, level, message)` writes to `console.log` as `[mods] <mod id>: <message>`.
