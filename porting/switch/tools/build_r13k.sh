#!/usr/bin/env bash
set -euo pipefail
WORKSPACE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PROJECT="$WORKSPACE/porting/switch"
export DEVKITPRO=/c/devkitPro
export PATH="$DEVKITPRO/msys2/usr/bin:$DEVKITPRO/devkitA64/bin:$PATH"
BUILD="$PROJECT/build-vulkan-perfdiag"
SUFFIX=language
cmake -S "$PROJECT" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DXBOXRECOMP_DIR="$WORKSPACE/external_versions/secondsight-vulkan-runtime" \
    -DSECOND_SIGHT_VULKAN=ON \
    -DSECOND_SIGHT_CPU_PROFILE=OFF \
    -DSECOND_SIGHT_CPU_BOOST=OFF \
    -DSECOND_SIGHT_FLOW_TRACE=OFF \
    -DSECOND_SIGHT_VK_UNIFORM_CACHE=ON \
    -DSECOND_SIGHT_PB_BULK=ON \
    -DSECOND_SIGHT_VK_RAW_CMP=ON \
    -DSECOND_SIGHT_VK_FEEDBACK_COPY=ON \
    -DSECOND_SIGHT_VK_RT_ALIASES=ON \
    -DSECOND_SIGHT_GAME_LTO=OFF \
    -DSECOND_SIGHT_HW_PROGRESS=OFF \
    -DNVK_SDK="$WORKSPACE/external_versions/nvk_sdk" \
    -Dglslang_DIR="$WORKSPACE/external_versions/glslang-switch/lib/cmake/glslang"
cmake --build "$BUILD" --target second_sight_recomp_nro --parallel 6
DEST="$PROJECT/switch_sd/switch/secondsight/second_sight_vulkan_r13k_${SUFFIX}.nro"
cp "$BUILD/second_sight_recomp.nro" "$DEST"
mkdir -p "$PROJECT/snapshots/r13k-language"
cp "$BUILD/second_sight_recomp.elf" "$PROJECT/snapshots/r13k-language/r13k_${SUFFIX}.elf"
sha256sum "$DEST"


