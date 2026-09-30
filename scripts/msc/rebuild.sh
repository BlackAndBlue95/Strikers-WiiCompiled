#!/bin/zsh
# Retranslate main.dol (picks up runtime native overrides), regenerate the build graph, and build.
# Needs Assets/main.dol extracted from your own copy of the game (see README).
set -e
cd "$(dirname $0)/../.."
source scripts/msc/env.sh
mkdir -p build-macos
translator translate-recursive 0x80006124 --project projects/mscharged/recomp.yml --outdir generated/functions \
  --output-metadata generated/base_translation_output.json --production-source-bundle generated/base_translation_sources.bin \
  --no-function-files --prune-stale --threads $(sysctl -n hw.ncpu) > build-macos/translate.log 2>&1 \
  || { tail -20 build-macos/translate.log; exit 1; }
translator generate-data-init --project projects/mscharged/recomp.yml > /dev/null
translator emit-build-shards --project projects/mscharged/recomp.yml --base-metadata generated/base_translation_output.json \
  --base-functions-dir generated/functions --native-source-dir runtime/src --out generated/build_shards > /dev/null
cmake -S runtime -B build-macos > /dev/null
cmake --build build-macos --target WiiCompiled --parallel $(sysctl -n hw.ncpu) > build-macos/build.log 2>&1 \
  || { grep -E 'error|FAILED' build-macos/build.log | head -20; exit 1; }
echo "build ok"
