#!/usr/bin/env bash
# Build Strikers-WiiCompiled from your own extracted copy of Mario Strikers Charged (macOS / Linux).
#
#   ./build.sh "/path/to/extracted/game"      # full build
#   ./build.sh --skip-translate               # recompile the runtime only
#
# The game folder is the one you extracted the disc into (contains sys/ and files/, or DATA/).
# Everything this produces (Assets/, generated/, build-*/) stays on your machine and is gitignored.
set -euo pipefail

REPO="$(cd "$(dirname "$0")" && pwd)"
PROJECT="projects/mscharged/recomp.yml"
TRANSLATOR_DLL="translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll"
EXE_NAME="Strikers-WiiCompiled"
cd "$REPO"

fail() { printf '\nerror: %s\n' "$*" >&2; exit 1; }
step() { printf '\n==> %s\n' "$*"; }
need() { command -v "$1" >/dev/null 2>&1 || fail "$1 not found. $2"; }

# Runs a command with output going to a log; shows the log's tail on failure.
logged() {
    local log="$BUILD_DIR/$1"; shift
    if ! "$@" >"$log" 2>&1; then
        tail -n 25 "$log" | sed 's/^/    | /'
        fail "step failed (full log: $log)"
    fi
}

DISC=""
SKIP_TRANSLATE=0
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
while [ $# -gt 0 ]; do
    case "$1" in
        --skip-translate) SKIP_TRANSLATE=1 ;;
        --jobs) JOBS="$2"; shift ;;
        -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
        *) DISC="$1" ;;
    esac
    shift
done

case "$(uname -s)" in
    Darwin) BUILD_DIR="$REPO/build-macos"; APP_DIR="$HOME/Library/Application Support/MSCRecomp"
            SHA256() { shasum -a 256 "$1" | cut -d' ' -f1; } ;;
    Linux)  BUILD_DIR="$REPO/build-linux"; APP_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/MSCRecomp"
            SHA256() { sha256sum "$1" | cut -d' ' -f1; } ;;
    *) fail "unsupported OS; on Windows run build.cmd" ;;
esac

need dotnet "Install the .NET 8 SDK."
need cmake "Install CMake 3.25 or newer."
need ninja "Install Ninja."
need clang "Install Clang."
need clang++ "Install Clang."
mkdir -p "$BUILD_DIR"

DISC_ROOT=""
if [ -n "$DISC" ]; then
    for candidate in "$DISC" "$DISC/DATA"; do
        if [ -f "$candidate/sys/main.dol" ] && [ -d "$candidate/files" ]; then
            DISC_ROOT="$(cd "$candidate" && pwd)"; break
        fi
    done
    [ -n "$DISC_ROOT" ] || fail "$DISC doesn't look like an extracted disc (expected sys/main.dol and files/ in it or in DATA/)"
fi

if [ "$SKIP_TRANSLATE" -eq 0 ]; then
    step "Checking main.dol"
    DOL="Assets/main.dol"
    if [ -n "$DISC_ROOT" ]; then SOURCE="$DISC_ROOT/sys/main.dol"
    elif [ -f "$DOL" ]; then SOURCE="$DOL"
    else fail "pass the folder you extracted the game into: ./build.sh \"/path/to/game\""; fi
    EXPECTED="$(sed -n 's/.*sha256: *\([0-9a-fA-F]\{64\}\).*/\1/p' "$PROJECT" | head -1)"
    ACTUAL="$(SHA256 "$SOURCE")"
    [ "$ACTUAL" = "$EXPECTED" ] || fail "$SOURCE has SHA-256 $ACTUAL, expected $EXPECTED.
Only a clean Mario Strikers Charged USA Rev 1 (R4QE01) main.dol is supported."
    if [ ! "$SOURCE" -ef "$DOL" ]; then
        mkdir -p Assets
        rm -f "$DOL"   # never write through a link to some other file
        cp "$SOURCE" "$DOL"
    fi
    echo "    main.dol OK (R4QE01 Rev 1)"

    step "Building the translator"
    logged translator-build.log dotnet build translator/src/Translator.Cli -c Release --nologo

    step "Translating main.dol (this takes a few minutes)"
    logged translate.log dotnet "$TRANSLATOR_DLL" translate-recursive 0x80006124 --project "$PROJECT" \
        --outdir generated/functions --output-metadata generated/base_translation_output.json \
        --production-source-bundle generated/base_translation_sources.bin \
        --no-function-files --prune-stale --threads "$JOBS"

    step "Generating data sections and the build graph"
    logged data-init.log dotnet "$TRANSLATOR_DLL" generate-data-init --project "$PROJECT"
    logged build-shards.log dotnet "$TRANSLATOR_DLL" emit-build-shards --project "$PROJECT" \
        --base-metadata generated/base_translation_output.json --base-functions-dir generated/functions \
        --native-source-dir runtime/src --out generated/build_shards
fi

step "Configuring the native build"
logged configure.log cmake -S runtime -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$(command -v clang)" -DCMAKE_CXX_COMPILER="$(command -v clang++)" \
    -DCMAKE_MAKE_PROGRAM="$(command -v ninja)"

step "Compiling (the first build takes a long time)"
logged build.log cmake --build "$BUILD_DIR" --target WiiCompiled --parallel "$JOBS"
[ -x "$BUILD_DIR/$EXE_NAME" ] || fail "build finished but $BUILD_DIR/$EXE_NAME is missing (see $BUILD_DIR/build.log)"

if [ -n "$DISC_ROOT" ]; then
    # Point Config.toml's [paths] dvd_root at the extracted disc, keeping any other settings.
    CONFIG="$APP_DIR/Config.toml"
    mkdir -p "$APP_DIR"
    touch "$CONFIG"
    DVD_LINE="dvd_root = \"$(printf '%s' "$DISC_ROOT" | sed 's/\\/\\\\/g; s/"/\\"/g')\"" awk '
        BEGIN { line = ENVIRON["DVD_LINE"] }
        /^[[:space:]]*dvd_root[[:space:]]*=/ { if (!done) print line; done = 1; next }
        { print }
        /^\[paths\][[:space:]]*$/ && !done { print line; done = 1 }
        END { if (!done) { if (NR) print ""; print "[paths]"; print line } }
    ' "$CONFIG" > "$CONFIG.tmp" && mv "$CONFIG.tmp" "$CONFIG"
    printf '\n    Config: %s (dvd_root -> %s)\n' "$CONFIG" "$DISC_ROOT"
fi

printf '\nDone. Run the game with:\n    %s\n' "$BUILD_DIR/$EXE_NAME"
