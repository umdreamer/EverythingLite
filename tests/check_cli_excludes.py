#!/usr/bin/env python3
"""CLI/database exclusion contract against an isolated synthetic index."""
import pathlib
import sqlite3
import subprocess
import sys
import tempfile

cli = sys.argv[1]
with tempfile.TemporaryDirectory(prefix="everything-lite-excludes-") as temporary:
    base = pathlib.Path(temporary)
    root = base / "root"
    (root / "sample" / "nested").mkdir(parents=True)
    (root / "sample-other").mkdir()
    for relative in ["sample/nested/hidden.txt", "sample-other/visible.txt", "exact.txt", "保留甲.txt", "砀例甲.txt", "a;b.tmp"]:
        (root / relative).write_text("synthetic", encoding="utf-8")
    db = base / "index.db"
    def run(*args, success=True):
        result = subprocess.run([cli, "--db", str(db), *args], text=True, capture_output=True)
        assert (result.returncode == 0) == success, (args, result.returncode, result.stdout, result.stderr)
        return result
    run("index", str(root))
    for command in ("search", "search-safe", "search-correct", "probe-search"):
        scoped = run(command, "txt", "--in-folder", str(root / "sample"), "--limit", "1").stdout
        assert "hidden.txt" in scoped and "visible.txt" not in scoped, (command, scoped)
    run("search", "txt", "--in-folder", "relative", success=False)
    run("search", "txt", "--in-folder", success=False)
    run("excludes", "show")
    with sqlite3.connect(db) as connection:
        connection.execute("INSERT OR REPLACE INTO app_metadata(key,value) VALUES('index_app_bundle_contents','1')")
    with sqlite3.connect(db) as connection:
        before = connection.execute("SELECT count(*) FROM files").fetchone()[0]
    run("excludes", "set", "--directory", str(root / "sample"), "--file", str(root / "exact.txt"), "--file-filter", '"a;b.tmp";?例甲.txt')
    with sqlite3.connect(db) as connection:
        assert connection.execute("SELECT value FROM app_metadata WHERE key='index_app_bundle_contents'").fetchone()[0] == "1"
    assert "hidden" not in run("search", "ext:txt").stdout
    assert "visible.txt" in run("search", "ext:txt").stdout
    assert "砀例甲.txt" not in run("search", "ext:txt").stdout
    rows = [line for line in run("search", "type:file").stdout.splitlines() if line.startswith("[F]")]
    assert rows and all(len(line.split("\t")) == 3 for line in rows)
    with sqlite3.connect(db) as connection:
        policy = connection.execute("SELECT revision FROM exclusion_policy").fetchone()[0]
        count = connection.execute("SELECT count(*) FROM files").fetchone()[0]
    run("excludes", "set", "--file-filter", "ext:tmp", success=False)
    run("excludes", "set", "--file-filter", '"unclosed', success=False)
    with sqlite3.connect(db) as connection:
        assert connection.execute("SELECT revision FROM exclusion_policy").fetchone()[0] == policy
        assert connection.execute("SELECT count(*) FROM files").fetchone()[0] == count
    run("index", str(root))
    assert "hidden" not in run("search", "type:file").stdout
    run("excludes", "clear")
    with sqlite3.connect(db) as connection:
        assert connection.execute("SELECT value FROM app_metadata WHERE key='index_app_bundle_contents'").fetchone()[0] == "1"
    with sqlite3.connect(db) as connection:
        assert connection.execute("SELECT count(*) FROM files").fetchone()[0] == count
    run("index", str(root))
    with sqlite3.connect(db) as connection:
        assert connection.execute("SELECT count(*) FROM files").fetchone()[0] == before
print("CLI exclusion contract passed")
