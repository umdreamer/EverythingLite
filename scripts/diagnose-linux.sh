#!/usr/bin/env bash
set -euo pipefail
[[ "$(uname -s)" == Linux ]] || { echo '此脚本需要 Linux。' >&2; exit 1; }
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
EL_BUILD_DIR="${EL_BUILD_DIR:-$ROOT_DIR/build/linux-debug}"
printf '架构：%s\n' "$(uname -m)"
for tool in cmake c++ pkg-config dbus-run-session xdg-open; do
    if command -v "$tool" >/dev/null 2>&1; then
        printf '%s：可用\n' "$tool"
    else
        printf '%s：未发现\n' "$tool"
    fi
done
if command -v pkg-config >/dev/null 2>&1; then
    for module in sqlite3 Qt6Widgets Qt6DBus Qt6Test; do
        pkg-config --modversion "$module" 2>/dev/null || printf '%s：未发现\n' "$module"
    done
fi
for executable in EverythingLite everything-lite-cli; do
    if [[ -x "$EL_BUILD_DIR/$executable" ]]; then
        printf '%s：构建产物可用\n' "$executable"
    else
        printf '%s：尚未构建\n' "$executable"
    fi
done
printf '会话总线：%s\n' "${DBUS_SESSION_BUS_ADDRESS:+已设置}"
printf '显示会话：%s\n' "${XDG_SESSION_TYPE:-未设置}"
printf 'inotify max_user_watches：'
cat /proc/sys/fs/inotify/max_user_watches
printf '诊断不启动应用、不访问索引、不调整系统监听配额。\n'
