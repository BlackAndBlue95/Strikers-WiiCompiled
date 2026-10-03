# Packs

Riivolution packs for Mario Strikers Charged (USA, `R4QE01`). Each folder has a pack's sources, its
Riivolution XML and a `build-pack.sh` that builds it, laid out like an SD card.
[docs/modding](../docs/modding/README.md) says how packs work, how to install them and how to make
one.

| Pack | What it does | Runs in |
| --- | --- | --- |
| [Strikers Mod Loader](loader/README.md) | loads the other packs' code | Dolphin, Wii |
| [Super Team](superteam/README.md) | Super Mario Strikers' robot team as a captain of its own | Strikers Recharged, Dolphin, Wii |
| [GameCube controllers](gamecube/README.md) | GameCube controllers as players, with menu navigation | Dolphin, Wii (built into Strikers Recharged) |

## Building

The packs with code need the Kamek toolchain, once:

```bash
sdk/kamek/fetch-toolchain.sh
```

Then each `build-pack.sh` writes its `riivolution/<pack>.xml` and its folder into the folder given:

```bash
packs/loader/build-pack.sh out/
packs/gamecube/build-pack.sh out/
packs/superteam/build-pack.sh --assets <Super Team's assets> --game <your extracted game> out/
```

Built into one folder, they make one ready to copy onto an SD card, into Dolphin's
`Load/Riivolution` folder or into Strikers Recharged's `Riivolution` folder (or to zip). Super Team
is built from your own discs: its [README](superteam/README.md) says what it needs.
