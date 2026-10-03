# Strikers Mod Loader

Loads packs' code (Kamek modules) on a Wii and in Dolphin. Put it next to any pack with code there.
Strikers Recharged builds packs' code into the game instead, and ignores this pack.

## How it works

Riivolution writes the loader into memory the game never uses (MetroTRK's debugger table, at
0x80004060) and puts a call to it in place of `nlInit`'s call to `nlInitFileSystem`, but only if that
call is where the USA disc has it (`original="480B30E5"`), so on any other disc nothing changes. The
loader starts the file system, then loads every `sml_*.bin` at the disc root, in name order, with
Kamek's loader: each module's code goes in memory from the game's heap, its patches are applied and
its constructors run. Dolphin's log shows `[Strikers Mod Loader] N code mod(s)` and each module.

Its source is [`sdk/kamek/loader`](../../sdk/kamek/loader): `strikers.cpp`, around Kamek's own
`kamekLoader.cpp` from the toolchain.

## Building it

```bash
sdk/kamek/fetch-toolchain.sh   # once
packs/loader/build-pack.sh out/
```

`out/` then holds `riivolution/strikersloader.xml` and `strikersloader/loader.bin`.
