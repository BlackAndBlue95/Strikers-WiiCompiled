# Strikers-WiiCompiled

<p align="center">
  <img alt="Windows 10 / 11, x64" src="https://img.shields.io/badge/Windows-10%20%2F%2011%20%C2%B7%20x64-0078D4">
  <img alt="Linux, x64 / ARM64" src="https://img.shields.io/badge/Linux-x64%20%2F%20ARM64-FCC624?logo=linux&amp;logoColor=white">
  <img alt="macOS 14+, Apple Silicon" src="https://img.shields.io/badge/macOS-14%2B%20%C2%B7%20Apple%20Silicon-0A84FF?logo=apple&amp;logoColor=white">
  <img alt="PowerPC static recompilation" src="https://img.shields.io/badge/PowerPC-static%20recompilation-FF9F0A">
  <a href="LICENSE"><img alt="License: GPLv3" src="https://img.shields.io/badge/license-GPLv3-2EA44F?logo=gnu&amp;logoColor=white"></a>
</p>

A native PC port of **Mario Strikers Charged**, made with static recompilation. It's a fork of
[WiiCompiled](https://github.com/patchzyy/wiicompiled), the Mario Kart Wii project, with the
translator and runtime retargeted at Mario Strikers Charged (USA, Rev 1, `R4QE01`). There's no
emulator, interpreter or JIT, and no PowerPC code runs at all.

> [!IMPORTANT]
> This repository contains no Nintendo code, game data or assets. You need your own legally dumped
> copy of the game. The translation runs on your machine, and its output is never uploaded or
> committed.

> [!WARNING]
> This is an early, work-in-progress port, developed and tested on macOS (Apple Silicon). Windows
> and Linux builds use the same toolchain but are untested with this port, so expect rough edges.
> There are no prebuilt releases.

The [wiki](https://github.com/BlackAndBlue95/Strikers-WiiCompiled/wiki) has the details:
[building](https://github.com/BlackAndBlue95/Strikers-WiiCompiled/wiki/Building),
[controls](https://github.com/BlackAndBlue95/Strikers-WiiCompiled/wiki/Controls),
[Wii Remotes](https://github.com/BlackAndBlue95/Strikers-WiiCompiled/wiki/Wii-Remotes),
[graphics options](https://github.com/BlackAndBlue95/Strikers-WiiCompiled/wiki/Graphics) and
[every tweak](https://github.com/BlackAndBlue95/Strikers-WiiCompiled/wiki/Tweaks).

## Status

Booting, menus, the Hub, tutorials, VS matches, music, voices and saving all work, and the game
runs at full speed on Apple Silicon.

- **120 FPS:** **F10 > Graphics > Frame rate**, or `frame_rate = 120` in `Config.toml`. It isn't
  interpolation: matches run on a fixed clock and twice as many frames are really rendered. It
  needs a high refresh rate display and uses more power, so 60 is the default.
- **Known issues:** occasional rendering differences from the original hardware. Online play and
  WiiConnect24 aren't supported. Only the USA Rev 1 disc (`R4QE01`) is mapped; other regions and
  revisions won't work.

### Real Wii Remotes (experimental)

Real Wii Remotes, with or without a Nunchuk, connect directly over Bluetooth HID, with the IR
camera and accelerometers driven the way Dolphin does it.

- **Pairing:** press 1+2 (or SYNC) while the game is searching, or pair the remote in your
  system's Bluetooth settings.
- **Sensor bar:** pointing needs an IR source. If yours sits above the screen, tick
  **F10 > Wii Remotes (Bluetooth) > Sensor bar is above the screen**.
- **DolphinBar:** use it in **mode 4**, its Wii Remote mode.
- **Limitations:** plain HID access may not work on macOS, and the Wii U Pro Controller isn't
  supported.

Full guide, including connecting on Windows:
[Wii Remotes](https://github.com/BlackAndBlue95/Strikers-WiiCompiled/wiki/Wii-Remotes).

## Controls

The game was built for the Wii Remote and Nunchuk. This port plays it on a regular controller with
*Super Mario Strikers*' GameCube controls, which the engine still has underneath, plus Charged's
extras. Face buttons follow **Nintendo's layout by position**: the bottom button is always B.

- **A (right):** pass; with **L**, a lob pass. Without the ball, switch player.
- **B (bottom):** shoot, hold to charge (captains: Mega Strike). Without the ball, slide tackle.
- **Y (left):** deke. Without the ball, big hit.
- **X (top):** use item.
- **R:** the character's special move. **L:** lob modifier. **Z** (right bumper): cycle items.
- **Right stick:** deke in that direction. **D-pad:** Charged's D-pad moves.

Buttons can be rebound per controller in **F10**, saved to `Config.toml`. GameCube controllers work
through the official Wii U / Switch GameCube adapter (on Windows, switch it to the WinUSB driver
once with [Zadig](https://zadig.akeo.ie/)).
[Full controls](https://github.com/BlackAndBlue95/Strikers-WiiCompiled/wiki/Controls).

## Mods and tweaks

Mods are packages with a `mod.toml` manifest that go in the `Mods` folder: new characters with
their own models, animations, voices, cutscenes and menu art, additions to the game's shared files,
and native plugins. Switch them on or off in **F10 > Mods**; [docs/modding](docs/modding/README.md)
explains how to make one.

**F10 > Tweaks** has quality-of-life switches:

- **Menus:** navigate with the D-pad and sticks instead of a pointer, skip transitions with
  **Fast menus**, and boot straight to the main menu with **Skip intro**.
- **No Mega Strikes with controllers** (on by default): defending a Mega Strike takes a Wii Remote
  pointer, so with a controller they're off for both sides to keep matches playable.
- **Community fixes and extras:** the NK bug fix, home/away kit choice on captain select, win by 2,
  Blue Peach against red teams, captain-only teams.

[Every tweak](https://github.com/BlackAndBlue95/Strikers-WiiCompiled/wiki/Tweaks).

## Requirements

- **Game:** your own dump of Mario Strikers Charged (USA) (Rev 1), `R4QE01`. The build checks
  `main.dol`'s SHA-256 and rejects any other version.
- **Machine:** 64-bit Windows 10/11, Linux, or macOS 14+ on Apple Silicon, with a GPU that supports
  Direct3D 12, Vulkan or Metal, and about 10 GB of free disk space.
- **Tools:** the .NET 8 SDK, CMake 3.25+, Ninja and Clang (LLVM-MinGW Clang on Windows).
  Per-platform setup: [Building](https://github.com/BlackAndBlue95/Strikers-WiiCompiled/wiki/Building).

## Building

Clone the repository, run `./build.sh` (`build.cmd` on Windows) and pick your disc image when
asked. The script extracts it, translates the game code and compiles the runtime. You don't need
the disc image afterwards: the game files, config, saves and caches live in an `MSCRecomp` folder
([where](https://github.com/BlackAndBlue95/Strikers-WiiCompiled/wiki/Building#building-the-port)).

## How the port works

WiiCompiled's translator is game-agnostic: it turns the game's PowerPC code into C++ ahead of time.
Its runtime, which stands in for the Wii's hardware and system software, was written against Mario
Kart Wii's addresses; this fork re-keyed it to Strikers Charged using the symbols from
[mscharged-decomp](https://github.com/yannicksuter/mscharged-decomp), and the Strikers-specific
parts (the emulated Wii Remote and pointer, renderer details, the tweaks and the mod framework)
live in the runtime too.
[More](https://github.com/BlackAndBlue95/Strikers-WiiCompiled/wiki/How-the-Port-Works).

## FAQ

- **Is this an emulator?** No. Everything is compiled to native code before you play.
- **Can I use the PAL or Japanese version?** Not yet: each version needs its own address map.

## Credits

Built on [WiiCompiled](https://github.com/patchzyy/wiicompiled),
[mscharged-decomp](https://github.com/yannicksuter/mscharged-decomp),
[aurora](https://github.com/encounter/aurora), [nod](https://github.com/encounter/nod) and
[Dolphin](https://github.com/dolphin-emu/dolphin). Bundled third-party components and their
licenses are listed in [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).

## AI usage

AI coding tools were used heavily in developing this port.

## License

Strikers-WiiCompiled, like WiiCompiled, is free software: you can redistribute it and/or modify it
under the terms of the [GNU General Public License, version 3](LICENSE) as published by the Free
Software Foundation.

It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the
implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public
License for more details.

Not affiliated with, endorsed by, or associated with Nintendo. Mario Strikers Charged is a
trademark of Nintendo. No Nintendo intellectual property is contained in, distributed with, or
obtainable through this project.
