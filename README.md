# Strikers-WiiCompiled

<p align="center">
  <img alt="Windows 10 / 11, x64" src="https://img.shields.io/badge/Windows-10%20%2F%2011%20%C2%B7%20x64-0078D4">
  <img alt="Linux, x64 / ARM64" src="https://img.shields.io/badge/Linux-x64%20%2F%20ARM64-FCC624?logo=linux&amp;logoColor=white">
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
> This is an early, work-in-progress port. It's developed and tested on macOS (Apple Silicon);
> Windows and Linux builds use the same upstream WiiCompiled toolchain but are untested with this
> port so far, so expect rough edges there. There are no prebuilt releases, and the WiiCompiled
> setup tool / Wheel Wizard integration is Mario Kart Wii only.

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

- **Your own copy of Mario Strikers Charged (USA) (Rev 1)**, `R4QE01`, extracted to a folder.
  The easiest way is [Dolphin](https://dolphin-emu.org): right-click the game, then
  Properties > Filesystem > right-click the disc > Extract Entire Disc. Only this version works;
  the build checks `main.dol`'s SHA-256 and rejects anything else.
- A 64-bit Windows 10/11, Linux or macOS 14+ (Apple Silicon) machine with a GPU that supports
  Direct3D 12, Vulkan or Metal.
- About 10 GB of free disk space for the build.
- The [.NET 8 SDK](https://dotnet.microsoft.com/download/dotnet/8.0), CMake 3.25+, Ninja, and
  Clang. Per-platform setup is below.

> [!NOTE]
> Nobody here will tell you where to get the game. Dumping your own disc is on you, and links to
> game files won't be provided or tolerated.

### Windows

The runtime builds with **LLVM-MinGW** Clang, not MSVC.

1. Install the tools (from a terminal; or grab the installers from each project's site):

   ```powershell
   winget install Microsoft.DotNet.SDK.8 Kitware.CMake Ninja-build.Ninja Git.Git
   ```

2. Download the latest `llvm-mingw-<version>-ucrt-x86_64.zip` from
   [mstorsjo/llvm-mingw releases](https://github.com/mstorsjo/llvm-mingw/releases), extract it
   (for example to `C:\llvm-mingw`), and put its `bin` folder first on your `PATH`, so that
   `clang --version` reports the target `x86_64-w64-windows-gnu`.

### Linux (Debian/Ubuntu shown)

```bash
sudo apt install git clang lld cmake ninja-build pkg-config dotnet-sdk-8.0 \
  libasound2-dev libpulse-dev libpipewire-0.3-dev libx11-dev libxext-dev libxrandr-dev \
  libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev libdrm-dev \
  libgbm-dev libgl1-mesa-dev libegl1-mesa-dev libwayland-dev libdecor-0-dev libdbus-1-dev \
  libudev-dev libusb-1.0-0-dev
```

(`dotnet-sdk-8.0` may need [Microsoft's package feed](https://learn.microsoft.com/dotnet/core/install/linux)
on older distributions. Other distributions need the equivalent packages.)

### macOS (Apple Silicon)

```bash
xcode-select --install
brew install cmake ninja
```

and the [.NET 8 SDK installer](https://dotnet.microsoft.com/download/dotnet/8.0) (Arm64) from Microsoft.

## Building

Clone the repository and run the build script with the folder you extracted the game into:

```bash
git clone https://github.com/BlackAndBlue95/Strikers-WiiCompiled.git
cd Strikers-WiiCompiled
./build.sh "/path/to/extracted/game"
```

On Windows, run this from a terminal in the repository folder:

```bat
build.cmd "C:\path\to\extracted\game"
```

The script:

1. checks and copies `sys/main.dol` to `Assets/main.dol`,
2. builds the translator,
3. statically translates the game code to C++ under `generated/`,
4. compiles the runtime and translated code with CMake + Ninja + Clang into
   `build-windows/`, `build-linux/` or `build-macos/`,
5. points the runtime's `Config.toml` at your extracted disc.

The first build takes a while (translation is a few minutes, and compiling the translated code
and fetching the graphics dependencies can take much longer on slower machines). Logs for every
step are written to the build folder. Later runs are incremental.

Then run `build-<platform>/Strikers-WiiCompiled` (`Strikers-WiiCompiled.exe` on Windows). Config, saves and caches
live in a `MSCRecomp` folder, separate from any Mario Kart Wii WiiCompiled install:

| Platform | Location |
| --- | --- |
| Windows | `%LOCALAPPDATA%\MSCRecomp` |
| Linux | `$XDG_DATA_HOME/MSCRecomp` (default `~/.local/share/MSCRecomp`) |
| macOS | `~/Library/Application Support/MSCRecomp` |

> [!IMPORTANT]
> Everything the build produces (`Assets/`, `generated/`, `build-*/`) contains or is derived from
> game code, and is gitignored. Don't commit it, upload it or share builds. Everyone builds from
> their own copy.

### Developing

After changing only runtime code, `./build.sh --skip-translate` (`build.cmd -SkipTranslate`) recompiles
without retranslating. Adding or removing a `PPC_NATIVE_OVERRIDE` needs a full run, because the
translator scans `runtime/src` for them to decide which game functions to leave untranslated.

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
