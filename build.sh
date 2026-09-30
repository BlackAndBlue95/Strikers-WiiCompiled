#!/usr/bin/env bash
# Build Strikers-WiiCompiled from your own copy of Mario Strikers Charged (macOS / Linux).
#
#   ./build.sh                           # asks for your game (disc image or extracted folder)
#   ./build.sh "/path/to/game.wbfs"      # disc image: ISO, RVZ, WBFS, WIA, CISO, GCZ, ...
#   ./build.sh "/path/to/extracted/game" # or a folder extracted with Dolphin (sys/ + files/)
#   ./build.sh --skip-translate          # recompile the runtime only
#
# Everything this produces (Assets/, generated/, build-*/) stays on your machine and is gitignored.
set -euo pipefail

REPO="$(cd "$(dirname "$0")" && pwd)"
PROJECT="projects/mscharged/recomp.yml"
TRANSLATOR_DLL="translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll"
EXE_NAME="Strikers-WiiCompiled"
GAME_DIR="$REPO/Assets/Game"   # where a disc image gets extracted
# nodtool (https://github.com/encounter/nod) reads Wii disc images; pinned by version and hash.
NOD_VERSION="v2.0.0-alpha.10"
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

GAME=""
SKIP_TRANSLATE=0
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
while [ $# -gt 0 ]; do
    case "$1" in
        --skip-translate) SKIP_TRANSLATE=1 ;;
        --jobs) JOBS="$2"; shift ;;
        -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
        *) GAME="$1" ;;
    esac
    shift
done

case "$(uname -s)-$(uname -m)" in
    Darwin-arm64)  NOD_ASSET=nodtool-macos-arm64;  NOD_SHA=e23ca466999b720c55e6d29c9683fce8cc74451ba64ead2e543d50129f24528a ;;
    Darwin-x86_64) NOD_ASSET=nodtool-macos-x86_64; NOD_SHA=f68f504dc2b72694b468ca78b6a24142c7aa5c8800f77564297f4143682e6575 ;;
    Linux-x86_64)  NOD_ASSET=nodtool-linux-x86_64; NOD_SHA=f853b83268b9542faf0d28815bc3a23c090ae28ebf397e46f99cbb08122c8307 ;;
    Linux-aarch64|Linux-arm64) NOD_ASSET=nodtool-linux-aarch64; NOD_SHA=b0d94617ed2393334845669cc1f74f1e4fce0d6acb24e8be28c479689a1b907b ;;
    *) NOD_ASSET=""; NOD_SHA="" ;;
esac
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

EXPECTED_DOL_SHA="$(sed -n 's/.*sha256: *\([0-9a-fA-F]\{64\}\).*/\1/p' "$PROJECT" | head -1)"
check_dol() {
    local actual; actual="$(SHA256 "$1")"
    [ "$actual" = "$EXPECTED_DOL_SHA" ] || fail "$1 has SHA-256 $actual, expected $EXPECTED_DOL_SHA.
Only a clean Mario Strikers Charged USA Rev 1 (R4QE01) disc is supported."
}

# Prints the folder that directly contains sys/main.dol and files/ (the folder itself or DATA/).
disc_root_of() {
    local candidate
    for candidate in "$1" "$1/DATA"; do
        if [ -f "$candidate/sys/main.dol" ] && [ -d "$candidate/files" ]; then
            (cd "$candidate" && pwd); return 0
        fi
    done
    return 1
}

ask_for_game() {
    local picked=""
    if [ "$(uname -s)" = Darwin ]; then
        picked="$(osascript -e 'POSIX path of (choose file with prompt "Choose your Mario Strikers Charged (USA) disc image (ISO, RVZ, WBFS, ...)")' 2>/dev/null || true)"
    fi
    if [ -z "$picked" ]; then
        [ -t 0 ] || fail "pass your game: ./build.sh \"/path/to/game.wbfs\""
        printf '\nDrag your Mario Strikers Charged (USA) disc image or extracted folder here, then press Enter:\n> '
        IFS= read -r picked
        # Undo the quoting/escaping terminals add to dragged-in paths.
        picked="$(printf '%s' "$picked" | sed -e "s/^[[:space:]]*['\"]//" -e "s/['\"][[:space:]]*\$//" -e 's/\\\(.\)/\1/g')"
    fi
    [ -n "$picked" ] || fail "no game selected"
    GAME="$picked"
}

fetch_nodtool() {
    [ -n "$NOD_ASSET" ] || fail "no prebuilt nodtool for $(uname -s) $(uname -m); extract the disc with Dolphin and pass the folder instead"
    NODTOOL="$BUILD_DIR/tools/nodtool"
    if [ -x "$NODTOOL" ] && [ "$(SHA256 "$NODTOOL")" = "$NOD_SHA" ]; then return; fi
    need curl "Install curl."
    mkdir -p "$BUILD_DIR/tools"
    echo "    downloading nodtool $NOD_VERSION"
    curl -fsSL -o "$NODTOOL.part" "https://github.com/encounter/nod/releases/download/$NOD_VERSION/$NOD_ASSET" \
        || fail "couldn't download nodtool"
    [ "$(SHA256 "$NODTOOL.part")" = "$NOD_SHA" ] || { rm -f "$NODTOOL.part"; fail "nodtool download failed its checksum"; }
    chmod +x "$NODTOOL.part"; mv "$NODTOOL.part" "$NODTOOL"
}

extract_image() {
    local image="$1" stage="$REPO/Assets/.extract-$$" dol root
    step "Extracting your disc image (a few minutes)"
    fetch_nodtool
    rm -rf "$stage"; mkdir -p "$REPO/Assets"
    trap "rm -rf '$stage'" EXIT
    logged extract.log "$NODTOOL" extract "$image" "$stage"
    dol="$(find "$stage" -type f -path '*/sys/main.dol' | head -1)"
    [ -n "$dol" ] || fail "that image doesn't contain sys/main.dol (is it a Wii disc?)"
    root="$(dirname "$(dirname "$dol")")"
    [ -d "$root/files" ] || fail "that image doesn't contain a files/ folder"
    check_dol "$dol"
    rm -rf "$GAME_DIR"; mv "$root" "$GAME_DIR"
    rm -rf "$stage"; trap - EXIT
    echo "    extracted to $GAME_DIR"
}

DISC_ROOT=""
if [ -z "$GAME" ] && [ "$SKIP_TRANSLATE" -eq 0 ] && [ ! -f Assets/main.dol ]; then
    ask_for_game
fi
if [ -n "$GAME" ]; then
    if [ -d "$GAME" ]; then
        DISC_ROOT="$(disc_root_of "$GAME")" \
            || fail "$GAME doesn't look like an extracted disc (expected sys/main.dol and files/ in it or in DATA/)"
    elif [ -f "$GAME" ]; then
        if DISC_ROOT="$(disc_root_of "$GAME_DIR")" && [ "$(SHA256 "$DISC_ROOT/sys/main.dol")" = "$EXPECTED_DOL_SHA" ]; then
            echo "    using the already-extracted game in $GAME_DIR"
        else
            extract_image "$GAME"
            DISC_ROOT="$GAME_DIR"
        fi
    else
        fail "$GAME not found"
    fi
fi

if [ "$SKIP_TRANSLATE" -eq 0 ]; then
    step "Checking main.dol"
    DOL="Assets/main.dol"
    if [ -n "$DISC_ROOT" ]; then SOURCE="$DISC_ROOT/sys/main.dol"; else SOURCE="$DOL"; fi
    check_dol "$SOURCE"
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
    # Point Config.toml's [paths] dvd_root at the game files, keeping any other settings.
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
