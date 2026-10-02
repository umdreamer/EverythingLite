#!/usr/bin/env bash
set -euo pipefail
[[ "$(uname -s)" == Linux ]] || { echo 'Debian 包必须在 Linux 环境生成。' >&2; exit 1; }
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
EL_RELEASE_DIR="${EL_RELEASE_DIR:-$ROOT_DIR/build/linux-release}"
EL_PACKAGE_DIR="${EL_PACKAGE_DIR:-$ROOT_DIR/dist}"
cmake -S "$ROOT_DIR" -B "$EL_RELEASE_DIR" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_GUI=ON -DREQUIRE_GUI=ON -DBUILD_TESTS=OFF
cmake --build "$EL_RELEASE_DIR" --parallel "${EL_BUILD_JOBS:-4}"
test -x "$EL_RELEASE_DIR/EverythingLite"
test -x "$EL_RELEASE_DIR/everything-lite-cli"
mkdir -p "$EL_PACKAGE_DIR"
cpack --config "$EL_RELEASE_DIR/CPackConfig.cmake" -B "$EL_PACKAGE_DIR"
