#!/bin/bash
# Render the Super Team's NIS cues from SMS data and merge them into STREAM_GEN_NIS.
# Usage: build_superteam_nis_bank.sh <Charged audio dir> [out dir]
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
A="$1"; O="${2:-$HERE/../out/audio/superteam}"
python3 "$HERE/supernisaudio.py" --out "$O" > "$O.render.log"
SIZE=$(python3 -c "import os,sys; print(os.path.getsize(sys.argv[1]))" "$A/STREAM_GEN_NIS.nlxwb")
CUES="superteam_goal_winner_high_0 superteam_goal_winner_high_1 superteam_goal_winner_low_0 superteam_home_capt_intro_1 superteam_away_capt_intro_1"
args=(); refs=()
for c in $CUES; do
  for v in "$c" "${c}_nocrowd"; do args+=("$v=$O/$v.idsp"); refs+=(--ref "$v=$O/$v.wav"); done
done
python3 "$HERE/streambank.py" merge "$A/STREAM_GEN_NIS" "$O/bank" --nlxwb-size "$SIZE" --template waluigi_goal_winner_high_0 "${args[@]}"
python3 "$HERE/streambank.py" verify "$A/STREAM_GEN_NIS" "$O/bank/STREAM_GEN_NIS.resbun" \
  --append "$O/bank/STREAM_GEN_NIS.nlxwb.append" --nlxwb "$A/STREAM_GEN_NIS.nlxwb" "${refs[@]}"
