# Kamek SDK

What code mods are built with: [Code mods](../../docs/modding/code-mods.md) is the guide.

```bash
sdk/kamek/fetch-toolchain.sh                           # once: Kamek, the compiler, the decomp's headers
sdk/kamek/build-module.sh out.bin source.cpp [...]     # a module
sdk/kamek/examples/hello/build-pack.sh out/            # the example, as a pack
```

| Path | What it is |
| --- | --- |
| `fetch-toolchain.sh` | fetches Kamek, CodeWarrior GC/3.0a5 (the game's own compiler), wibo to run it on macOS and Linux, PowerPC binutils and the decomp's headers into `.toolchain/` (never committed) |
| `build-module.sh` | compiles sources against the decomp's headers and links them with Kamek into a module and its map |
| `include/kamek.h` | Kamek's hook macros, adapted for GC/3.0a5 |
| `externals-R4QE01.txt` | the game's functions and globals by name, for linking; `gen-externals.py` regenerates it from the decomp's symbols |
| `examples/hello/` | the smallest code mod, as a complete pack |
| `loader/` | the Strikers Mod Loader's source ([packs/loader](../../packs/loader/README.md) builds it) |
