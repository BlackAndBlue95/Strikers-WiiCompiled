# mod.toml reference (framework 1)

The manifest is [TOML](https://toml.io). Unknown keys are reported as warnings, so a typo doesn't go
unnoticed. Problems that stop the mod from loading are errors; both show on the F10 Mods page and in
`console.log`.

## `[mod]` (required)

| Key | Type | Required | Meaning |
| --- | --- | --- | --- |
| `id` | string | yes | The mod's identity: lower-case `a-z`, `0-9`, `.`, `_`, `-`, up to 64 characters, e.g. `sms.superteam`. Settings are stored under it, so keep it stable across versions. Must be unique among installed mods. |
| `name` | string | yes | Shown on the Mods page. |
| `framework` | integer | yes | The manifest format the mod is written for. This build reads `1`; a mod asking for a newer one isn't loaded and asks the player to update. |
| `version` | string | no | Shown on the Mods page. |
| `authors` | string or array | no | Shown on the Mods page. (`author` works too.) |
| `description` | string | no | One or two sentences for the Mods page. |
| `load_order` | integer | no | Default `0`. Mods load in ascending `load_order`, then by `id`; when two mods ship the same file, the one loaded later wins. |

## `[plugin]` (optional)

The mod's native plugin, per platform. Paths are relative to the mod folder. Only the current
platform's entry is used; a missing file is an error.

| Key | Meaning |
| --- | --- |
| `macos` | e.g. `"plugin/libmymod.dylib"` |
| `windows` | e.g. `"plugin/mymod.dll"` |
| `linux` | e.g. `"plugin/libmymod.so"` |

The plugin loads only after the player allows the mod's native code on the Mods page.

## `[[character]]` (optional, any number)

A mod character: a named variant of one of the game's characters. [characters.md](characters.md)
describes how they work and every file they can have.

| Key | Type | Required | Meaning |
| --- | --- | --- | --- |
| `name` | string | yes | Internal name: 1–15 of `a-z`, `0-9`, `_`. Every name-keyed asset the game loads for this character uses it. Can't be a game character's name or another mod's character. |
| `kind` | string | yes | `"captain"`. (`"partner"` is read, with a warning, but partner characters aren't used yet.) |
| `base` | string | yes | The game character it plays as: `mario`, `bowser`, `daisy`, `donkeykong`, `luigi`, `peach`, `waluigi`, `wario`, `yoshi`, `bowserjr`, `diddykong`, `petey`. |
| `display_name` | string | no | Localisation key of its name, e.g. `NAME_SUPERTEAM`; give it a string in `catalogs/loc`. Without one, its base's name shows. |
| `voice_bank` | string | no | Its sound bank: `audio/<voice_bank>.resbun` and `.nlxwb`. |
| `goalie_kit` | string | no | Its team's goalie kit: `art/characters/<goalie_kit>/<goalie_kit>.rlt`. |
| `stats` | string | no | Its stats INI, e.g. `ini/characters/superteam.ini` (Charged's format; see the game's own). Without one, its base's. |
| `colours` | table | no | `{ primary = 0xRRGGBB, alternate = 0xRRGGBB }`: its team colour (HUD markers, menus). With its own colour it never counts as clashing with its base. |
| `stats_bars` | array | no | Captain select's four bars, 0–1: `[movement, shooting, passing, defense]`. Without them, its base's. |
| `role` | string | no | Captain select's role: `offensive`, `defensive`, `playmaker`, `power` or `balanced`. |
| `ability` | string | no | `"base"` (default): its base's super ability. `"none"`: its team is never given a super ability. |
| `deke` | string | no | `"base"` (default): its base's deke. `"default"`: the generic directional deke Mario, Luigi and Yoshi use; give the character its own animations with `deke_300` (sideways; mirrored for the other side), `deke_600` (back) and `deke_1200` (forward). `"plain"`: its base's deke without the teleport (bases with a teleporting deke). |
| `teammates` | array | no | Up to 3 character names (mod or game characters): its teammates with `fixed_team`, and its default picks when captain-only teams is on. |
| `extra_effects` | array | no | Effects bundles loaded with its own (disc paths such as `"art/effects/myeffects.bun"`; a matching `myeffectsnonres.bun.zlib` is loaded with it). For effects its base's bundle lacks, e.g. ones its cutscenes' trigger scripts name. |
| `fixed_team` | bool | no | `true`: its team is always it and its `teammates` (copies of itself for any slot `teammates` leaves out), with or without captain-only teams, whatever partner select picks; partner select shows them in its slots. As SMS's Super Team. |

## Example

SMS's Super Team, the first mod built on the framework:

```toml
[mod]
id = "sms.superteam"
name = "Super Team"
version = "1.0.0"
framework = 1
authors = ["Bowser"]
description = "The robot team from Super Mario Strikers, built from your own SMS disc."

[[character]]
name = "superteam"
kind = "captain"
base = "waluigi"
display_name = "NAME_SUPERTEAM"
voice_bank = "CHAR_SUPERTEAM_Sfx"
goalie_kit = "superteamgoalie"
stats = "ini/characters/superteam.ini"
teammates = ["superteam", "superteam", "superteam"]
fixed_team = true                                    # always three robots, as in SMS
extra_effects = ["art/effects/superteam_sms.bun"]    # its SMS cutscene effects
colours = { primary = 0x3A6EA5 }
stats_bars = [0.6, 0.6, 0.6, 0.6]
role = "balanced"
ability = "none"                                     # no Waluigi wall
deke = "default"                                     # a spin deke, not Waluigi's teleport
```

## Settings the runtime keeps

The player's switches live in `Config.toml`, keyed by mod id:

```toml
[mod_packages]
"sms.superteam" = true    # loaded

[mod_plugins]
"sms.superteam" = false   # native plugin not allowed

[mod_selection]
home = "superteam"        # the mod character leading each side, picked on captain select
away = ""
```
