# Code mods (Kamek)

A code mod is PowerPC code that runs inside the game: a [Kamek](https://github.com/Treeki/Kamek)
module, compiled with the compiler Mario Strikers Charged itself was built with (CodeWarrior GC/3.0a5)
against the game's own classes from the [decomp](https://github.com/yannicksuter/mscharged-decomp).
It ships in a Riivolution pack, so the same pack runs on a Wii, in Dolphin and in Strikers Recharged:

| Where | What loads the module |
| --- | --- |
| Strikers Recharged | `build.sh` translates every installed pack's modules into the game, like the rest of `main.dol` |
| Wii, Dolphin | the Strikers Mod Loader (`packs/loader`), a small Riivolution pack that loads them from the disc |

Players need none of this; it's for people writing code mods.

## Set up

```bash
sdk/kamek/fetch-toolchain.sh
```

This fetches Kamek, the CodeWarrior compiler (from the decomp community's archive, as decomp projects
fetch it; it isn't redistributable), [wibo](https://github.com/decompals/wibo) to run it on macOS and
Linux, PowerPC binutils, and the decomp's headers, into `sdk/kamek/.toolchain` (never committed).

## Write one

```cpp
#include <kamek.h>

extern "C" void OSReport(const char* format, ...);
void nlInitTicker();

// Constructors run once, when the game starts its code mods (in nlInit, once the OS is up).
static struct Hello {
    Hello() { OSReport("hello from a code mod\n"); }
} s_hello;

// Hooks: nlInit's call to nlInitTicker (the bl at 0x802B44F0) calls this instead.
kmCallDefCpp(0x802B44F0, void, void) {
    nlInitTicker();
    OSReport("nlInitTicker hooked\n");
}

kmWrite32(0x806E0E38, 0x48454C4F);  // a data patch
```

- `include/kamek.h` has Kamek's hook macros (`kmWrite32`, `kmCall`, `kmBranch`, `kmBranchDefCpp`,
  `kmCallDefCpp`, `kmWritePointer`, ...), adapted for GC/3.0a5: hooks are named after their line, so
  keep one per line, and a hook function without parameters is written `kmCallDefCpp(addr, void, void)`.
- The game's functions and globals are called by the names the decomp gives them. Include the
  decomp's headers for its classes (`#include "Game/GameInfo.h"`, ...) or declare what you need;
  `externals-R4QE01.txt` resolves the names to addresses. `gen-externals.py` regenerates it from the
  decomp's `config/R4QE01/symbols.txt`.
- Addresses are the USA disc's, `R4QE01` Rev 1, the only version the port supports.

## Build it

```bash
sdk/kamek/build-module.sh out.bin source.cpp [more.cpp ...]
```

`out.bin` is the module (Kamek's dynamic format, relocated to wherever it's loaded) and `out.map` its
symbol map.

## Ship it in a pack

A module goes on the disc at the root, named `sml_<NN>_<pack>.bin`. Modules start in name order
(`sml_10_...` before `sml_50_...`); where two write the same address, the later one wins.

```
riivolution/mypack.xml
mypack/sml_50_mypack.bin
mypack/...                     the pack's other files
```

```xml
<wiidisc version="1">
  <id game="R4Q"/>
  <options>
    <section name="My pack">
      <option name="My pack" id="mypack" default="1">
        <choice name="Enabled"><patch id="mypack"/></choice>
      </option>
    </section>
  </options>
  <patch id="mypack" root="/mypack">
    <file disc="/sml_50_mypack.bin" external="sml_50_mypack.bin" create="true"/>
  </patch>
</wiidisc>
```

Code mods must be mapped with `<file>` patches (not `<folder>`). `examples/hello` is a complete pack:
`examples/hello/build-pack.sh` builds it.

## Try it in Strikers Recharged

1. Copy the pack (its `riivolution` folder and its own folder) into the data folder's `Riivolution`
   folder (macOS: `~/Library/Application Support/MSCRecomp/Riivolution`).
2. Run `build.sh` again: it lists the code mods it found and builds them into the game.
3. `console.log` lists the code mods built in, on lines starting with `[mods]`, and anything the
   modules print with `OSReport`, on lines starting with `[OSReport]`.

The modules' hooks are compiled into the game, so adding, updating or removing a code mod (or
turning its option on or off in F10 > Mods) takes another `build.sh`; F10 and `console.log` say when
the installed packs no longer match what was built in.

How the port runs them: `translator link-code-mods` finds the modules the installed packs put on the
disc, the same way the game resolves packs, and lays them out one after another from 0x81800000 (a
2 MiB region above the Wii's 24 MiB of MEM1). The base translation then accounts for every address they
patch, and `translator translate-mod --profile code-mods` translates the patched functions, the
modules' code and the places they branch back into the game. At boot the modules' image is copied in;
when the game calls `glplatPreStartup` from `nlInit` their patches are applied and their constructors
run (`runtime/src/mods/code_mods.cpp`). On a console the Strikers Mod Loader starts them a little later
in `nlInit`, once `nlInitFileSystem` has started the disc it reads them from.

Hooked code that runs before then (the game's static constructors, `OSInit`, `nlInitMemory`) already
runs the mod's code in the port, but not on a console, where nothing is loaded yet: don't hook it.
