# Strikers-WiiCompiled

Strikers-WiiCompiled is a native PC port of **Mario Strikers Charged**, achieved through static recompilation. Forked from the Mario Kart Wii project WiiCompiled, this version retargets the translator and runtime specifically for Mario Strikers Charged (USA, Rev 1, `R4QE01`). It operates entirely without emulators, interpreters, JIT compilers, or PowerPC emulation at runtime.

> [!IMPORTANT]
> This repository contains zero Nintendo code, game data, or assets. You must provide your own legally dumped copy of the game. The translation runs locally on your machine, and the output is never uploaded or committed.

> [!WARNING]
> This is an early, work-in-progress port. It is primarily developed and tested on macOS (Apple Silicon). Windows and Linux builds use the upstream WiiCompiled toolchain but remain untested, so expect rough edges. There are no prebuilt releases available.

The [wiki](https://github.com/BlackAndBlue95/Strikers-WiiCompiled/wiki) has the details: [building](https://github.com/BlackAndBlue95/Strikers-WiiCompiled/wiki/Building), [controls](https://github.com/BlackAndBlue95/Strikers-WiiCompiled/wiki/Controls), [Wii Remotes](https://github.com/BlackAndBlue95/Strikers-WiiCompiled/wiki/Wii-Remotes), [graphics options](https://github.com/BlackAndBlue95/Strikers-WiiCompiled/wiki/Graphics) and [every tweak](https://github.com/BlackAndBlue95/Strikers-WiiCompiled/wiki/Tweaks).

## Status

Core gameplay elements—including booting, menus, the Hub, tutorials, VS matches, music, voice lines, and saving—are fully functional. The game runs at full speed on Apple Silicon hardware.

* **120 FPS:** You can enable 120 FPS via **F10 > Graphics > Frame rate** or in `Config.toml`. This isn't interpolation; the engine actually renders twice as many frames based on a fixed clock. It requires a high refresh rate display and consumes more power, so the default remains 60 FPS.
* **Known Issues:** You may encounter occasional rendering differences compared to original hardware. Online play and WiiConnect24 features are entirely unsupported. Only the USA Rev 1 disc (`R4QE01`) is mapped; other regions and revisions will not work.

### Real Wii Remotes (Experimental)

Real Wii Remotes (with or without Nunchuks) connect directly over Bluetooth HID, utilizing the IR camera and accelerometers exactly as Dolphin does.

* **Pairing:** Sync via the 1+2 buttons (or SYNC) while the game searches, or connect through your operating system's Bluetooth settings.
* **Sensor Bar:** An IR source is required for pointing. If it is mounted above your screen, check **F10 > Wii Remotes (Bluetooth) > Sensor bar is above the screen** for accurate aiming.
* **DolphinBar:** Mayflash DolphinBars must be set to **mode 4** (Wii Remote mode) to function correctly.
* **Limitations:** Native HID access on macOS may fail entirely, and Wii U Pro Controllers are not supported by this backend.

Full guide, including connecting on Windows: [Wii Remotes](https://github.com/BlackAndBlue95/Strikers-WiiCompiled/wiki/Wii-Remotes).

## Controls

This port translates the game's Wii-centric gesture controls back to standard gamepads by reviving the underlying *Super Mario Strikers* GameCube control scheme. Face buttons strictly follow the physical **Nintendo layout** (e.g., the bottom button is always 'B').

* **A (Right):** Pass. With **L**, perform a lob pass. Without the ball, switch players.
* **B (Bottom):** Shoot or hold to charge. Captains execute a Mega Strike. Without the ball, perform a slide tackle.
* **Y (Left):** Deke. Without the ball, perform a big hit.
* **X (Top):** Use item.
* **Triggers/Bumpers:** **R** triggers character special moves. **L** modifies lobs. **Z** (Right Bumper) cycles items.
* **Right Stick / D-Pad:** The right stick dekes in a specific direction, while the D-Pad mirrors original Charged D-pad moves.

You can remap inputs per controller via the **F10** menu, which saves directly to `Config.toml`. Original GameCube controllers work natively via the official Wii U/Switch USB adapter, requiring Zadig WinUSB drivers on Windows.

## Mods & Tweaks

Enhance the game by placing mod packages containing a `mod.toml` manifest into the `Mods` directory. Mods can introduce new characters with custom assets, modify shared files, or add native plugins. Toggle them via **F10 > Mods**.

Quality-of-life adjustments are available under **F10 > Tweaks**:

* **Menu Navigation & Speed:** Navigate menus with the D-pad/sticks instead of a pointer, enable "Fast menus" to skip UI transitions, and "Skip intro" to boot straight to the main menu.
* **No Mega Strikes with Controllers:** *On by default.* Because defending a Mega Strike requires a physical IR pointer, they are disabled for both you and the CPU when using a controller to keep matches playable.
* **Community Fixes:** Toggle options to fix the "NK bug," select home/away kits on the captain screen, enforce "Win by 2" rules, force Blue Peach against red teams, or build all-captain/all-sidekick teams.

## Requirements

* **Game:** A personal dump of Mario Strikers Charged (USA) (Rev 1), `R4QE01`. The build checks the SHA-256 hash and strictly rejects any other version.
* **Hardware:** 64-bit Windows 10/11, Linux, or macOS 14+ (Apple Silicon) with a modern GPU supporting Direct3D 12, Vulkan, or Metal. You need approximately 10 GB of free space.
* **Dependencies:** .NET 8 SDK, CMake 3.25+, Ninja, and Clang. Windows builds explicitly require LLVM-MinGW Clang. Per-platform setup: [Building](https://github.com/BlackAndBlue95/Strikers-WiiCompiled/wiki/Building).

## Building

Clone the repository and run the build script (`./build.sh` on Mac/Linux, `build.cmd` on Windows). Provide your game file when prompted.

The script automatically extracts the image, translates the game code, compiles the native runtime, and links your graphics dependencies. Afterward, the original ISO is no longer needed to play. Saved data, configurations, and caches are stored safely in a dedicated `MSCRecomp` folder based on your OS.

## How the Port Works

While the WiiCompiled translator is game-agnostic, this fork remaps the Mario Kart Wii runtime hooks specifically to Strikers Charged. It utilizes symbol maps and a `datamap.py` script to match guest addresses. Game-specific quirks, like texture details and pointer emulation, are integrated natively into the runtime.

## FAQ & Credits

* **Is this an emulator?** No. Everything is compiled to native code before you play.
* **Can I use other regions?** PAL and Japanese releases are currently unsupported as each version requires a custom address map.
* **Credits:** Built upon [WiiCompiled](https://github.com/patchzyy/wiicompiled), [mscharged-decomp](https://github.com/yannicksuter/mscharged-decomp), [aurora](https://github.com/encounter/aurora), [nod](https://github.com/encounter/nod), and [Dolphin Emulator](https://github.com/dolphin-emu/dolphin). Bundled third-party components and their licenses are listed in [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).

## AI usage

AI coding tools were used heavily in developing this port.

## License

Strikers-WiiCompiled, like WiiCompiled, is free software: you can redistribute it and/or modify it under the terms of the [GNU General Public License, version 3](LICENSE) as published by the Free Software Foundation.

It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

Not affiliated with, endorsed by, or associated with Nintendo. Mario Strikers Charged is a trademark of Nintendo. No Nintendo intellectual property is contained in, distributed with, or obtainable through this project.
