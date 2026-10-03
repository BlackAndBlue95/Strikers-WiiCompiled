# Modding

Mods for Mario Strikers Charged are [Riivolution](https://wiibrew.org/wiki/Riivolution) packs, the
format Wii mods have long used. **The same pack runs in Strikers Recharged, in Dolphin and on a Wii.**
A pack can:

- **replace or add game files**: models, textures, menus, sounds, cutscenes, INI files;
- **add code**: Kamek modules, PowerPC code compiled against the game's own classes, which hooks
  into the game.

Nothing is patched for good: Riivolution lays a pack over the game when it starts, and turning the
pack off brings the original back.

## Guides

| To | Read |
| --- | --- |
| install a pack and play with it, here, in Dolphin or on a Wii | [Installing packs](installing.md) |
| make a pack that changes the game's files | [Making a pack](making-packs.md) |
| write code that hooks into the game | [Code mods](code-mods.md) |
| add a captain of your own to captain select | [Characters](characters.md) |

Players only need the first one, and the [Mods](https://github.com/BlackAndBlue95/Strikers-WiiCompiled/wiki/Mods)
page of the wiki has the short version.

## The packs in this repository

| Pack | What it does | Runs in |
| --- | --- | --- |
| [Super Team](../../packs/superteam/README.md) | Super Mario Strikers' robot team as a captain of its own, on a second page of captain select | Strikers Recharged, Dolphin, Wii |
| [GameCube controllers](../../packs/gamecube/README.md) | GameCube controllers as players: menu navigation, Super Mario Strikers' controls | Dolphin, Wii (built into Strikers Recharged) |
| [Strikers Mod Loader](../../packs/loader/README.md) | loads the other packs' code | Dolphin, Wii |

[packs/](../../packs/README.md) says how to build each one.

## Where things are

| Folder | What's in it |
| --- | --- |
| [`packs/`](../../packs/README.md) | the packs: each one's sources, Riivolution XML and `build-pack.sh` |
| [`sdk/kamek/`](../../sdk/kamek/README.md) | the code-mod SDK: toolchain fetch, `kamek.h`, the game's symbols, the module build script, an example |
| [`scripts/modkit/`](../../scripts/modkit/README.md) | helpers packs build with, and the converters that made Super Team from Super Mario Strikers |
| `runtime/src/mods/`, `translator/src/Translator.Core/Mods/` | how Strikers Recharged builds packs' code into the game |

> [!IMPORTANT]
> A pack in this repository never contains Nintendo's files. One that needs content from a game (a
> model from Super Mario Strikers, or a Charged file it changes) comes with a script that builds the
> pack from the player's own copy.
