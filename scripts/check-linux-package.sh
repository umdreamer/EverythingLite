#!/usr/bin/env bash
set -euo pipefail
[[ "$(uname -s)" == Linux ]] || { echo '安装包检查必须在 Linux 环境运行。' >&2; exit 1; }
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
EL_PACKAGE="${1:?需要提供 Debian 包路径}"
EL_CHECK_DIR="$(mktemp -d /tmp/everything-lite-package.XXXXXX)"
EL_WESTON_PID=''
cleanup() {
    if [[ -n "$EL_WESTON_PID" ]]; then
        kill "$EL_WESTON_PID" 2>/dev/null || true
        wait "$EL_WESTON_PID" 2>/dev/null || true
    fi
    rm -rf -- "$EL_CHECK_DIR"
}
trap cleanup EXIT
dpkg-deb --info "$EL_PACKAGE"
dpkg-deb --contents "$EL_PACKAGE"
EL_EXPECTED_VERSION="$(awk '/^project\(EverythingLite VERSION / { print $3 }' "$ROOT_DIR/CMakeLists.txt")"
[[ "$(dpkg-deb -f "$EL_PACKAGE" Package)" == everything-lite ]]
[[ "$(dpkg-deb -f "$EL_PACKAGE" Version)" == "$EL_EXPECTED_VERSION" ]]
[[ "$(dpkg-deb -f "$EL_PACKAGE" Architecture)" == "$(dpkg --print-architecture)" ]]
EL_DEPENDENCIES="$(dpkg-deb -f "$EL_PACKAGE" Depends)"
for dependency in libqt6core6 libqt6gui6 libqt6widgets6 libqt6dbus6 libsqlite3-0 qt6-qpa-plugins qt6-wayland xdg-utils; do
    [[ "$EL_DEPENDENCIES" == *"$dependency"* ]] || { echo "缺少运行时依赖：$dependency" >&2; exit 1; }
done
dpkg-deb --extract "$EL_PACKAGE" "$EL_CHECK_DIR/prefix"
EL_GUI="$EL_CHECK_DIR/prefix/usr/bin/EverythingLite"
EL_CLI="$EL_CHECK_DIR/prefix/usr/bin/everything-lite-cli"
test -x "$EL_GUI"
test -x "$EL_CLI"
test -f "$EL_CHECK_DIR/prefix/usr/share/applications/org.everythinglite.EverythingLite.desktop"
test -f "$EL_CHECK_DIR/prefix/usr/share/icons/hicolor/scalable/apps/everything-lite.svg"
cmp "$ROOT_DIR/LICENSE" "$EL_CHECK_DIR/prefix/usr/share/doc/everything-lite/LICENSE"
python3 "$ROOT_DIR/tests/check_cli_parity.py" "$EL_CLI"
export HOME="$EL_CHECK_DIR/home" XDG_CONFIG_HOME="$EL_CHECK_DIR/config"
export XDG_CONFIG_DIRS="$EL_CHECK_DIR/system-config"
export XDG_DATA_HOME="$EL_CHECK_DIR/data" XDG_CACHE_HOME="$EL_CHECK_DIR/cache"
export XDG_RUNTIME_DIR="$EL_CHECK_DIR/runtime"
export EVERYTHING_LITE_DB="$EL_CHECK_DIR/index.db"
unset EVERYTHING_LITE_SEARCH_TRACE
mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_CONFIG_DIRS" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
check_startup() {
    local platform="$1" status=0
    shift
    QT_QPA_PLATFORM="$platform" timeout 3 "$@" "$EL_GUI" > "$EL_CHECK_DIR/$platform.log" 2>&1 || status=$?
    if [[ "$status" != 124 ]]; then
        cat "$EL_CHECK_DIR/$platform.log" >&2
        echo "GUI $platform 启动检查失败，退出码 $status。" >&2
        exit 1
    fi
    echo "GUI $platform 保持运行三秒，随后由 timeout 结束。"
}
check_startup offscreen
if [[ "${EL_CHECK_NATIVE_QPA:-0}" == 1 ]]; then
    check_startup xcb xvfb-run -a
    weston --backend=headless --renderer=pixman --socket=el-check-wayland \
        --no-config --idle-time=0 > "$EL_CHECK_DIR/weston.log" 2>&1 &
    EL_WESTON_PID=$!
    for ((attempt=0; attempt<100; attempt++)); do
        [[ -S "$XDG_RUNTIME_DIR/el-check-wayland" ]] && break
        if ! kill -0 "$EL_WESTON_PID" 2>/dev/null; then
            cat "$EL_CHECK_DIR/weston.log" >&2
            exit 1
        fi
        sleep 0.1
    done
    [[ -S "$XDG_RUNTIME_DIR/el-check-wayland" ]] || { cat "$EL_CHECK_DIR/weston.log" >&2; exit 1; }
    export WAYLAND_DISPLAY=el-check-wayland
    check_startup wayland
fi
