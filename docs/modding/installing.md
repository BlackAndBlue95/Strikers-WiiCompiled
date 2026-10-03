# Installing packs

A pack comes laid out like a Wii's SD card: a `riivolution` folder with the pack's XML file, and the
pack's own folder beside it.

```
riivolution/superteam.xml
superteam/...
```

A pack with code (a `sml_*.bin` file in its folder) also needs the **Strikers Mod Loader** pack
(`riivolution/strikersloader.xml`, `strikersloader/`) in Dolphin and on a Wii; Strikers Recharged
doesn't. Every pack here is for the USA disc, revision 1 (`R4QE01`).

- [In Strikers Recharged](#in-strikers-recharged)
- [In Dolphin](#in-dolphin)
- [On a Wii](#on-a-wii)

## In Strikers Recharged

1. Copy the pack's folders into the data folder's `Riivolution` folder, merging `riivolution` with
   the one already there:

   | Platform | Riivolution folder |
   | --- | --- |
   | Windows | `%LOCALAPPDATA%\MSCRecomp\Riivolution` |
   | Linux | `$XDG_DATA_HOME/MSCRecomp/Riivolution` (default `~/.local/share/MSCRecomp/Riivolution`) |
   | macOS | `~/Library/Application Support/MSCRecomp/Riivolution` |

2. If the pack has code, run `build.sh` again (Windows: `build.cmd`): the packs' code is built into
   the game along with the game's own.
3. Start the game. **F10 > Mods** lists the packs and each one's options.

A change in **F10 > Mods** applies the next time the game starts, and a pack with code needs a build
after it's added, updated, removed, or turned on or off; F10 and `console.log` say when the installed
packs no longer match the build. The choices are saved in `riivolution/config/R4QE.xml`, the file
Riivolution and Dolphin use.

`console.log` (in the data folder's `Logs` folder) says what each pack did, on lines starting with
`[riivolution]`, and which code mods were built in, on lines starting with `[mods]`. `[paths]
overlay_roots` in `Config.toml` adds folders laid out the same way (code is only taken from the
default one).

## In Dolphin

1. Copy the pack's folders, and the Strikers Mod Loader's for a pack with code, into Dolphin's
   `Load/Riivolution` folder: **File > Open User Folder**, then `Load` (create `Riivolution` in it).
2. Right-click the game, choose **Start with Riivolution Patches...**, turn the packs on (and
   **Strikers Mod Loader > Code mods** for a pack with code) and press **Start**.

A game modification descriptor does the same from the command line, `Dolphin -e strikers.json`:

```json
{
  "type": "dolphin-game-mod-descriptor",
  "version": 1,
  "base-file": "/path/to/Mario Strikers Charged.iso",
  "display-name": "Mario Strikers Charged + Super Team",
  "riivolution": {
    "patches": [
      {
        "xml": "/path/to/Dolphin/Load/Riivolution/riivolution/strikersloader.xml",
        "root": "/path/to/Dolphin/Load/Riivolution",
        "options": [{ "section-name": "Strikers Mod Loader", "option-name": "Code mods", "choice": 1 }]
      },
      {
        "xml": "/path/to/Dolphin/Load/Riivolution/riivolution/superteam.xml",
        "root": "/path/to/Dolphin/Load/Riivolution",
        "options": [{ "section-name": "Super Team", "option-name": "Super Team", "choice": 1 }]
      }
    ]
  }
}
```

`base-file` can also be an extracted game's `sys/main.dol`. Dolphin's log (**View > Show Log**, with
**OSReport** on) shows the loader at work: `[Strikers Mod Loader] 2 code mod(s)`, then each module it
loads.

## On a Wii

You need [Riivolution](https://wiibrew.org/wiki/Riivolution) on the Wii, from the Homebrew Channel,
and an SD card.

1. Copy the pack's folders, and the Strikers Mod Loader's for a pack with code, to the root of the
   SD card. If the card already has a `riivolution` folder, merge them: each pack's XML goes into it.
2. Put the game's disc in, start Riivolution and pick **Mario Strikers Charged**.
3. Set the packs to **Enabled** (and **Strikers Mod Loader > Code mods** for a pack with code), then
   **Launch**.

Riivolution saves the choices on the card, in `riivolution/config/R4QE.xml`.
