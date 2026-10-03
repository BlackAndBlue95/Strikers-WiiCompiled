# Modding Strikers Recharged

Mods are [Riivolution](https://wiibrew.org/wiki/Riivolution) packs: the format Wii mods have long
used, which Dolphin supports too. One pack runs on a Wii, in Dolphin and in Strikers Recharged. A pack
can:

- **replace or add game files**: models, textures, menus, sounds, INI files;
- **add code**: Kamek modules, PowerPC code compiled against the game's own classes, which hooks
  into the game ([code mods](../../sdk/kamek/README.md)).

The disc image itself is never patched: Riivolution lays the pack over it at launch.

> [!IMPORTANT]
> Packs must not contain Nintendo's game files. A pack that needs content from a game (a model from
> Super Mario Strikers, say, or a Charged file it changes) comes with a script that builds the pack
> from the player's own copy, so it is only ever built locally. `packs/superteam/build-pack.sh` is
> one.

## Installing a pack

A pack is laid out like an SD card: a `riivolution` folder with the pack's XML, and the pack's own
folder. Copy both into the `Riivolution` folder of the data folder:

| Platform | Riivolution folder |
| --- | --- |
| Windows | `%LOCALAPPDATA%\MSCRecomp\Riivolution` |
| Linux | `$XDG_DATA_HOME/MSCRecomp/Riivolution` (default `~/.local/share/MSCRecomp/Riivolution`) |
| macOS | `~/Library/Application Support/MSCRecomp/Riivolution` |

`[paths] overlay_roots` in `Config.toml` adds more folders laid out the same way (code mods are only
picked up from the default folder).

**F10 > Mods** lists the packs found and each pack's options. A choice is saved to the folder's
`riivolution/config/R4QE.xml`, the file Riivolution and Dolphin use, and applies the next time the
game starts. `console.log` (in the `Logs` folder) says what each pack did, on lines starting with
`[riivolution]`.

**Code mods are built into the game.** After installing, updating or removing a pack with code, or
turning its option on or off, run `build.sh` (Windows: `build.cmd`) again. F10 and `console.log` say
when the installed packs no longer match what was built in.

## Making a pack

The XML says what the pack's options are and what each one does:

```xml
<wiidisc version="1" root="/mypack">
  <id game="R4Q"/>
  <options>
    <section name="My pack">
      <option name="New menus" id="mypack_menus" default="1">
        <choice name="Enabled"><patch id="menus"/></choice>
      </option>
    </section>
  </options>
  <patch id="menus">
    <!-- mypack/files is laid out like the disc: files/Art/fe/... replaces the disc's Art/fe/... -->
    <folder disc="/" external="files" create="true"/>
  </patch>
</wiidisc>
```

- `<file disc="..." external="..."/>` replaces one disc file (`create="true"` adds a new one);
  `<folder>` does a whole folder.
- `external` paths are relative to the patch's `root` (the pack's folder here); a leading `/` makes
  them relative to the SD card (the `Riivolution` folder).
- An option's `default` is the choice it starts on (1 is the first choice, 0 is off).
- `<memory>` patches (changing the game's code or data at an address) are read but not applied yet;
  use a code mod.

Riivolution's own documentation covers the rest of the format.

### Code

A code mod is a Kamek module placed at the disc root as `sml_<NN>_<pack>.bin`: see
[sdk/kamek](../../sdk/kamek/README.md) for writing, building and testing one. Strikers Recharged
builds the modules in ahead of time. On a Wii and in Dolphin the Strikers Mod Loader loads them: a
pack of its own (`packs/loader`, built by its `build-pack.sh`) that Riivolution writes into memory
the game doesn't use (MetroTRK's debugger table, 0x80004060) and calls in place of nlInit's call to
nlInitFileSystem. Once the file system is up it loads every `sml_*.bin` at the disc root, in name
order, with Kamek's loader (memory from the game's heap). Strikers Recharged ignores its patches, so
the same packs work everywhere: put the loader pack next to the code packs.

To run packs in Dolphin, copy them (the `riivolution` folder's XML files and the packs' folders) into
Dolphin's `Load/Riivolution` folder and start the game with **Start with Riivolution Patches**, with
the Strikers Mod Loader and the packs enabled. A game modification descriptor does the same from the
command line (`Dolphin -e superteam.json`): a JSON file with `"type": "dolphin-game-mod-descriptor"`,
`"version": 1`, the game as `"base-file"` (a disc image, or an extracted game's `sys/main.dol`), and
under `"riivolution": {"patches": [...]}` each pack's `"xml"`, `"root"` and chosen `"options"`.

`packs/gamecube` is a code pack for consoles: GameCube controllers on a Wii or in Dolphin, played as
Strikers Recharged plays them (a controller in port N is player N when no Wii Remote is on channel N;
menus navigated with the D-pad; Super Mario Strikers' controls in matches). Recharged has this built
in and doesn't need it.

A code mod runs on a console without Strikers Recharged's own fixes, so it brings what it relies on.
Super Team's team of captains does: the game loads a captain's voice into the team captain's slot,
waits for partners' cutscene companions and for walk-out animations of the partners picked, none of
which a team of captains has (`packs/superteam/code/team.cpp`, `cutscenes.cpp`).

### Characters

A character a pack adds is a character of its own, after the game's 12 captains: the n-th is team
12 + n, character class 40 + n and goalie 50 + n, with its own CharacterInfo row, character template,
goalie kit, sound bank and name key. Its code (`packs/superteam/code`, the Super Team pack) extends
the game's lookups for them (`slots.cpp`) and puts the pack's characters on pages of captain select
of their own, after the captains: `-` and `+` switch pages. So any captain can face it, its base
too (Waluigi vs Super Team). Each character is built on a captain, its base: its row and template
start as the base's, and its model uses the base's skeleton and animations, and the base's
cutscenes for any kind it has none of. `characters.h` describes what a character can have;
`superteam.cpp` is Super Team's.

The game finds most files by a character's internal name, so a character brings its own under its
name: textures (HUD icon, logos, menu art, the Striker Times' photos, as `ownTextures`), cutscenes
(`<name>_<kind>` in the cutscene dictionary), team art (`ExtraTextures.rlt` next to its textures),
its crowd (`ini/CrowdCharacterLists/<name>.ini`) and its hologram framing (a `[<name>]` section of
`ini/ImpostorCharacterTweaks.ini`). Where a screen looks for a picture or a slide named after it that
the game's files don't have, the framework (`art.cpp`) gives it the character's from the pack, or
the base's slide in the character's name and pictures. Effects the game names after the character
(`<name>_megastrike_home_3_gameplay` on the balls of its Mega Strike, `<name>_mega_bg`) are its own
when its effects bundle has them, else its base's.

A cutscene runs the trigger script named after it (`Art/scripts/nis_triggers.byte_code`): effects,
sounds, slow motion at given frames. A character's cutscenes can run another's script
(`triggerAliases`) and add triggers of their own (`nisTriggers`), which is how cutscenes the game has
no script for get their effects. Super Team's Mega Strike is one: its four cutscenes
(`superteam_megastrike_<side>_<n>`) are Waluigi's shots with SMS's Super Strike animation, and SMS's
effects (the vortex, the kick, the balls' fireballs) play on their triggers.
