# Super Team

Super Mario Strikers' robot team as a captain of its own in Mario Strikers Charged. It has a page of
captain select after the captains (**-** and **+** switch pages), so any captain can face it,
Waluigi (its base) too.

- **The robots:** SMS's model on Waluigi's skeleton, with SMS's animations. Its partners are always
  robots too, whatever partner select picked, as in SMS.
- **Its cutscenes:** SMS's walk-on, goal celebrations and end of match, converted, with SMS's
  effects (the self-shock, the rocket boots), music and sounds, and the robot's voice.
- **Its Mega Strike:** SMS's Super Strike, the float in the yellow lightning vortex and the kick, in
  Charged's Mega Strike shots, with yellow fireballs on the balls. It has no music yet.
- **Its team:** a blue team with its logo on the banners, emblems, HUD icon, Striker Times photos,
  captain select, loading screens and results.
- **How it plays:** balanced, no super ability, the generic deke (SMS's spin).

It runs in Strikers Recharged, in Dolphin and on a Wii (with the
[Strikers Mod Loader](../loader/README.md) there).

## Building it

The pack is built from your own discs: its model, animations, cutscenes, audio and effects come from
Super Mario Strikers, and some of its files are Charged's with Super Team added in.

```bash
sdk/kamek/fetch-toolchain.sh   # once
packs/superteam/build-pack.sh --assets <assets> --game <game> out/
```

- **`--game`**: your extracted Mario Strikers Charged, a folder with `files/` and `sys/` (Dolphin's
  **Extract Entire Disc** makes one).
- **`--assets`**: Super Team's assets converted from Super Mario Strikers (USA): `files/` (model,
  animations, cutscenes, voice, effects, at their disc paths) and `catalogs/` (menu art, cutscene
  dictionary entries, cutscene audio streams). They were made with the converters in
  [`scripts/modkit/sms`](../../scripts/modkit/sms/README.md), kept there as reference tools.

`out/` then holds `riivolution/superteam.xml` and `superteam/`. [Installing
packs](../../docs/modding/installing.md) has the rest.

## What's in it

| Path | What it is |
| --- | --- |
| `code/` | the code: the character framework ([Characters](../../docs/modding/characters.md)) and Super Team itself (`superteam.cpp`) |
| `fx/megastrike.fx` | its Mega Strike's effects, arranged from SMS's (`scripts/modkit/sms/smsfx2bun.py --extra`) |
| `art/super_omega.png` | its logo, the source of its team art |
| `tools/team_art.py` | its team art: Waluigi's banners, emblems and Mega Strike backdrops in its blue with its logo, its Striker Times photo and HUD icon |
| `riivolution/superteam.xml` | the pack's option and patches |
| `build-pack.sh` | builds the pack |
