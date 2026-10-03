# Making a pack

A pack is its folder of files (and code, if it has any) plus an XML file that tells Riivolution what
its options are and what each one does. This page makes a pack that changes the game's files;
[Code mods](code-mods.md) adds code, and [Characters](characters.md) adds a captain.

## Layout

A pack is laid out like a Wii's SD card, so copying it onto one (or into a Riivolution folder)
installs it:

```
riivolution/mypack.xml        its options and what they do
mypack/files/...              files, laid out like the disc: files/Art/fe/... replaces the disc's Art/fe/...
mypack/sml_50_mypack.bin      code, if it has any (Code mods)
```

## The XML

```xml
<wiidisc version="1" root="/mypack">
  <id game="R4QE"/>
  <options>
    <section name="My pack">
      <option name="New menus" id="mypack_menus" default="1">
        <choice name="Enabled"><patch id="menus"/></choice>
      </option>
    </section>
  </options>
  <patch id="menus">
    <folder disc="/" external="files" create="true"/>
  </patch>
</wiidisc>
```

- **`root`** is the pack's folder on the card. `external` paths are relative to it; a leading `/`
  makes one relative to the card itself.
- **`<id game="R4QE"/>`** is the USA disc, the one these packs are for (`R4Q` would match every
  region).
- **`<option>`** is a switch the player sees, under its `<section>`. Each `<choice>` turns on the
  patches it names. `default` is the choice it starts on (1 is the first, 0 is off), and `id` is
  what Riivolution remembers the choice by.
- **`<patch>`** is what a choice does:
  - `<folder disc="/" external="files" create="true"/>`: every file under `files/` over the disc's;
    `create` adds the ones the disc doesn't have.
  - `<file disc="/audio/x.resbun" external="x.resbun"/>`: one file (`create="true"` adds a new one).
  - `<file disc="/audio/x.nlxwb" external="x.append" offset="455077440"/>`: written into the disc's
    file at that offset; at its end, it appends. Super Team adds its cutscene audio to the game's
    stream bank this way.
  - `<memory offset="0x80..." value="..."/>`: changes memory at an address. Dolphin and a Wii apply
    these; Strikers Recharged doesn't yet, so changes to the game's code or data belong in a code mod.

Riivolution's own documentation covers the rest of the format.

## Building it from the player's files

Nintendo's files can't go in a pack that's published. A pack that needs some (a texture it recolours,
a list it adds to) comes with a script that builds the pack from the player's own copy of the game,
the way Super Team's `build-pack.sh` takes the extracted game with `--game`.
[`scripts/modkit`](../../scripts/modkit/README.md) has helpers for the files packs most often add to:

| Helper | What it does |
| --- | --- |
| `merge_nis_dict.py` | adds entries to the cutscene dictionary, `Art/nis/nis_dict.txt` |
| `merge_stream_bank.py` | adds audio streams to a stream bank (`audio/<bank>.resbun` + `.nlxwb`): a new index, and the streams to append to the `.nlxwb` with an offset patch |
| `gxtex.py` | reads and writes the game's GX textures (`.gxt`), CMPR included |

## Trying it

In Strikers Recharged, copy the pack into the data folder's Riivolution folder and turn it on in
**F10 > Mods**; `console.log` shows each patch it applied, on lines starting with `[riivolution]`.
[Installing packs](installing.md) has Dolphin and the Wii.

## Sharing it

Zip the `riivolution` folder and the pack's folder together. Unzipped onto a Wii's SD card, into
Dolphin's `Load/Riivolution` folder or into Strikers Recharged's `Riivolution` folder, it's installed.
