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

**120 FPS:** set **F10 > Video > Frame rate** to 120 FPS (or `frame_rate = 120` under `[video]`
in `Config.toml`). This isn't interpolation: the game simulates matches on a fixed clock and
blends between those steps when it draws, so it really renders twice as many frames at normal
game speed. It needs a high refresh rate display, such as a ProMotion MacBook or a 120 Hz+
monitor, and uses more power, so the default is 60.

Known issues:

- Occasional rendering differences from the original hardware.
- Online play and WiiConnect24 features are not supported.
- Only the USA Rev 1 disc (`R4QE01`) has been mapped. Other regions and revisions won't work.

### Real Wii Remotes (experimental)

Real Wii Remotes, with or without a Nunchuk, are driven directly over Bluetooth HID the way
Dolphin does it: the IR camera is switched on for the **pointer** (menus, Mega Strike defence),
along with the accelerometers and the Nunchuk. Pair the remote in your system's Bluetooth settings
(press 1+2 or the red SYNC button, leave any PIN empty), and it takes the first free player port.
You need a sensor bar (or any IR source) for the pointer. If it sits above your screen rather than
below, tick **F10 > Sensor bar is above the screen** (like the Wii's sensor bar setting) so the
pointer lines up with where you aim. The HOME Menu's Wii Menu and Reset options close the game.

**Mayflash DolphinBar:** use it in **mode 4**, its Wii Remote mode (the same mode Dolphin needs),
and sync the remotes to the bar. In the other modes it presents remotes as mouse, keyboard or
gamepad input, which the game can't use as Wii Remotes.

This is new and has had little testing, so reports are very welcome. `console.log` lists each step
("Wii Remote connected on port 1", "Nunchuk connected", "IR camera setup failed", ...). Known gaps:

- On macOS, plain HID access to Wii Remotes may not work at all (Dolphin uses a separate
  Bluetooth backend there).
- The Wii U Pro Controller isn't supported by this backend.

Where to start: `runtime/src/wiimote_hid.cpp` (the remote driver) and
`runtime/src/wii_remote_input.cpp` (how its state reaches the game).

## Controls

Strikers Charged was built for the Wii Remote and Nunchuk. This port plays it on a regular
controller with **Super Mario Strikers' GameCube controls**, plus Charged's extras on top. Under
the hood it rebuilds the GameCube controller support the engine inherited from SMS, so passing,
shooting and the rest go through the game's own GameCube button table, and the moves Charged
turned into Wii Remote gestures are back on SMS's buttons.

Face buttons follow **Nintendo's layout by position**: on a Switch-style controller the labels
match, and on an Xbox-style pad they sit where a Switch controller's would (bottom button = B).

| Button (Nintendo position) | With the ball | Without the ball |
| --- | --- | --- |
| **A** (right) | Pass (with L: lob pass) | Switch player |
| **B** (bottom) | Shoot, hold to charge (captain: Mega Strike) | Slide tackle, when the other team has the ball |
| **Y** (left) | Deke | Big hit |
| **X** (top) | Use item | Use item |
| **Right stick** (C-stick) | Deke in that direction | |
| **Right bumper** (Z) | Cycle items | Cycle items |
| **Left trigger** (L) | Lob/chip modifier | |
| **Right trigger** (R) | Charged extra: the character's special move | Charged extra: special move |
| **D-pad** | Charged's D-pad moves | Charged's D-pad moves |
| **Start** | Pause | Pause |

Buttons can be rebound per controller in the **F10** settings bar, which also has resolution,
FPS counter, volume and the mods below. Settings are saved to `Config.toml` straight away.

Real Wii Remotes use the game's original controls (see *Real Wii Remotes* above).

**GameCube controllers** work through the official Wii U / Switch GameCube adapter (WUP-028), or
a third-party adapter switched to its Wii U mode. As with Dolphin, on Windows the adapter must be
switched to the WinUSB driver once with [Zadig](https://zadig.akeo.ie/).

## Mods

Strikers Charged was built around the Wii Remote pointer. These controller-friendly changes are
switched on or off under **F10 > Mods** (or `[mods]` in `Config.toml`):

- **Controller menu navigation** (on): the D-pad and sticks move the game's hand cursor between
  buttons, row by row (on the main ring menu, the stick picks the icon in that direction). New
  screens start on their first option, **B** goes back (the on-screen BACK button is never
  reached by moving), and **L/R** flip pages such as stages on stage select.
- **Selection badge** (off): marks the selected button with a badge in the player's colour.
- **Fast menus** (off): skips menu transitions. Panels that slide in and out appear in place, and
  camera moves such as the zoom from the main menu are over in a frame or two. Idle animations,
  music and matches are unchanged.
- **No Mega Strikes with controllers** (on): see below.

The same menu has fixes and extras, several taken from the community's
[setup guide](https://mariostrikers.gg/msc-setup-guide):

- **Fix the NK bug** (on): a deke or teleport through the goalie that gets cut short (Boo deking
  into his own Kritter, Dry Bones teleporting behind the goal) no longer leaves every shot passing
  through Kritter for the rest of the match.
- **Choose home/away kits on captain select** (on): with both captains picked, **X** switches the
  home team between its home and away kit and **Y** the away team (Wii Remote: **-** and **2**).
  Mario, Luigi, Waluigi and Wario have no away kit in the game, so they get generated ones (white
  with red trim, sky blue, orange, blue), recoloured at runtime from your own disc's textures:
  captain, sidekicks, goalie and team art.
- **Unlock everything** (off): all characters, stadiums and cheats through the game's own
  unlock-all switch. Your save isn't changed.
- **Win by 2** (off): in first-to-X goal matches, you have to win by at least 2 goals.
- **Captain-only teams** (off): captains can be teammates, using a switch the developers left in
  the game. On the sidekick screen, pick a slot and captain select's grid comes up under the team
  boards: pick a captain. **-** and **+** (**L/R** on a controller) switch to the sidekicks, so a
  team can mix both (say Waluigi, Waluigi, Boo and Boo). **Default** makes every slot the team's
  captain and **Random** picks at random from whichever grid is showing. On captain select,
  **-** and **+** switch the grid to the sidekicks too, so a sidekick can lead a team: it wears a
  captain's colours, banners, logo and goalie (Mario at home and Luigi away to start; **X/Y** or
  **L/R** cycle them once both teams are picked), for a team of four Boos in Yoshi green. Teams are
  kept for next time. The match intro is skipped with this on.
- **Blue Peach against red teams** (off): Peach wears her blue kit against red captains.
- **Shot counter on the results screen** (off): the Mega Strike row shows white and yellow shots /
  red and orange shots instead.
- **All stadiums fast-paced** (off): every pitch plays like the fast, dry ones.

> [!NOTE]
> **Mega Strikes are disabled when you play with a controller.** Defending a Mega Strike means
> pointing the Wii Remote at each incoming ball, which a gamepad or keyboard can't do, so the
> shots would simply always go in. While a gamepad or keyboard is in use, every match has Mega
> Strikes off for both sides: a fully charged captain shot becomes a normal strong shot, for you
> and the CPU alike. Your saved game options aren't changed, and with real Wii Remotes Mega
> Strikes work as normal. Turn the mod off under F10 > Mods if you want them back.

## Requirements

- **Your own dump of Mario Strikers Charged (USA) (Rev 1)**, `R4QE01`, as a disc image (ISO,
  RVZ, WBFS, WIA, CISO, GCZ, ...). An already-extracted folder works too. Only this version is
  supported; the build checks `main.dol`'s SHA-256 and rejects anything else.
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

Clone the repository and run the build script:

```bash
git clone https://github.com/BlackAndBlue95/Strikers-WiiCompiled.git
cd Strikers-WiiCompiled
./build.sh
```

On Windows, double-click `build.cmd` in the repository folder (or run it from a terminal).

The script asks you to pick your disc image; that's the only input it needs. You can also pass
it directly: `./build.sh "/path/to/game.wbfs"` or `build.cmd "C:\path\to\game.wbfs"`. A folder
extracted with Dolphin works in place of an image.

The script:

1. installs the game files into the app's data folder (see the table below), extracting a disc
   image with [nodtool](https://github.com/encounter/nod) (downloaded once, pinned by version
   and checksum),
2. checks and copies `sys/main.dol` to `Assets/main.dol`,
3. builds the translator,
4. statically translates the game code to C++ under `generated/`,
5. compiles the runtime and translated code with CMake + Ninja + Clang into
   `build-windows/`, `build-linux/` or `build-macos/`,
6. points the runtime's `Config.toml` at the installed game files.

Like a WiiCompiled install, you **don't need the original disc image or folder afterwards**: the
game reads its files from the data folder. (Keep your dump backed up somewhere if you ever want
to reinstall.)

The first build takes a while (translation is a few minutes, and compiling the translated code
and fetching the graphics dependencies can take much longer on slower machines). Logs for every
step are written to the build folder. Later runs are incremental.

Then run `build-<platform>/Strikers-WiiCompiled` (`Strikers-WiiCompiled.exe` on Windows). The game
files (`Game/`), config, saves and caches live in a `MSCRecomp` folder, separate from any Mario
Kart Wii WiiCompiled install:

| Platform | Location |
| --- | --- |
| Windows | `%LOCALAPPDATA%\MSCRecomp` |
| Linux | `$XDG_DATA_HOME/MSCRecomp` (default `~/.local/share/MSCRecomp`) |
| macOS | `~/Library/Application Support/MSCRecomp` |

> [!IMPORTANT]
> The installed game files and everything the build produces (`Assets/`, `generated/`, `build-*/`)
> contain or are derived from the game, and the build output is gitignored. Don't commit it, upload
> it or share builds. Everyone builds from their own copy.

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
- **[Super Mario Strikers decomp](https://github.com/yannicksuter/smstrikers-decomp)**: how the
  shared engine handled GameCube controls, which made rebuilding them possible.
- **[aurora](https://github.com/encounter/aurora)**: the GX rendering/windowing backend. MIT
  licensed.
- **[Dawn](https://dawn.googlesource.com/dawn)**: Google's WebGPU implementation, powering
  aurora's backends.
- **[nod](https://github.com/encounter/nod)**: the disc-image library and `nodtool`, used to read
  your disc image. MIT licensed.
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
