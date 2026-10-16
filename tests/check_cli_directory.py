"""Explicit compact-format migration uses only synthetic temporary data."""
import json
import os
import pathlib
import sqlite3
import subprocess
import sys
import tempfile
cli = sys.argv[1]
with tempfile.TemporaryDirectory(prefix="everything-directory-") as temp:
    base = pathlib.Path(temp).resolve()
    root = base / "synthetic"
    root.mkdir()
    for name in ("abcdef.txt", "abc---bcd---cde---def.txt", "砀例甲乙.txt", "中砀例甲.txt", "ALPHA-Beta.txt"):
        (root / name).write_text("fixture")
    db, backup = base / "index.sqlite", base / "recovery.sqlite"
    def run(*args, database=db):
        return subprocess.run([cli, "--db", str(database), *args], capture_output=True, text=True, timeout=30)
    indexed = run("index", str(root))
    assert indexed.returncode == 0, indexed
    with sqlite3.connect(db) as c:
        original = c.execute("SELECT * FROM files ORDER BY id").fetchall()
        sources = c.execute("SELECT * FROM sources ORDER BY id").fetchall()
    result = run("storage", "optimize", "--backup", str(backup))
    assert result.returncode == 0, ("explicit directory migration must succeed", result)
    data = json.loads(result.stdout)
    assert data["migration_committed"] and data["vacuum_completed"] and not data["already_compact"] and not data["error"], data
    with sqlite3.connect(db) as c:
        assert c.execute("SELECT type FROM sqlite_master WHERE name='files'").fetchone() == ("view",)
        assert c.execute("SELECT * FROM files ORDER BY id").fetchall() == original
        assert c.execute("SELECT * FROM sources ORDER BY id").fetchall() == [row[:-1] + (row[-1] + 1,) for row in sources]
        assert c.execute("SELECT sql FROM sqlite_master WHERE name='files_name_fts'").fetchone()[0].find("detail=none") >= 0
        assert c.execute("PRAGMA integrity_check").fetchone() == ("ok",)
        for statement in ("UPDATE sources SET label='old-writer'", "UPDATE app_metadata SET value=value", "DELETE FROM roots"):
            try:
                c.execute(statement)
            except sqlite3.DatabaseError:
                c.rollback()
            else:
                raise AssertionError("old writer must be rejected: " + statement)
    with sqlite3.connect(backup) as c:
        assert c.execute("SELECT * FROM files ORDER BY id").fetchall() == original
        assert c.execute("PRAGMA user_version").fetchone() == (413,)
    repeat_backup = base / "repeat.sqlite"
    repeat = run("storage", "optimize", "--backup", str(repeat_backup))
    assert repeat.returncode == 0, repeat
    repeated = json.loads(repeat.stdout)
    assert repeated["already_compact"] and not repeated["migration_committed"] and not repeat_backup.exists(), repeated
    for query in ("abcdef", "砀例甲乙", "中砀例甲", "alpha", "abc", "ab", "ext:txt abc", "matchpath: synthetic", 'path:synthetic alpha'):
        fast, safe = run("search", query), run("search-safe", query)
        assert fast.returncode == safe.returncode == 0 and fast.stdout == safe.stdout, (query, fast, safe)
    reindexed = run("index", str(root))
    assert reindexed.returncode == 0, reindexed
    assert run("check-search-index").returncode == 0
    old = base / "old.sqlite"
    with sqlite3.connect(old) as c:
        c.execute("CREATE TABLE files(id INTEGER PRIMARY KEY,path TEXT)")
    old_bytes = old.read_bytes()
    refused = run("storage", "optimize", "--backup", str(base / "old-backup.sqlite"), database=old)
    assert refused.returncode != 0 and old.read_bytes() == old_bytes and not (base / "old-backup.sqlite").exists(), refused
print("CLI directory migration tests passed")
