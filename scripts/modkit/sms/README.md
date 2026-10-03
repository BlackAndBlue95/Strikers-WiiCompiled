# Converters from Super Mario Strikers

The tools that converted Super Mario Strikers' Super Team (the "mystery" team in SMS's files) into
the assets [Super Team's pack](../../../packs/superteam/README.md) is built from. Super Mario Strikers
(GameCube) and Mario Strikers Charged (Wii) share an engine, so most formats are close cousins: these
convert between them, and read and dump both.

They're kept as reference tools, not a pipeline: each does one job, and its header says how to run
it. They're Python 3 with no dependencies; the ones that read SMS's files expect them extracted (with
`gcextract.py`) under the working folder, in `sms/`.

## The tools

**Discs and containers**

| Tool | What it does |
| --- | --- |
| `gcdisc.py`, `gcextract.py` | reads a GameCube disc image (`.ciso`); `gcextract.py <disc> <out> <prefix>...` extracts files |
| `nlchunk.py`, `nlchunks.py` | the NL chunk container both games' files use: walk it, read and write it as a tree |

**Models and animations**

| Tool | What it does |
| --- | --- |
| `sms2rlg.py` | an SMS character model (`.glg` + `.glt`) as a Charged one (`.rlg` + `.rlt`), rebound to a Charged skeleton |
| `smsglg.py`, `shier.py` | reads SMS models and both games' skeletons (`.shier`) |
| `sanim.py` | animations: lists both games', converts SMS's onto Charged's Waluigi skeleton, builds a `.sanim.zlib` from a Charged one with SMS's animations swapped in |

**Cutscenes**

| Tool | What it does |
| --- | --- |
| `nis.py` | cutscenes (`.nis`): lists both games', converts SMS's to Charged's, writes their `nis_dict.txt` entries |
| `megastrike.py` | Super Team's Mega Strike cutscenes: Charged's (Waluigi's) shots with SMS's Super Strike animation |
| `stick.py` | stick-figure contact sheets of animations, to check poses without the game |

**Effects**

| Tool | What it does |
| --- | --- |
| `smsfx2bun.py` | SMS's particle effects (`scripts.fx`, `templates.fx`, `effects.glt`) as a Charged effects bundle; `--extra` adds groups of a pack's own made of SMS's (`packs/superteam/fx/megastrike.fx`); `dump` and `verify` read Charged's |

**Audio**

| Tool | What it does |
| --- | --- |
| `musyx.py` | SMS's MusyX sound banks (`sebring.*`): reads them, extracts sounds |
| `superbank.py` | Super Team's voice as a Charged character sound bank (`.resbun` + `.nlxwb`) |
| `smsstream.py` | an SMS stream (`.idsp`) as a WAV |
| `supernisaudio.py`, `build_superteam_nis_bank.sh` | Super Team's cutscene audio: SMS's music and sounds mixed as SMS played them live, as streams for Charged's cutscene stream bank |
| `streambank.py`, `resbun.py` | Charged's stream banks and resident sound banks: dump, merge, verify |

**Front end and scripts**

| Tool | What it does |
| --- | --- |
| `superportrait.py` | captain select portraits, partner select heads, logos and lower third, from SMS's art in Charged's style |
| `bytecode.py`, `smsbc.py` | disassemble Charged's and SMS's script bytecode (`.byte_code`: cutscene triggers, presentation) |

## How Super Team's assets were made

| Asset (in the pack's `--assets` folder) | Made by |
| --- | --- |
| `files/Art/characters/superteam/superteam.rlg`, `.rlt`, `files/Art/characters/superteamgoalie/superteamgoalie.rlt` | `sms2rlg.py`, from SMS's `SuperTeam.glg`/`.glt` onto Charged's Waluigi |
| `files/Art/animation/superteam.sanim.zlib` | `sanim.py build`, Charged's Waluigi animations with SMS's swapped in |
| `files/Art/nis/superteam_*.nis`, `catalogs/nis/nis_dict.txt` | `nis.py superteam` (SMS's cutscenes), `megastrike.py` (the Mega Strike) |
| `files/Art/effects/superteam_sms.bun`, `superteam_smsnonres.bun.zlib` | `smsfx2bun.py build`, below |
| `files/audio/CHAR_SUPERTEAM_Sfx.resbun`, `.nlxwb` | `superbank.py` |
| `catalogs/streams/STREAM_GEN_NIS/` | `supernisaudio.py` (`build_superteam_nis_bank.sh` checks them merged) |
| `catalogs/fe/` | `superportrait.py` |

The effects, for example, from a working folder with SMS's files extracted in `sms/`:

```bash
python3 <repo>/scripts/modkit/sms/smsfx2bun.py build --sms sms/effects \
    --extra <repo>/packs/superteam/fx/megastrike.fx \
    --face-joints <game>/files/Art/characters/waluigi/waluigi.rlg \
    --out-bun superteam_sms.bun --out-nonres superteam_smsnonres.bun.zlib \
    'mystery_attitude_*' 'mystery_end_of_game_*' 'mystery_goal_low_sparks*' 'superteam_*'
```
