# GameCube controllers

GameCube controllers as players on a Wii and in Dolphin, played as Strikers Recharged plays them
(Recharged has this built in and doesn't need the pack). A GameCube controller in port N plays as
player N whenever no Wii Remote is connected on channel N; a Wii Remote there takes over.

Needs the [Strikers Mod Loader](../loader/README.md).

## Controls

**Menus** (the game's are pointer-only; these move between its buttons):

| Button | Does |
| --- | --- |
| D-pad, a stick flick | move to the next button that way (hold to repeat; the main menu's ring follows the stick) |
| A | select |
| B | back (closes the pause menu) |
| L, R | page (captain select's pages, tabs) |

**Matches:** Super Mario Strikers' GameCube controls, which Charged's engine still has: passing,
shooting, lobbing and switching players are on the buttons SMS had them on. Charged's motion
gestures are buttons again:

| Button | Does |
| --- | --- |
| Y | big hit without the ball, deke with it |
| C-stick | deke |
| Z | cycle item |
| B | slide tackle, while the other team has the ball |
| R | special move |
| Start | pause |

Defending a Mega Strike takes a Wii Remote's pointer, which a GameCube controller can't give
(Strikers Recharged has **No Mega Strikes with controllers** in **F10 > Tweaks** for this). There's
no HOME Button menu from a GameCube controller.

## Building it

```bash
sdk/kamek/fetch-toolchain.sh   # once
packs/gamecube/build-pack.sh out/
```

`out/` then holds `riivolution/gamecube.xml` and `gamecube/sml_10_gamecube.bin`; add the loader and
see [Installing packs](../../docs/modding/installing.md).

## How it works

The game reads its controllers in one place, `PlatPadManager::UpdateChannel`, through the Wii SDK
(`WPADProbe`, `WPADRead`, `KPADRead`). The pack answers those calls there, and only there, for a
channel with a GameCube controller in its port: a Wii Remote with a Nunchuk, held level, the stick
the Nunchuk's, its buttons mapped and a pointer. In matches the player's input becomes the engine's
GameCube kind (`DetInput`), read from the controller itself, and the four actions Charged turned into
gestures read buttons again.

| File | What it does |
| --- | --- |
| `code/gamecube.cpp` | the controllers as the game reads them; Super Mario Strikers' controls in matches |
| `code/navigation.cpp` | menu navigation, as Strikers Recharged's |
