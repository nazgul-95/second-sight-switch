#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROJECT="$ROOT/porting/switch"

if [[ -z "${DEVKITPRO:-}" ]]; then
    if [[ -d /opt/devkitpro ]]; then
        export DEVKITPRO=/opt/devkitpro
    else
        export DEVKITPRO=/c/devkitPro
    fi
fi

[[ -f "$DEVKITPRO/cmake/Switch.cmake" ]] || {
    echo "devkitPro Switch toolchain not found at $DEVKITPRO" >&2
    exit 1
}
[[ -f "$PROJECT/generated_boot/recomp_funcs.h" ]] || {
    echo "Lifted code missing; see $PROJECT/README.md" >&2
    exit 1
}
[[ -f "$PROJECT/game_files/e/ss_ntsc.xbe" ]] || {
    echo "English NTSC game files missing; see $PROJECT/README.md" >&2
    exit 1
}

BUILD="$PROJECT/build"
cmake -S "$PROJECT" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSECOND_SIGHT_GEN_DIR="$PROJECT/generated_boot" \
    -DSECOND_SIGHT_CPU_PROFILE="${SECOND_SIGHT_CPU_PROFILE:-OFF}" \
    -DSECOND_SIGHT_GFX_DIAGNOSTICS="${SECOND_SIGHT_GFX_DIAGNOSTICS:-OFF}"
ninja -C "$BUILD" -j"${JOBS:-8}"

DEST="$ROOT/porting/switch/switch_sd/switch/secondsight"
mkdir -p "$DEST/game/e"
cp "$BUILD/second_sight_recomp.nro" "$DEST/second_sight_recomp.nro"
if command -v rsync >/dev/null 2>&1; then
    rsync -a --size-only "$PROJECT/game_files/e/" "$DEST/game/e/"
else
    # The Windows devkitPro shell does not necessarily ship rsync.
    cp -a -u "$PROJECT/game_files/e/." "$DEST/game/e/"
fi
echo "Staged homebrew layout at $DEST"

