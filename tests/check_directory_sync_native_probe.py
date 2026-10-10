#!/usr/bin/env python3
"""Read-only native AX probe isolation. Never request consent or write controls."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

exe = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="el-native-probe-", dir="/private/tmp") as tmp:
    home = Path(tmp)
    env = dict(os.environ, HOME=tmp, CFFIXEDUSERHOME=tmp,
               XDG_DATA_HOME=str(home / "data"), XDG_CONFIG_HOME=str(home / "config"),
               EVERYTHING_LITE_DB=str(home / "sentinel.db"))
    env.pop("QT_QPA_PLATFORM", None)
    env.pop("EVERYTHING_LITE_SEARCH_TRACE", None)
    def run(args):
        return subprocess.run([str(exe), *args], env=env, cwd=tmp,
                              capture_output=True, timeout=12)
    def decode(result):
        assert result.returncode == 0, "native probe failed"
        assert len(result.stdout.splitlines()) == 1, "native probe must output one JSON line"
        assert len(result.stdout) <= 196608, "native output must be bounded"
        value = json.loads(result.stdout)
        assert value["protocol"] == 1 and type(value["protocol"]) is int
        assert type(value["accessibility"]) is bool
        assert value["finder_permission_only"] is True
        assert value["finder_status"] in {"granted", "consent-required", "denied", "unavailable", "error"}
        assert type(value["finder_error"]) is int
        assert value["native_status"] in {"success", "permission-denied", "unsupported", "error"}
        assert type(value["ax_error"]) is int
        assert type(value["focused_window_read"]) is bool
        assert value["kind"] in {"other", "open-panel", "save-panel"}
        assert value["directory_known"] is False
        assert type(value["truncated"]) is bool
        assert type(value["fixture_directory_match"]) is bool
        assert value["fixture_reason"] in {"none", "invalid-path", "invalid-pid", "target-rejected"}
        assert len(value["nodes"]) <= 128
        for node in value["nodes"]:
            assert set(node) == {"role", "identifier", "attributes", "url_type", "value_type", "fixture_url_match", "fixture_value_match"}
            assert len(node["attributes"]) <= 32
            assert type(node["fixture_url_match"]) is bool
            assert type(node["fixture_value_match"]) is bool
            for key in ("role", "identifier", "url_type", "value_type"):
                assert type(node[key]) is str
            assert all(type(attr) is str for attr in node["attributes"])
        assert b"synthetic untouched" not in result.stdout and tmp.encode() not in result.stdout
        return value
    for existing in (False, True):
        if existing:
            (home / "sentinel.db").write_bytes(b"synthetic untouched database sentinel")
        before = {str(p.relative_to(home)): p.read_bytes() for p in home.rglob("*") if p.is_file()}
        value = decode(run(["--directory-sync-native-probe"]))
        print("actual native status:", value["native_status"], "AX error:", value["ax_error"], "trust:", value["accessibility"])
        for args in (["--directory-sync-native-probe", "extra"],
                     ["extra", "--directory-sync-native-probe"],
                     ["--directory-sync-native-probe", "--request"],
                     ["--directory-sync-native-probe", "--fixture", "--request", "--directory", tmp],
                     ["--directory-sync-native-probe", "--debug-search"],
                     ["--directory-sync-native-probe", "--directory-sync-setup"],
                     ["--directory-sync-permission-probe", "--directory-sync-native-probe"],
                     ["--directory-sync-native-probe", "--directory", tmp, "--fixture", "1"],
                     ["--directory-sync-native-probe", "--fixture", "1", "--directory", tmp, "extra"]):
            result = run(args)
            assert result.returncode == 2 and not result.stdout, "mixed arguments must reject before GUI"
        empty = decode(run(["--directory-sync-native-probe", "--fixture", "", "--directory", ""]))
        assert empty["fixture_reason"] == "invalid-path" and not empty["nodes"], "empty fixture must never fall through to frontmost app"
        invalid = decode(run(["--directory-sync-native-probe", "--fixture", "1", "--directory", "/Users/synthetic"]))
        assert invalid["fixture_reason"] == "invalid-path" and not invalid["nodes"]
        invalid = decode(run(["--directory-sync-native-probe", "--fixture", "not-a-pid", "--directory", tmp]))
        assert invalid["fixture_reason"] == "invalid-pid" and not invalid["nodes"]
        rejected = decode(run(["--directory-sync-native-probe", "--fixture", "1", "--directory", tmp]))
        assert rejected["fixture_reason"] == "target-rejected" and not rejected["nodes"]
        after = {str(p.relative_to(home)): p.read_bytes() for p in home.rglob("*") if p.is_file()}
        assert before == after, "native probe must preserve database and settings"
print("native protocol, bounded output, strict arguments, and storage isolation passed")
