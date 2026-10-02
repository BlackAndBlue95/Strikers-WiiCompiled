# Mod characters

A mod character is a named variant of one of the game's captains. Its **base** decides how it plays:
skeleton, physics, super ability, deke, AI. Its **name** decides what it looks and sounds like,
because Charged picks almost every per-character asset by the character's internal name (`waluigi`,
`mario`, ...). Give a character the name `superteam` and base `waluigi`, and wherever the game would load
something called `waluigi`, it loads the `superteam` version, if the mod supplies it, or Waluigi's if it
doesn't. A mod can start with just a model and add the rest over time; nothing breaks for a missing file.

Characters are declared in `mod.toml` ([manifest.md](manifest.md)). They're picked on captain select:
**-** and **+** (**L/R** on a controller) page through the captains, the partners (with captain-only teams
on) and the mod captains, each on its base's button. A character and its base can play on one team or
against each other.

**Teams.** A mod captain's teammates are whatever partner select picks, like any captain's. With
`fixed_team = true` its team is always itself and its `teammates` (copies of itself by default), as SMS's
Super Team was: partner select shows them on its board, and the match puts them on the pitch whatever
was picked. With captain-only teams on, `teammates` are its default picks instead.

## What a character can have

Paths are disc paths: put the files under the mod's `files/` folder at that path. `<name>` is the
character's internal name, `<base>` its base's.

### In a match

| Asset | File | Notes |
| --- | --- | --- |
| Model | `art/characters/<name>/<name>.rlg` | Rigged to the base's skeleton. Its model id must be `nlStringHash("<name>/<name>")`, as the game's own are. |
| Textures | `art/characters/<name>/<name>.rlt` | The textures its model names. |
| Shadow, shock, low-poly models | `<name>_shadow.rlg`, `<name>_shock.rlg`, `<name>_lowpoly.rlg` | Else the base's. |
| Away kit | `art/characters/<name>/<name>_alt.rlt` | Swaps `<name>/<name>` for `<name>_alt/<name>_alt`. |
| Animations | `art/animation/<name>.sanim.zlib`, `art/animation/<name>.trg` | Its own copy of the base's animation set: the base's skeleton and animation names (gameplay looks them up by name). The `.trg` holds their triggers (kick contact, footsteps); keep the base's timings, or ship your own. Else the base's. |
| Team art | `art/characters/<name>/ExtraTextures.rlt` | `<name>/<name>_logo`, `<name>/<name>_banners`, `<name>/mega_gameplay_bg`, `<name>/mega_cone_colour`, `<name>/mega_hand`, `<name>/mega_hand1`. Without one, the base's is loaded under the character's names (and still answers to the base's, for the base's effects). |
| Effects | `art/effects/<name>Effects.bun` (+ `<name>EffectsNonRes.bun.zlib`) | Replaces the base's bundle. To keep the base's and add your own, use the manifest's `extra_effects` instead. See *Effects* below. |
| Stats | the manifest's `stats` | Charged's INI format (`ini/characters/*.ini`). |
| Goalie kit | the manifest's `goalie_kit` | `art/characters/<kit>/<kit>.rlt`, texture `<kit>/<kit>`. |
| Partner kits | `art/characters/<partner>/<prefix>_<name>.rlt` | The team's partners' kits (`toad_superteam.rlt`, ...; prefix `hammer` for Hammer Bro). Else the base's. |
| Voice | the manifest's `voice_bank` | A sound bank answering Charged's character cues (`CHAR_CAPTAIN_*` and their sidekick versions). Its voice-preview cue (`0x270203ED`) is also what the character says when picked on captain select. |
| Crowd | `ini/CrowdCharacterLists/<name>.ini`, `<name>Alt.ini` | Else the base's. |
| Cutscenes | `art/nis/<name>_<type>_<n>.nis` + `catalogs/nis` entries | Goal celebrations (`goal_winner_high`, `goal_winner_low`), intros (`home_capt_intro_1`..`3`, `away_capt_intro_1`..`2`), `end_of_game_holotron_home`, `megastrike_home`, ... One of a type is picked at random by name prefix; any type without one of the character's own plays the base's. See *Cutscenes* below. |
| Cutscene audio | `catalogs/streams/STREAM_GEN_NIS/<cutscene>.idsp` | The cue a cutscene plays (music, voice, crowd in one stream); see *Catalogs*. |

### Menus and screens

Front-end textures go in the bundles through catalogs (below), named `fe/screens/images/<file>` unless
noted:

| Asset | Texture | Bundle | Where |
| --- | --- | --- | --- |
| Captain select portrait | `captain_<name>_s`, greyed `captain_<name>_ds` | `mainui.dmn` | The mods page of captain select, the post-match summary. 128×128. |
| Stats panel portrait | `attributes_<name>` | `mainui.dmn` | Captain select's panel. 128×256. |
| Board heads | `<name>_right`, `<name>_left` | `mainui.dmn` | Partner select's boards. 64×64, transparent. |
| Team portrait | `logos_TEAM_<name>` | `gameloadingui.res`, `ingameui.dmn`, `mainui.dmn` | Match loading screen and other team screens. 128×128. |
| HUD icon | `fe/captain_icons/captain_icons_<name>` | `captainiconsui.res` | The in-match HUD. 128×128. |
| Banner emblem | `lowerthird_<name>` | `ingameui.dmn` | The goal and final-score banners' team emblem. 128×64, the banner's shape (transparent right end). |
| Name | the manifest's `display_name` | `catalogs/loc` | HUD, overlays, results, menus. |
| Hologram framing | `[<name>]` in `ini/ImpostorCharacterTweaks.ini` | `catalogs/ini` | Choose sides. Copied from the base's section when the mod has none. |

Screens find their art by name, as slides (a component's `<name>` slide) and image instances
(`logos_TEAM_<name>`). A character has no slides of its own in the game's screens, so those fall back to
its base's, shown as the character's: the slide's name text says the character's name, and its images
of the base's named textures above show the character's where the mod adds them.

## Cutscenes

A cutscene is a `.nis` (the characters' and cameras' animation) plus three things looked up by its name:

- its **dictionary entry** (`catalogs/nis/nis_dict.txt`, the game's format: `name <file>.nis` then its
  header values, as in `Art/nis/nis_dict.txt`). Every `.nis` the mod adds needs one;
- its **trigger script**, the function named after it in `Art/nis/nis_triggers.byte_code`, else
  `all_<type>_<n>`'s: effects, sounds and crowd at given frames. A mod can't add scripts yet, so
  `catalogs/nis/triggers.txt` points a cutscene at an existing one;
- its **audio cue** in `STREAM_GEN_NIS`, `<cutscene>` (or `<cutscene>_nocrowd` in stadiums without a
  crowd). A mod adds it as a stream (`catalogs/streams`), or points at an existing cue
  (`catalogs/nis/cues.txt`). Intros start their cue on the first part (`*_capt_intro_1`) and play it
  through the others.

```
# catalogs/nis/triggers.txt
superteam_end_of_game_holotron_home = mystery_end_of_game_home_0
```

Without a script or a cue of its own, a cutscene runs the generic `all_` script, if any, and is silent.
Effects a script names that no loaded bundle has are skipped (and reported in the log).

## Effects

An effects bundle holds effect groups (a group plays templates at joints, with offsets) and templates
(the particles). Effects are found by name, and the game names some after the character
(`<name>_megastrike_home_3_gameplay`, `<name>_mega_bg`, ...): one no loaded bundle has falls back to the
base's (`<base>_...`), which the base's bundle has.

Bundles in `extra_effects` load after the character's own (its base's, unless it ships
`<name>Effects.bun`), each with its `<x>nonres.bun.zlib` (textures) when there is one, once a match.
Don't name an extra bundle `<name>Effects.bun`: that name replaces the base's bundle instead.

Gameplay effects face the way the character does; cutscene effects start facing straight up. An emitter
whose offsets and shape assume the character's frame (y up, z back) lands sideways in a cutscene unless
it faces along its joint: an `EffectsSpec`'s axis field (+0x48, 1-6 = +X +Y +Z -X -Y -Z of the joint)
makes it follow that joint every frame. On the game's characters the joint axis pointing forward is
+Y for the spine, pelvis, legs and feet, -Y for the forearms and +X for the toes.

## Catalogs

Some game files are shared tables: one file for every character. A mod adds entries to them instead
of replacing them, from its `catalogs/` folder; the runtime rebuilds each table at launch (into the cache
folder) with every active mod's entries.

```
catalogs/
  fe/<bundle>/<path>.gxt          a texture added to Art/fe/<bundle>, named "fe/<path>"
  loc/all.txt                     strings for every language
  loc/<language>.txt              strings for one language (english, french, german, ...), over all.txt
  ini/<path>.ini                  appended to ini/<path>.ini
  nis/nis_dict.txt                appended to Art/nis/nis_dict.txt
  nis/triggers.txt                "<cutscene> = <other>": the cutscene runs the other's trigger script
  nis/cues.txt                    "<cutscene> = <other>": the cutscene plays the other's audio cue
  streams/<bank>/<cue>.idsp       a streamed sound added to audio/<bank> as the cue <cue>
  streams/<bank>/cues.txt         "<other cue> = <cue>": more names for a stream; "template = <cue>"
```

- **Textures** (`.gxt`): a texture as the game's bundles store it, its 32-byte GX header then the image
  data (CMPR, RGB5A3, C8, ...). `catalogs/fe/mainui.dmn/screens/images/captain_superteam_s.gxt` adds
  `fe/screens/images/captain_superteam_s` to `Art/fe/mainui.dmn`. A name the bundle already has is replaced.
- **Strings**: UTF-8 lines `KEY = text`; `#` starts a comment.

  ```
  NAME_SUPERTEAM = SUPER TEAM
  ```
- **Streamed sounds** (`.idsp`): one stream as the bank's `.nlxwb` stores it: `IDSP`, the bank's
  interleave (`0x6B40` in `STREAM_GEN_NIS` and `STREAM_GEN_Music`, `0x35A0` in `STREAM_GEN_Crowd`), two
  0x60-byte DSP-ADPCM channel headers, then the two channels' blocks interleaved, padded to a multiple of
  32 bytes. Any sample rate. The cue's settings (category, volume group, looping) are copied from the
  bank's first cue, or from the one `template = <cue>` names. The streams are appended to the bank's
  `.nlxwb` virtually: the 455 MB file isn't copied.

## Limits

- **Kind:** only `kind = "captain"`. Partner characters are read but not used yet.
- **Gameplay comes from the base:** super ability, deke, AI and Mega Strike behaviour follow the base.
  Two switches turn the base's specials off ([manifest.md](manifest.md)): `ability = "none"` (the team is
  never given a super ability) and `deke = "default"` (the generic directional deke, from the character's
  own `deke_*` animations) or `"plain"` (its base's deke without the teleport). A special of the
  character's own isn't possible yet.
- **Animations follow the base's skeleton and names:** a character's own animation file replaces the
  base's set as a whole, for that character only.
- **Trigger scripts:** a cutscene can only use one of the game's (by alias).
- **Memory:** a character playing alongside its base loads a second copy of its templates (model,
  physics). Every player who is a captain loads their own voice bank (a fixed team loads four), and sound
  banks share the game's 8 MiB audio memory, so keep banks small.
