# modkit

Tools for building packs ([docs/modding](../../docs/modding/README.md)).

## Pack helpers

Packs' build scripts run these on the player's own files, so the packs never carry the game's:

| Script | What it does |
| --- | --- |
| `merge_nis_dict.py` | appends cutscene dictionary entries to the game's `Art/nis/nis_dict.txt` |
| `merge_stream_bank.py` | adds audio streams to a stream bank: a new index (`.resbun`) and the bytes to append to its `.nlxwb` with an offset patch |
| `gxtex.py` | the game's GX textures: decodes C8 and CMPR, encodes CMPR, writes PNGs to check them |

Each prints its usage when run without arguments (`gxtex.py` is a library).

## Converters from Super Mario Strikers

[`sms/`](sms/README.md) has the converters that made Super Team's assets from Super Mario Strikers:
models, animations, cutscenes, effects and audio. They're kept as reference tools, for anyone
converting more of SMS.
