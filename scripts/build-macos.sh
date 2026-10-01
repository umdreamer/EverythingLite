#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

if ! command -v brew >/dev/null 2>&1; then
  echo "Homebrew 未安装。请先安装 Homebrew，再执行：brew install cmake qt" >&2
  exit 1
fi

if ! brew --prefix qt >/dev/null 2>&1; then
  echo "Qt 未安装。请执行：brew install qt" >&2
  exit 1
fi

QT_PREFIX="$(brew --prefix qt)"
BUILD_DIR="${BUILD_DIR:-build-macos}"

cmake -S . -B "$BUILD_DIR" \
  -DBUILD_GUI=ON \
  -DBUILD_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$QT_PREFIX"

cmake --build "$BUILD_DIR" -j"$(sysctl -n hw.logicalcpu)"
ctest --test-dir "$BUILD_DIR" --output-on-failure

APP="$BUILD_DIR/everything-lite.app"
if [[ -d "$APP" && -x "$QT_PREFIX/bin/macdeployqt" ]]; then
  "$QT_PREFIX/bin/macdeployqt" "$APP" -always-overwrite
fi

echo
echo "构建完成：$ROOT_DIR/$APP"
echo "运行：open \"$APP\""
