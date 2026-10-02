#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

if ! command -v brew >/dev/null 2>&1; then
  echo "Homebrew 未安装。请先安装 Homebrew。" >&2
  exit 1
fi

if ! command -v cmake >/dev/null 2>&1; then
  echo "CMake 未安装。请执行：brew install cmake" >&2
  exit 1
fi

# Everything Lite uses Qt Core/Gui/Widgets and Qt Test. Homebrew's `qt` formula is a
# meta-package containing many unrelated modules. Pointing CMake at qtbase
# avoids dragging QtPdf/QtSvg/QtVirtualKeyboard and their plugins into a simple
# Widgets application.
if ! brew --prefix qtbase >/dev/null 2>&1; then
  echo "Qt Base（含 Widgets/Test）未安装。请执行：brew install qtbase" >&2
  echo "如果已经执行过 brew install qt，qtbase 通常已经作为依赖安装。" >&2
  exit 1
fi

QTBASE_PREFIX="$(brew --prefix qtbase)"
BUILD_DIR="${BUILD_DIR:-build-macos}"
CLEAN_BUILD="${CLEAN_BUILD:-0}"
EL_BUILD_TYPE="${EL_BUILD_TYPE:-Release}"

cmake -S . -B "$BUILD_DIR" \
  -DBUILD_GUI=ON \
  -DREQUIRE_GUI=ON \
  -DBUILD_TESTS=ON \
  -DCMAKE_BUILD_TYPE="$EL_BUILD_TYPE" \
  -DCMAKE_PREFIX_PATH="$QTBASE_PREFIX"

if [[ "$CLEAN_BUILD" == "1" ]]; then
  cmake --build "$BUILD_DIR" --clean-first --parallel "${EL_BUILD_JOBS:-4}"
else
  cmake --build "$BUILD_DIR" --parallel "${EL_BUILD_JOBS:-4}"
fi
if [[ "$EL_BUILD_TYPE" == Debug ]]; then
  ctest --test-dir "$BUILD_DIR" --output-on-failure
else
  echo 'Release 仅验证编译；assert 回归需单独使用 Debug。'
fi

APP="$BUILD_DIR/EverythingLite.app"
if [[ ! -x "$APP/Contents/MacOS/EverythingLite" ]]; then
  echo "错误：未找到 $APP" >&2
  exit 1
fi

echo
echo "本地开发版构建完成：$ROOT_DIR/$APP"
echo "运行：open \"$APP\""
echo "调试搜索：./scripts/run-macos-debug.sh"
echo "直接运行：\"$APP/Contents/MacOS/EverythingLite\""
echo
echo "说明：此脚本默认不运行 macdeployqt。"
echo "原因：本地开发版直接使用 Homebrew qtbase 最稳定；独立分发包应单独执行部署/签名流程。"
