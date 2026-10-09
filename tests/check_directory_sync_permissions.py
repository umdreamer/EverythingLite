#!/usr/bin/env python3
"""Read-only native helper checks; never request real system consent."""
import json
import os
from pathlib import Path
import plistlib
import re
import subprocess
import sys
import tempfile

exe = Path(sys.argv[1]).resolve()
source = Path(sys.argv[2]).resolve()
with tempfile.TemporaryDirectory(prefix="el-permissions-") as tmp:
    home = Path(tmp)
    env = dict(os.environ, HOME=tmp, CFFIXEDUSERHOME=tmp,
               XDG_DATA_HOME=str(home / "data"), XDG_CONFIG_HOME=str(home / "config"),
               EVERYTHING_LITE_DB=str(home / "sentinel.db"), QT_QPA_PLATFORM="offscreen")
    def run(args):
        return subprocess.run([str(exe), *args], env=env, cwd=tmp,
                              capture_output=True, timeout=15)
    for existing in (False, True):
        sentinel = home / "sentinel.db"
        if existing:
            sentinel.write_bytes(b"synthetic untouched database sentinel")
        before = {str(p.relative_to(home)): p.read_bytes() for p in home.rglob("*") if p.is_file()}
        result = run(["--directory-sync-permission-probe"])
        assert result.returncode == 0, "read-only helper failed"
        assert len(result.stdout.splitlines()) == 1, "helper must output exactly one JSON line"
        value = json.loads(result.stdout)
        assert set(value) == {"protocol", "accessibility", "finder_status", "finder_error", "request"}
        assert type(value["protocol"]) is int and value["protocol"] == 1
        assert type(value["accessibility"]) is bool
        assert type(value["request"]) is bool and value["request"] is False
        assert value["finder_status"] in {"granted", "consent-required", "denied", "unavailable", "error"}
        assert type(value["finder_error"]) is int and -(2**31) <= value["finder_error"] < 2**31
        for args in (["--directory-sync-permission-probe", "extra"],
                     ["--directory-sync-permission-probe", "--request", "extra"],
                     ["extra", "--directory-sync-permission-probe"],
                     ["--db", "--directory-sync-permission-probe"],
                     ["--directory-sync-setup", "extra"],
                     ["--directory-sync-setup", "--request"],
                     ["--request", "--directory-sync-permission-probe"],
                     ["extra", "--directory-sync-setup"],
                     ["--directory-sync-setup", "--directory-sync-permission-probe"]):
            result = run(args)
            assert result.returncode == 2 and not result.stdout, "mixed arguments must reject before GUI"
        after = {str(p.relative_to(home)): p.read_bytes() for p in home.rglob("*") if p.is_file()}
        assert before == after, "helper must not create/change database or settings"
main = (source / "src/ui/main.cpp").read_text()
assert main.index("runDirectorySyncPermissionHelper") < main.index("QApplication app")
assert main.index("runDirectorySyncPermissionHelper") < main.index("--debug-search")
assert main.index("DirectorySyncSetupDialog window") < main.index("MainWindow window")
plist_path = exe.parent.parent / "Info.plist"
with plist_path.open("rb") as f:
    plist = plistlib.load(f)
assert plist["CFBundleName"] == "Everything Lite"
assert plist["CFBundleExecutable"] == "EverythingLite"
assert plist["CFBundleIdentifier"] == "org.cichi.everything-lite"
project_version = re.search(r"project\(EverythingLite\s+VERSION\s+([0-9.]+)",
                            (source / "CMakeLists.txt").read_text()).group(1)
assert plist["CFBundleShortVersionString"] == project_version
assert plist["CFBundleVersion"] == project_version
assert plist["NSAppleEventsUsageDescription"] == "用于在文件打开和保存窗口与 Finder 之间同步当前目录。"
print("read-only native helper, argument isolation, database preservation, and bundle metadata passed")
