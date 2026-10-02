# Changelog

## 0.1.0 — first release

Strikers Recharged is a native PC port of **Mario Strikers Charged** (USA, `R4QE01`), made by static
recompilation with WiiCompiled. Bring your own copy of the game: the build turns it into a native
program for Windows, macOS or Linux. See the [README](README.md) and the
[wiki](https://github.com/BlackAndBlue95/Strikers-WiiCompiled/wiki) for building and setup.

### Playing

- One-command builds for Windows, macOS and Linux, straight from a disc image.
- Super Mario Strikers' GameCube controls on any controller, with Charged's extras (special moves,
  D-pad moves) on top; buttons rebindable per controller; keyboard and mouse.
- GameCube controllers through the Wii U / Switch adapter, each adapter pad told apart by its slot.
- Real Wii Remotes over Bluetooth (IR pointer, Nunchuk, rumble, DolphinBar), with the Wii's sensor
  bar position and IR sensitivity settings.
- Menus you can drive with a stick or D-pad.

### Looking and sounding

- Any internal resolution up to 8x, widescreen that fills any window shape, exclusive fullscreen.
- Real frame rates of 30, 60, 120, 144, 160 or 165 FPS, or the display's own.
- Dolphin-style options: Scaled EFB copies, the copy filter, custom textures (Dolphin packs) and
  texture dumps.
- Volume for music, sound effects, voices, menus and cutscenes, on top of the game's own sliders.

### Tweaks and mods

- Optional tweaks: fast menus, skip intro, unlock everything, win by 2, the NK bug fix, home/away
  kit choice, captain-only teams, all stadiums fast-paced, and more.
- Mod packages (files, catalogs, mod characters) and Riivolution file patches.
- Miis: copy Dolphin's Mii database or import an `RFL_DB.dat`.
- Everything is in one settings window (F10), saved to `Config.toml`.

### Fixes since the port began

Blue Peach's match load, lighting on the ball and stadium lights, effects in Stormship's lightning,
heat haze and impact effects blurring above 1x, the Hall of Fame's dates, help popups showing empty
with fast menus, and more.

### Known issues

- Some rendering differs from the original hardware.
- Online play and WiiConnect24 aren't supported.
- Only the USA disc (`R4QE01`) is mapped.
- On Windows, borderless fullscreen can hitch briefly after tabbing back in.
