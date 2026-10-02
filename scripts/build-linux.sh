#!/usr/bin/env bash
set -euo pipefail
[[ "$(uname -s)" == Linux ]] || { echo '此脚本需要 Linux。' >&2; exit 1; }
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
EL_BUILD_DIR="${EL_BUILD_DIR:-$ROOT_DIR/build/linux-debug}"
EL_BUILD_TYPE="${EL_BUILD_TYPE:-Debug}"
cmake -S "$ROOT_DIR" -B "$EL_BUILD_DIR" -DCMAKE_BUILD_TYPE="$EL_BUILD_TYPE" \
    -DBUILD_GUI=ON -DREQUIRE_GUI=ON -DBUILD_TESTS=ON
cmake --build "$EL_BUILD_DIR" --parallel "${EL_BUILD_JOBS:-4}"
test -x "$EL_BUILD_DIR/EverythingLite"
test -x "$EL_BUILD_DIR/everything-lite-cli"
if [[ "$EL_BUILD_TYPE" == Debug ]]; then
    if command -v dbus-run-session >/dev/null 2>&1; then
        dbus-run-session -- ctest --test-dir "$EL_BUILD_DIR" --output-on-failure
    else
        ctest --test-dir "$EL_BUILD_DIR" --output-on-failure
    fi
else
    echo '非 Debug 构建仅验证编译；assert 回归需要单独运行 Debug 测试。'
fi
printf 'GUI: %s\nCLI: %s\n' "$EL_BUILD_DIR/EverythingLite" "$EL_BUILD_DIR/everything-lite-cli"
