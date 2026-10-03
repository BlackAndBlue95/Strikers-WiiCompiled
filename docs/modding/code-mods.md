# Code mods

A code mod is PowerPC code that runs inside the game: a [Kamek](https://github.com/Treeki/Kamek)
module, compiled with the compiler Mario Strikers Charged itself was built with (CodeWarrior GC/3.0a5)
against the game's own classes from the [decomp](https://github.com/yannicksuter/mscharged-decomp).
It ships in a pack, so it runs everywhere the pack does:

| Where | What loads it |
| --- | --- |
| Strikers Recharged | `build.sh`, which translates the installed packs' code into the game along with `main.dol` |
| Dolphin, Wii | the [Strikers Mod Loader](../../packs/loader/README.md), a pack that loads it from the disc when the game starts |

- [Set up](#set-up)
- [A first mod](#a-first-mod)
- [Hooks](#hooks)
- [Build it](#build-it)
- [Ship it](#ship-it)
- [Try it](#try-it)
- [Writing for a console](#writing-for-a-console)
- [How Strikers Recharged builds code mods](#how-strikers-recharged-builds-code-mods)

## Set up

```bash
sdk/kamek/fetch-toolchain.sh
```

It fetches Kamek, the CodeWarrior compiler (from the decomp community's archive, as decomp projects
do; it isn't redistributable), [wibo](https://github.com/decompals/wibo) to run it on macOS and
Linux, PowerPC binutils and the decomp's headers, into `sdk/kamek/.toolchain` (never committed).

## A first mod

[`sdk/kamek/examples/hello`](../../sdk/kamek/examples/hello) is a complete pack:

```cpp
#include <kamek.h>

// The game's own functions and globals, by the names the decomp gives them.
extern "C" void OSReport(const char* format, ...);
void nlInitTicker();
extern unsigned int sDebugPassSpaceSearch;  // a word of the game's .sbss that retail never reads

// A data patch, applied when the game starts its code mods, before any constructor runs.
kmWrite32(&sDebugPassSpaceSearch, 0x48454C4F);  // "HELO"

// A constructor: runs once, when the game starts its code mods (in nlInit, once the OS is up).
static struct Hello {
    Hello() { OSReport("[hello] constructed; the data patch reads 0x%08X\n", sDebugPassSpaceSearch); }
} s_hello;

// A hook: nlInit's call to nlInitTicker (the bl at 0x802B44F0) calls this instead.
kmCallDefCpp(0x802B44F0, void, void) {
    nlInitTicker();
    OSReport("[hello] nlInitTicker hooked\n");
}
```

```bash
sdk/kamek/examples/hello/build-pack.sh out/
```

builds it into `out/` as a pack; [Try it](#try-it) runs it.

## Hooks

`sdk/kamek/include/kamek.h` has Kamek's hook macros, adapted for GC/3.0a5:

| Macro | What it does |
| --- | --- |
| `kmWrite32(addr, value)`, `kmWritePointer(addr, ptr)` | writes a word: data, or an instruction |
| `kmCall(addr, fn)` | turns the `bl` at `addr` into a call to `fn` |
| `kmBranch(addr, fn)` | makes the game's function at `addr` jump to `fn`: yours runs instead of it |
| `kmCallDefCpp(addr, ret, args...)`, `kmBranchDefCpp(...)` | the same, with the function written inline |

Hooks are named after their line, so keep one per line; a hook function without parameters is
written `kmCallDefCpp(addr, void, void)`.

**Calling the game.** Its functions and globals go by the names the decomp gives them: include the
decomp's headers for its classes (`#include "Game/GameInfo.h"`) or declare what you need, and
`sdk/kamek/externals-R4QE01.txt` resolves the names to addresses (`gen-externals.py` regenerates it
from the decomp's `config/R4QE01/symbols.txt`). A function the headers don't declare well can be
called by address: `((void (*)(Nis*, int))0x80282390)(nis, type)`.

**Keeping the original.** To run code before or after one of the game's functions rather than
instead of it, `kmBranch` to yours and call the original through a trampoline: the instruction your
branch replaced, then a jump to the next one.

```cpp
asm static void UpdatePlatPad_Original() {
    nofralloc
    stwu r1, -16(r1)      // the function's first instruction, as the disassembly shows it
    lis r12, 0x8037       // then on into its second, at 0x80375380
    ori r12, r12, 0x5380
    mtctr r12
    bctr
}

static void UpdatePlatPad(PlatPadManager* manager) {
    // before it ...
    ((void (*)(PlatPadManager*))UpdatePlatPad_Original)(manager);
    // ... and after
}
kmBranch(0x8037537C, UpdatePlatPad);
```

The replaced instruction has to work anywhere (a `stwu`, `mflr`, `mr`; not a branch). The packs'
code is full of these: `packs/*/code` are worked examples.

All addresses are the USA disc's, `R4QE01` revision 1, the only version the port supports.

## Build it

```bash
sdk/kamek/build-module.sh out.bin source.cpp [more.cpp ...]
```

`out.bin` is the module (Kamek's dynamic format, relocated to wherever it's loaded) and `out.map` its
symbol map.

## Ship it

A module goes at the disc root as `sml_<NN>_<pack>.bin`, mapped there by a `<file>` patch (not a
`<folder>`). Modules start in name order, `sml_10_...` before `sml_50_...`; where two write the same
address, the later one wins.

```
riivolution/mypack.xml
mypack/sml_50_mypack.bin
```

```xml
<wiidisc version="1" root="/mypack">
  <id game="R4QE"/>
  <options>
    <section name="My pack">
      <option name="My pack" id="mypack" default="1">
        <choice name="Enabled"><patch id="mypack"/></choice>
      </option>
    </section>
  </options>
  <patch id="mypack">
    <file disc="/sml_50_mypack.bin" external="sml_50_mypack.bin" create="true"/>
  </patch>
</wiidisc>
```

## Try it

**In Strikers Recharged:**

1. Copy the pack into the data folder's `Riivolution` folder.
2. Run `build.sh` again: it lists the code mods it found and builds them in.
3. `console.log` lists the code mods built in, on lines starting with `[mods]`, and what they print
   with `OSReport`, on lines starting with `[OSReport]`.

Adding, updating or removing a code mod, or turning its option on or off, takes another `build.sh`;
F10 and `console.log` say when the installed packs no longer match the build.

**In Dolphin:** copy the pack and the Strikers Mod Loader into Dolphin's `Load/Riivolution` folder and
start the game with both turned on ([Installing packs](installing.md#in-dolphin)). With
**OSReport** on in Dolphin's log, the loader's lines and your `OSReport`s appear there. A crash shows
a dialog like `Invalid read from 0x0000000c, PC = 0x802e44b4`: the decomp's `symbols.txt` (or your
module's `.map`, for an address in your code) says which function the PC is in. Dolphin is the
closest thing to a Wii; test there before a pack goes on one.

## Writing for a console

The port and a console run the same code, but the port forgives things a Wii doesn't, and some of the
game's code is native in the port. What came up making the packs here:

- **The port replaces some of the game's functions with native code** (`PPC_NATIVE_OVERRIDE` in
  `runtime/src`). A hook on one of them runs on a console and never in the port: `grep -rn
  "NATIVE.*8027D710" runtime/src` tells for the function at 0x8027D710. The GameCube controllers pack
  hooks several the port has native versions of, which is fine: the port doesn't need it.
- **A bad read is harmless in the port and a crash on a Wii.** Low memory is mapped in the port, so
  reading through a null pointer quietly returns whatever is there; on a Wii (and in Dolphin) it stops
  the game. Check the pointers you follow from the game's structures.
- **The game trusts its own data.** It looks things up by a character's name (effects, textures,
  cameras) and often uses the result unchecked: fine for its 12 captains, empty for a new one. Super
  Team's Mega Strike crashed a console that way until `effects.cpp` fell back to its base's effects.
- **Some of the Wii SDK's calls wait for an answer that never comes.** `WPADGetInfo` on a channel
  with no Wii Remote waits forever (the SDK calls back before it starts waiting); the GameCube pack
  keeps the title screen from asking.
- **What the game doesn't use isn't set up.** It never initializes GameCube controllers (`PADInit`
  isn't even linked), so the GameCube pack does.
- **Nothing runs before the loader on a console.** The loader starts the modules in `nlInit`, once
  the file system is up; hooks on code that runs earlier (static constructors, `OSInit`,
  `nlInitMemory`) run in the port but not on a Wii. Don't hook it.
- **A module's memory comes from the game's heap** on a console (Kamek's loader takes it with
  `nlMalloc`), so a big buffer costs the game memory there.

## How Strikers Recharged builds code mods

`translator link-code-mods` finds the modules the installed packs put on the disc, resolving packs the
way the game does, and lays them out one after another from 0x81800000 (a 2 MiB region above the
Wii's 24 MiB of MEM1). The base translation then accounts for every address they patch, and
`translator translate-mod --profile code-mods` translates the patched functions, the modules' code and
the places they branch back into the game. At boot the modules' image is copied in; when the game
calls `glplatPreStartup` from `nlInit`, their patches are applied and their constructors run
(`runtime/src/mods/code_mods.cpp`). On a console the Strikers Mod Loader starts them a little later in
`nlInit`, once `nlInitFileSystem` has started the disc it reads them from.
