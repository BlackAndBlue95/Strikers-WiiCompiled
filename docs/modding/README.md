# Modding Strikers-WiiCompiled

Mods are packages: a folder with a `mod.toml` manifest and the files the mod adds. The runtime finds
them at launch, checks them, and adds their files to the game's file table, so the game loads them
the same way it loads its own data. No disc image is patched.

> [!IMPORTANT]
> Mods must not contain Nintendo's game files. A mod that needs content from a game (a model from
> Super Mario Strikers, say) should come with a script that builds it from the player's own copy, so
> it is only ever built locally. The modkit tools (next on the roadmap, below) are for exactly that.

## Installing a mod

Put the mod's folder (the one holding `mod.toml`) in the `Mods` folder of the data folder:

| Platform | Mods folder |
| --- | --- |
| Windows | `%LOCALAPPDATA%\MSCRecomp\Mods` |
| Linux | `$XDG_DATA_HOME/MSCRecomp/Mods` (default `~/.local/share/MSCRecomp/Mods`) |
| macOS | `~/Library/Application Support/MSCRecomp/Mods` |

`[paths] mods_dir` in `Config.toml` moves it (a relative path is relative to `Config.toml`).

Press F10 and open **Mods** to see what was found. Each mod has a switch. Mods are on when installed;
changes apply the next time the game starts. The page also shows:

- **errors** — the mod isn't loaded (a broken manifest, a newer framework version than this build reads,
  a character name another mod already uses, and so on);
- **warnings** — it loaded, but something in it looks wrong (an unknown manifest key, for example);
- **game files replaced** — see *Adding and replacing files* below;
- **native code** — the mod includes a plugin (see below), which stays off until you allow it.

Everything the runtime decides about mods is also written to `console.log` in the `Logs` folder, on
lines starting with `[mods]`.

## Making a mod

```
Mods/
  my.mod/
    mod.toml        the manifest (manifest.md)
    files/          disc-shaped: files/Art/... is the game's Art/... folder
```

A minimal `mod.toml`:

```toml
[mod]
id = "my.mod"
name = "My mod"
version = "1.0.0"
framework = 1
authors = ["You"]
description = "What it does, in a sentence."
```

`manifest.md` describes every key.

### Adding and replacing files

Every file under `files/` is added to the game's file table at its path: `files/Art/fe/mymenu.res`
becomes `Art/fe/mymenu.res`. A path the game already has replaces the game's file. That is a **global
override**: it applies to every match and menu, whoever is playing. The Mods page and the log list
each one, because two mods replacing the same file can't both win (the mod loaded later does: higher
`load_order`, then id).

Prefer adding files under new names. Characters (below) are built that way: a mod character's files
use its own name, so nothing of the game's is replaced.

## Characters

A mod character is a named variant of one of the game's characters: `base` decides how it plays
(skeleton, physics, super ability, deke, AI), `name` decides everything it looks and sounds like.
Charged picks almost every per-character asset by the character's internal name, `waluigi` for example:
model, textures and animations, team logo and banners, HUD icon, cutscenes and their audio and effects,
menu portraits, voice. A character named `superteam` with base `waluigi` plays like Waluigi but uses
`superteam` files wherever the mod supplies them, and Waluigi's wherever it doesn't.

Mod characters appear on extra pages of captain select (**-**/**+**), and play per team: a mod
character and its base can face each other. A character can bring its own fixed team (SMS's Super Team
always plays with three robots) and turn its base's special moves off. [characters.md](characters.md)
lists every file a character can have and how each falls back to its base's.

### Catalogs

Some game files are tables shared by every character: the menu texture bundles, the string tables,
some INI files, the cutscene dictionary, the streamed sound banks. Mods add entries to them from a
`catalogs/` folder instead of replacing them, and the runtime merges every active mod's entries at
launch. See [characters.md](characters.md#catalogs).

## Native plugins

A mod can also include a native plugin (a `.dylib`, `.dll` or `.so`) for things data can't do: frame
events, hooks on game functions, settings of its own. Plugins are ordinary native code: they run with
the player's permissions, so the runtime only loads one after the player allows it on the Mods page.
[plugins.md](plugins.md) covers the C API (`sdk/include/msc_mod_api.h`) and the example in
`sdk/examples/hello`.

## Roadmap

1. Packages, manifests, the Mods page, adding and replacing files — **done**.
2. Native plugins: the C API, hooks on game functions (wrap points), an SDK with an example — **done**;
   the list of hookable functions grows with the framework.
3. Characters in matches: per-team models, kits, goalies, voices, logos, banners, HUD; mirror matches;
   catalogs — **done**.
4. Captain select pages for mod characters; portraits, stats, holograms, loading screens — **done**.
5. Cutscenes for mod characters, with their own audio and effects; animations; fixed teams; switching
   the base's special moves off — **done**. The first mod built on all of it is SMS's Super Team.
6. modkit: tools to make, check and package mods, including building content from your own games
   (the Super Team from your SMS disc) — next.
7. Specials of a character's own (super ability, Mega Strike) and partner characters.
