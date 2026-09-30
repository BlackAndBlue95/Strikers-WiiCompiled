# Strikers-WiiCompiled

<p align="center">
  <img alt="macOS 14+, Apple Silicon" src="https://img.shields.io/badge/macOS-14%2B%20%C2%B7%20Apple%20Silicon-0A84FF?logo=apple&amp;logoColor=white">
  <img alt="PowerPC static recompilation" src="https://img.shields.io/badge/PowerPC-static%20recompilation-FF9F0A">
  <a href="LICENSE"><img alt="License: GPLv3" src="https://img.shields.io/badge/license-GPLv3-2EA44F?logo=gnu&amp;logoColor=white"></a>
</p>

A native PC port of **Mario Strikers Charged**, made with static recompilation.

This is a fork of [WiiCompiled](https://github.com/patchzyy/wiicompiled), the Mario Kart Wii
static recompilation project, with its translator and runtime retargeted at Mario Strikers
Charged (USA, Rev 1, `R4QE01`). There's no emulator in the loop, no interpreter, no JIT, and no
PowerPC anywhere at runtime.

> [!IMPORTANT]
> There is no Nintendo code, no assets and no game data anywhere in this repository. You need
> your own legally dumped copy of the game. The translation runs on your machine against your own
> copy, and its output is never committed or uploaded.

> [!WARNING]
> This is an early, work-in-progress port, and it's only been tested on macOS (Apple Silicon).
> There are no prebuilt releases, and the WiiCompiled setup tool / Wheel Wizard integration is
> Mario Kart Wii only. Building from source is the only way to play.

---

## Status

Boot, menus, the Hub, tutorials, VS matches, music, voices and saving all work. The game runs
at full speed on Apple Silicon.

Known issues:

- Occasional rendering differences from the original hardware.
- Online play and WiiConnect24 features are not supported.
- Only the USA Rev 1 disc (`R4QE01`) has been mapped. Other regions and revisions won't work.

## Controls

Strikers Charged expects a Wii Remote and Nunchuk. This port emulates both from a regular
gamepad, with a layout that follows **Vague Rant's Classic Controller hack for this game**
(GBAtemp), by button position:

| Gamepad | Game action |
| --- | --- |
| Left stick | Nunchuk stick (movement), and the menu pointer |
| East face button | A: pass |
| South face button / right trigger (ZR) | B: shoot |
| North face button | C: item |
| West face button | Remote shake (big hit) |
| Left trigger (ZL) | Z: chip |
| Left shoulder (L) | Nunchuk shake (switch item) |
| Right shoulder (R) / right stick / D-pad | D-pad (deke, tackle) |
| Start / Back | 1: pause |

In menus, the left stick moves the on-screen pointer, which stays where you leave it.

Press **F10** in-game for the settings bar: resolution, FPS counter, volume and controller
bindings. Settings are saved to `Config.toml` straight away.

## Requirements

- macOS 14 (Sonoma) or later on Apple Silicon
- Xcode Command Line Tools, CMake, Ninja, Python 3, and the .NET 8 SDK
- Your own dump of **Mario Strikers Charged (USA) (Rev 1)**, `R4QE01`, extracted to a folder
  (for example with Dolphin: right-click the game > Properties > Filesystem > Extract Entire Disc)

> [!NOTE]
> Nobody here will tell you where to get the game. Dumping your own disc is on you, and links to
> game files won't be provided or tolerated.

## Building from source

1. Build the translator:

   ```bash
   dotnet build translator/src/Translator.Cli -c Release
   ```

2. Copy `main.dol` from your extracted disc (`sys/main.dol`) to `Assets/main.dol`. Its SHA-256
   must match the one in [`projects/mscharged/recomp.yml`](projects/mscharged/recomp.yml).
   `Assets/` is gitignored, so it never gets committed.

3. Translate and build:

   ```bash
   scripts/msc/rebuild.sh
   ```

   This statically translates the whole DOL to C++ (under `generated/`, also gitignored),
   generates the build graph, and compiles `build-macos/WiiCompiled`.

4. Point the runtime at your extracted disc. Create
   `~/Library/Application Support/MSCRecomp/Config.toml` with:

   ```toml
   [paths]
   dvd_root = "/path/to/your/extracted/game/DATA"
   ```

   The runtime keeps its config, saves and caches in `MSCRecomp`, separate from any Mario Kart
   Wii WiiCompiled install.

5. Run it:

   ```bash
   build-macos/WiiCompiled
   ```

After changing the runtime, rebuild with `cmake --build build-macos --target WiiCompiled`. Adding
or removing a `PPC_NATIVE_OVERRIDE` needs a full `scripts/msc/rebuild.sh`, because the translator
scans `runtime/src` for them.

## How the port works

WiiCompiled's translator is game-agnostic, but its runtime (the HLE for the Wii SDK, GX, audio,
input, NAND and so on) hooks Mario Kart Wii addresses. This fork re-keys those hooks to Strikers
Charged:

- `projects/mscharged/`: project config and a function map generated from the
  [mscharged-decomp](https://github.com/yannicksuter/mscharged-decomp) symbols.
- `scripts/msc/remap_runtime.py`: rewrites the runtime's MKW guest addresses to MSC ones.
  Functions are matched by name between the MKW and MSC symbol maps, and SDK globals use a
  hand-checked table (`scripts/msc/datamap.py`). Hooks with no MSC equivalent are left as
  `MSC-UNMAPPED` comments.
- `runtime/src/hle/msc_game.cpp` and the `MSC:` comments across the runtime: game-specific fixes,
  such as the emulated Wii Remote/Nunchuk, pointer handling, and texture and palette details of
  MSC's renderer.

## FAQ

**Is this an emulator?**
No. Everything is compiled to native code before you press play.

**Do you provide the game?**
No. Nothing in this repository contains Nintendo code or assets.

**Can I use the PAL or Japanese version?**
Not yet. Each version needs its own address map.

**Will you fix original bugs?**
The goal is behavior identical to real hardware. Only report things where this port differs from
the original game.

## AI usage

AI coding tools were used heavily in developing this port.

## Credits

- **[WiiCompiled](https://github.com/patchzyy/wiicompiled)** by patchzyy and contributors: the
  translator, runtime and everything this port builds on. The WiiCompiled logo is by inkwreck.
- **[mscharged-decomp](https://github.com/yannicksuter/mscharged-decomp)**: the Mario Strikers
  Charged decompilation whose symbols make the address mapping possible (CC0).
- **[mkw](https://github.com/riidefi/mkw)** by riidefi and contributors: the Mario Kart Wii
  decompilation, used for the MKW side of the mapping (CC0).
- **Vague Rant**: the Classic Controller layout for this game (GBAtemp).
- **[aurora](https://github.com/encounter/aurora)**: the GX rendering/windowing backend. MIT
  licensed.
- **[Dawn](https://dawn.googlesource.com/dawn)**: Google's WebGPU implementation, powering
  aurora's backends.
- **[Dolphin Emulator](https://github.com/dolphin-emu/dolphin)**: an invaluable hardware
  reference, and the source of the free DSP coefficient ROM and the default WiiConnect24
  bootstrap tree bundled with the runtime.

Bundled third-party components and their licenses are listed in
[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).

## License

Strikers-WiiCompiled, like WiiCompiled, is free software: you can redistribute it and/or modify
it under the terms of the [GNU General Public License, version 3](LICENSE) as published by the
Free Software Foundation.

It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the
implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
Public License for more details.

Not affiliated with, endorsed by, or associated with Nintendo. Mario Strikers Charged is a
trademark of Nintendo. No Nintendo intellectual property is contained in, distributed with, or
obtainable through this project.
