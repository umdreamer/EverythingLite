#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
EL_EXPORT_DIR="${1:?需要提供空的源码导出目录}"
git -C "$ROOT_DIR" rev-parse --is-inside-work-tree >/dev/null
mkdir -p "$EL_EXPORT_DIR"
EL_EXPORT_DIR="$(cd "$EL_EXPORT_DIR" && pwd)"
case "$EL_EXPORT_DIR/" in
    "$ROOT_DIR/"*) echo '源码导出目录必须位于工程目录之外。' >&2; exit 1 ;;
esac
[[ -z "$(ls -A "$EL_EXPORT_DIR")" ]] || { echo '源码导出目录必须为空。' >&2; exit 1; }
# Pending source edits are included; local data and Git metadata are excluded.
while IFS= read -r -d '' relative; do
    case "$relative" in
        CMakeLists.txt|CMakePresets.json|LICENSE|Dockerfile.linux-dev|.dockerignore|\
        src/*.cpp|src/*.h|tests/*.cpp|tests/*.py|scripts/*.sh|\
        packaging/linux/*.desktop|assets/*.svg)
            component="$relative"
            while [[ "$component" != . ]]; do
                [[ ! -L "$ROOT_DIR/$component" ]] || { echo '构建源码不能包含符号链接。' >&2; exit 1; }
                component="$(dirname "$component")"
            done
            [[ -f "$ROOT_DIR/$relative" ]] || continue
            mkdir -p "$EL_EXPORT_DIR/$(dirname "$relative")"
            cp "$ROOT_DIR/$relative" "$EL_EXPORT_DIR/$relative"
            ;;
    esac
done < <(git -C "$ROOT_DIR" ls-files -z --cached --others --exclude-standard)
