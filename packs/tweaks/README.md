# Strikers Tweaks

Strikers Recharged's gameplay and menu tweaks as a pack, each one an option of its own: the same
tweaks, and the same options, in Strikers Recharged, in Dolphin and on a Wii.

| Option | Default | Does |
| --- | --- | --- |
| Tweaks | on | the pack's code: the tweaks below work while this is on |
| Skip intro | off | boots straight to the main menu: no notice screens, intro movie or title screen (the studio logo still shows) |
| Fast menus | off | skips menu transitions: panels sliding in and out, camera moves such as the zoom from the main menu |
| All stadiums fast-paced | off | every pitch plays like the fast, dry ones such as Bowser Stadium |
| No Mega Strikes with controllers | on | while a controller plays, matches have Mega Strikes off for both sides (defending one takes a Wii Remote's pointer) |
| Unlock everything | off | all characters, stadiums and cheats, through the game's own unlock-all switch; the save isn't changed |
| Win by 2 | off | first-to-X goal matches: tied one goal short of the target, the target moves up |
| Fix the NK bug | on | a deke or teleport through the goalie cut short no longer leaves every shot passing through Kritter |
| Blue Peach against red teams | off | Peach wears her blue kit against red captains |
| Shot counter on the results screen | off | the Mega Strike row shows white and yellow shots / red and orange shots |

On a Wii and in Dolphin it needs the [Strikers Mod Loader](../loader/README.md). "A controller" for
No Mega Strikes is a gamepad or the keyboard in Strikers Recharged, and a GameCube controller with
the [GameCube controllers](../gamecube/README.md) pack.

## In Strikers Recharged

`build.sh` installs the pack in the data folder's `Riivolution` folder and builds its code in: its
options are in **F10 > Mods**, and **a change applies at once**, while the game runs. The tweaks
that were in **F10 > Tweaks** before became this pack's options, set as they were.

On a Wii and in Dolphin a change applies at the next launch, as Riivolution's options always do.

## How it works

Turning a tweak on or off never changes the code. Each tweak's option only puts a small file on the
disc, `/tweaks/<name>`, and the code looks for those files (`DVDConvertPathToEntrynum`) when it
starts and every half second after. On a console the disc is put together once, at launch.
Strikers Recharged keeps every option's files on the disc and hides the ones whose option is off,
so F10 can turn a tweak on or off while the game plays: any pack option whose choices only add files
works that way (see [Making a pack](../../docs/modding/making-packs.md#options-that-apply-at-once)).

| File | What it does |
| --- | --- |
| `code/options.cpp` | reads the options |
| `code/frame.cpp` | the tweaks that act once a frame, from the game's main loop (`nlTaskManager::RunAllTasks`) |
| `code/menus.cpp` | fast menus and skip intro, on the front end's slides, presentations and boot screens |
| `code/fixes.cpp` | Peach's away kit, which the game never uses: her home kit's crowd, sidekick portraits, banners and Mega Strike hand where the away kit has none (without them the match load and sidekick select crashed, and the banners were black) |
| `riivolution/tweaks.xml` | the options; each one's `description` is what F10 shows under it |
| `sml_20_tweaks.bin` | the built code |

## Building it

The built code is kept in the repository, so building Strikers Recharged doesn't take the Kamek
toolchain. After changing the code:

```bash
sdk/kamek/fetch-toolchain.sh          # once
packs/tweaks/build-pack.sh --code out/
```

`--code` compiles `code/*.cpp` into `sml_20_tweaks.bin` (commit it with the change); without it the
script only lays the pack out. `out/` then holds `riivolution/tweaks.xml` and `tweaks/`; add the
loader for a Wii or Dolphin, see [Installing packs](../../docs/modding/installing.md).
