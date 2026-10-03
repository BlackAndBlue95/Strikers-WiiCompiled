#!/usr/bin/env bash
# Builds the Super Team pack, laid out like an SD card:
#   <out>/riivolution/superteam.xml
#   <out>/superteam/sml_50_superteam.bin     the code (code/*.cpp, built with sdk/kamek)
#   <out>/superteam/files/...                the files, at their disc paths
# Its files are made from your own games, so the pack is only ever built locally: Super Team's
# model, animations and art converted from your Super Mario Strikers disc (--assets: the converted
# files, laid out as files/ and catalogs/), plus files of your Mario Strikers Charged disc it adds to
# (--game: the extracted disc, with files/), and its logo (art/super_omega.png) for its team art.
#
#   build-pack.sh --assets <dir> --game <dir> [out]   (out: default ./pack)
#
# Install: copy <out>'s contents into the data folder's Riivolution folder (macOS:
# ~/Library/Application Support/MSCRecomp/Riivolution) and run build.sh again.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
assets="" game="" out="$here/pack"
while [ $# -gt 0 ]; do
    case "$1" in
    --assets) assets=$2; shift ;;
    --game) game=$2; shift ;;
    *) out=$1 ;;
    esac
    shift
done
[ -n "$assets" ] && [ -n "$game" ] || { sed -n '2,13p' "$0" >&2; exit 2; }

need() { [ -f "$1" ] || { echo "build-pack: $1 is missing" >&2; exit 1; }; }
pack="$out/superteam"
files="$pack/files"
rm -rf "$pack" && mkdir -p "$out/riivolution" "$files/Art/characters/superteam" "$files/Art/characters/superteamgoalie" \
    "$files/Art/animation" "$files/Art/nis" "$files/Art/effects" "$files/audio" "$files/mods/superteam"
modkit="$here/../../scripts/modkit"

# The character: SMS's model and textures, its animations retargeted to Waluigi's skeleton, its
# goalie kit (in place of the leftover one on Charged's disc) and its voice.
for f in Art/characters/superteam/superteam.rlg Art/characters/superteam/superteam.rlt Art/animation/superteam.sanim.zlib \
    Art/characters/superteamgoalie/superteamgoalie.rlt audio/CHAR_SUPERTEAM_Sfx.resbun audio/CHAR_SUPERTEAM_Sfx.nlxwb; do
    need "$assets/files/$f"
    cp "$assets/files/$f" "$files/$f"
done
# Its team art, from its logo (art/super_omega.png): Waluigi's ExtraTextures (emblem, banners, Mega
# Strike backdrops, gloves) with the logo for his emblem and on his banners, his purple made its blue,
# next to its textures where the game looks for them; and the logo as its Striker Times photo and its
# captain picture in the HUD.
extra=$(find "$game/files/Art/characters/waluigi" -maxdepth 1 -iname extratextures.rlt | head -n 1)
need "${extra:-$game/files/Art/characters/waluigi/ExtraTextures.rlt}"
python3 "$here/tools/team_art.py" --logo "$here/art/super_omega.png" --extra "$extra" \
    --out-extra "$files/Art/characters/superteam/ExtraTextures.rlt" \
    --out-news "$files/mods/superteam/news_superteam.gxt" \
    --out-icon "$files/mods/superteam/captain_icons_superteam.gxt" \
    --out-board "$files/mods/superteam/logos_TEAM_superteam_bg.gxt" >/dev/null
# Its crowd: the game reads ini/CrowdCharacterLists/<name>.ini for the stands, <name>Alt.ini when the
# home team wears its away kit (a list that isn't there crashes the match load); Waluigi's crowd, both.
mkdir -p "$files/ini/CrowdCharacterLists"
crowd=$(find "$game/files/ini/CrowdCharacterLists" -maxdepth 1 -iname waluigi.ini | head -n 1)
need "${crowd:-$game/files/ini/CrowdCharacterLists/Waluigi.ini}"
cp "$crowd" "$files/ini/CrowdCharacterLists/superteam.ini"
cp "$crowd" "$files/ini/CrowdCharacterLists/superteamAlt.ini"
# Its hologram before a match: framed as Waluigi's (ini/ImpostorCharacterTweaks.ini, by name).
tweaks=$(find "$game/files/ini" -maxdepth 1 -iname impostorcharactertweaks.ini | head -n 1)
need "${tweaks:-$game/files/ini/ImpostorCharacterTweaks.ini}"
{ cat "$tweaks"; printf '\n[superteam]\n'; awk '/^\[waluigi\]/{on=1; next} /^\[/{on=0} on' "$tweaks"; } \
    >"$files/ini/ImpostorCharacterTweaks.ini"
# Its cutscenes (SMS's, converted), the effects they use, and their entries in the game's cutscene
# dictionary.
for f in "$assets"/files/Art/nis/*.nis Art/effects/superteam_sms.bun Art/effects/superteam_smsnonres.bun.zlib; do
    case "$f" in /*) src="$f" ;; *) src="$assets/files/$f" ;; esac
    need "$src"
    case "$src" in *.nis) cp "$src" "$files/Art/nis/" ;; *) cp "$src" "$files/Art/effects/" ;; esac
done
need "$game/files/Art/nis/nis_dict.txt"
python3 "$modkit/merge_nis_dict.py" "$game/files/Art/nis/nis_dict.txt" "$assets/catalogs/nis/nis_dict.txt" \
    "$files/Art/nis/nis_dict.txt" >/dev/null
# Their audio: streams added to the game's cutscene stream bank. Its index is rebuilt; the streams go
# after the game's own in its .nlxwb, which the pack patches at its end rather than copying it.
streams="$assets/catalogs/streams/STREAM_GEN_NIS"
need "$game/files/audio/STREAM_GEN_NIS.resbun"
need "$game/files/audio/STREAM_GEN_NIS.nlxwb"
nlxwb_size=$(wc -c <"$game/files/audio/STREAM_GEN_NIS.nlxwb" | tr -d ' ')
template=$(sed -n 's/^template *= *//p' "$streams/cues.txt")
python3 "$modkit/merge_stream_bank.py" --resbun "$game/files/audio/STREAM_GEN_NIS.resbun" --nlxwb-size "$nlxwb_size" \
    --template "$template" --out-resbun "$files/audio/STREAM_GEN_NIS.resbun" \
    --out-append "$pack/STREAM_GEN_NIS.nlxwb.append" "$streams"/*.idsp >/dev/null
sed "s/@NLXWB_SIZE@/$nlxwb_size/" "$here/riivolution/superteam.xml" >"$out/riivolution/superteam.xml"

# Menu art: captain select portraits, the stats panel, logos, lower third, partner select heads.
fe="$assets/catalogs/fe"
for f in mainui.dmn/screens/images/captain_superteam_s mainui.dmn/screens/images/captain_superteam_ds \
    mainui.dmn/screens/images/attributes_superteam mainui.dmn/screens/images/logos_TEAM_superteam \
    mainui.dmn/screens/images/superteam_left mainui.dmn/screens/images/superteam_right \
    ingameui.dmn/screens/images/lowerthird_superteam; do
    need "$fe/$f.gxt"
    cp "$fe/$f.gxt" "$files/mods/superteam/$(basename "$f").gxt"
done

"$here/../../sdk/kamek/build-module.sh" "$pack/sml_50_superteam.bin" "$here"/code/*.cpp
rm -f "$pack/sml_50_superteam.map"
echo "Super Team pack: $out"
