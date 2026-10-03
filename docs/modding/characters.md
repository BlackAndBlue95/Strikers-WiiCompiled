# Characters

A pack can add captains of its own. They get pages of captain select after the game's own (**-** and
**+** switch pages, **L** and **R** with a controller), each on its base captain's button, so any
captain can face them, their base too. [Super Team](../../packs/superteam/README.md) is the example
throughout: Super Mario Strikers' robot team, built on Waluigi.

The character framework is code: it lives in Super Team's pack (`packs/superteam/code`), and a pack
adding characters of its own copies it and lists its own characters (see
[Defining one](#defining-one)).

- [How a character works](#how-a-character-works)
- [Defining one](#defining-one)
- [The files it brings](#the-files-it-brings)
- [Cutscenes](#cutscenes)
- [Limits](#limits)

## How a character works

**It's a character of its own,** after the game's 12 captains: the n-th one a pack adds (from 0) is
team 12 + n, character class 40 + n, goalie 50 + n and sound bank 100 + n, with its own CharacterInfo
row, character template, goalie kit and name key. The framework extends the game's lookups to them
(`slots.cpp`), so wherever the game asks about a team or a character, it gets the right answer.

**It's built on a captain, its base.** Its row and template start as the base's and the pack changes
what it brings: model, textures, animations, stats, name, voice, colour. Its model has to use the
base's skeleton, since the game plays the base's animations on it wherever the character has none of
its own, and the base's cutscenes for any kind it has none of. It plays with the game's default class
behaviour, not its base's special abilities, unless its code adds some.

The framework's files, in `packs/superteam/code`:

| File | What it does |
| --- | --- |
| `characters.h` | what a character can have (`ModCharacter`) |
| `superteam.cpp` | Super Team itself: the pack's list of characters |
| `framework.cpp` | captain select's pages, picking a character |
| `slots.cpp` | the characters' own teams, classes, rows, templates and partner rules |
| `identity.cpp` | its textures under the names the game asks for, its voice bank |
| `art.cpp` | front-end screens: pictures and slides the game's files only have for its 12 captains |
| `cutscenes.cpp` | its cutscenes, its base's for the rest, their triggers |
| `effects.cpp` | its effects bundle, and its effects by name falling back to its base's |
| `partners.cpp`, `team.cpp` | partner select, and teams that are always the character (Super Team's three robots) |
| `deke.cpp`, `voice.cpp` | the generic deke instead of its base's, its voice on captain select |

## Defining one

A pack lists its characters in `kModCharacters` (Super Team's: `superteam.cpp`), each a
`ModCharacter` (`characters.h`):

| Field | What it is |
| --- | --- |
| `name` | its internal name (`"superteam"`): its files are named after it |
| `base` | the captain it's built on: CharacterInfo row 0-11 (Mario 0, Bowser 1, Daisy 2, DK 3, Luigi 4, Peach 5, Waluigi 6, Wario 7, Yoshi 8, Bowser Jr 9, Diddy Kong 10, Petey 11) |
| `model`, `textures`, `animations` | its `.rlg`, `.rlt` and `.sanim.zlib` (0 keeps the base's) |
| `stats`, `statsGameplay` | its two stats files (0 keeps the base's) |
| `portrait`, `portraitTaken` | captain select's portrait, lit and greyed out |
| `displayName`, `nameKey` | its name as players see it, and the string key it goes under |
| `colour`, `statsBars`, `role`, `noAbility` | team colour, captain select's four bars, role, and no super ability |
| `goalieKit`, `goalieTextures` | its goalie kit's name and textures |
| `voiceBank` | its sound bank (`audio/<bank>.resbun` and `.nlxwb`) |
| `ownTextures` | textures the game asks for by its name, from `.gxt` files in the pack |
| `fixedTeam` | its team is always itself, whatever partner select picked |
| `extraEffects` | an effects bundle of its own, loaded with it |
| `triggerAliases`, `nisTriggers` | its cutscenes' triggers ([Cutscenes](#cutscenes)) |
| `defaultDeke` | the generic deke (Mario's, the game's default) instead of its base's |

## The files it brings

The game finds most files by a character's name, so a character brings its own under its name:

| What | Where |
| --- | --- |
| model and textures | `Art/characters/<name>/<name>.rlg`, `.rlt` (the model's id must be `nlStringHash("<name>/<name>")`) |
| animations | `Art/animation/<name>.sanim.zlib` |
| team art | `Art/characters/<name>/ExtraTextures.rlt`: emblem, banners, Mega Strike backdrop (`<name>/mega_cone_colour`, `<name>/mega_gameplay_bg`) |
| menu and HUD art | `ownTextures`: logos, the HUD icon, the Striker Times' photos, the lower third (`fe/screens/images/logos_TEAM_<name>`, ...) |
| crowd | `ini/CrowdCharacterLists/<name>.ini` |
| hologram framing | a `[<name>]` section of `ini/ImpostorCharacterTweaks.ini` |
| cutscenes | `Art/nis/<name>_<kind>.nis`, with their entries in `Art/nis/nis_dict.txt` |
| effects | its `extraEffects` bundle; effects the game names after the character (`<name>_megastrike_home_3_gameplay`, `<name>_mega_bg`) are its own if the bundle has them, else its base's |

Where a screen looks for a picture or a slide named after the character that the game's files
don't have, `art.cpp` gives it the character's from the pack, or its base's slide with the
character's name and pictures in it.

## Cutscenes

A character's cutscenes are `<name>_<kind>` in the cutscene dictionary
(`superteam_goal_winner_high_0`, `superteam_megastrike_home_2`, ...). A kind it has none of plays its
base's, whose animations fit its skeleton.

Each cutscene runs the trigger script named after it, from `Art/scripts/nis_triggers.byte_code`:
effects, sounds and slow motion at given frames. A character's cutscenes can run another's script
(`triggerAliases`: Super Team's run the scripts Charged kept from its own unfinished port of the
team) or add triggers of their own (`nisTriggers`), which is how cutscenes the game has no script for
get their effects:

```cpp
const ModNisTrigger kNisTriggers[] = {
    // cutscene ('*' for any text)  frame  type              effects group           where
    {"superteam_megastrike_*_2",    2,     kNisEffect,       "superteam_sts_vortex", "bip01", 0},
    {"superteam_megastrike_*_2",    33,    kNisTimeDilation, 0,                      0,       0.4f},
};
```

An effect plays on the character (`bip01`), the ball (`ball`) or one of the cutscene's props
(`nis_ball01`, `effects_mesh`). A cutscene's audio is the stream named after it in the cutscene
stream bank (`audio/STREAM_GEN_NIS`: `<cutscene>`, and `<cutscene>_nocrowd` in stadiums without a
crowd), so a cutscene of the character's own plays silent until its pack adds a stream for it
(`scripts/modkit/merge_stream_bank.py`).

Super Team's Mega Strike is a worked example: four cutscenes made from Waluigi's shots with SMS's
Super Strike animation (`scripts/modkit/sms/megastrike.py`), SMS's effects for them
(`packs/superteam/fx/megastrike.fx`), and the triggers that play them (`superteam.cpp`).

## Limits

| What | Limit | Where |
| --- | --- | --- |
| characters | 4 | `slots.cpp`, `kMaxCharacters` |
| captain select pages | 4 | `framework.cpp`, `kMaxPages` |
| textures by name | 32, in 192 KB | `identity.cpp`, `kMaxSwaps`, `s_memory` |
| cutscenes | 512 in the dictionary, 457 of them the game's | the game's `NisPlayer` |
