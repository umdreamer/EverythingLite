"""Storage commands use only synthetic SQLite files in /private/tmp."""
import json
import os
import pathlib
import sqlite3
import subprocess
import sys
import tempfile
cli = sys.argv[1]
with tempfile.TemporaryDirectory(prefix="everything-cli-storage-", dir="/private/tmp") as temp:
    base = pathlib.Path(temp)
    db = base / "synthetic.sqlite"
    env = os.environ.copy()
    env["EVERYTHING_LITE_DB"] = str(db)
    def run(*args):
        return subprocess.run([cli, "--db", str(db), *args], capture_output=True, text=True, env=env, timeout=15)
    missing = run("storage", "stats")
    assert missing.returncode != 0 and not db.exists(), missing
    invalid = [("storage",), ("storage", "stats", "extra"), ("storage", "compact"),
               ("storage", "compact", "--backup"), ("storage", "compact", "--backup", str(base / "nope"), "extra"),
               ("storage", "stats", "--backup", "x"), ("storage", "compact", "--backup", ""),
               ("storage", "compact", "--backup", "x", "--backup", "y"), ("--trace-search", "storage", "stats"),
               ("--db", str(db), "storage", "stats")]
    for args in invalid:
        result = run(*args)
        assert result.returncode != 0 and not db.exists(), (args, result)
    with sqlite3.connect(db) as c:
        c.executescript("CREATE TABLE arbitrary(id INTEGER PRIMARY KEY, value TEXT); WITH RECURSIVE n(x) AS(VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<2000) INSERT INTO arbitrary SELECT x,hex(zeroblob(1000)) FROM n; DELETE FROM arbitrary WHERE id>10;")
        rows = c.execute("SELECT * FROM arbitrary ORDER BY id").fetchall()
    duplicate = run("--db", str(db), "storage", "stats")
    assert duplicate.returncode != 0, duplicate
    original = db.read_bytes()
    report = run("storage", "stats")
    assert report.returncode == 0, report
    data = json.loads(report.stdout)
    assert len(report.stdout.splitlines()) == 1 and data["schema_version"] == 1 and data["action"] == "stats", data
    fields = {"main_bytes", "wal_bytes", "shm_bytes", "page_size", "page_count", "freelist_bytes", "backup_bytes", "backup_count", "total_bytes"}
    assert set(data["storage"]) == fields and data["storage"]["freelist_bytes"] > 0, data
    assert original == db.read_bytes()
    (base / "child").mkdir()
    (base / "parent-alias").symlink_to(base, target_is_directory=True)
    for suffix in ("-wal", "-shm", "-journal"):
        for directory in (base, base / "child" / "..", base / "parent-alias", pathlib.Path(os.path.relpath(base)) / "child" / ".."):
            reserved = directory / (db.name + suffix)
            assert not reserved.exists()
            refused = run("storage", "compact", "--backup", str(reserved))
            assert refused.returncode != 0, refused
            assert not reserved.exists() and db.read_bytes() == original
    same = run("storage", "compact", "--backup", str(base / "parent-alias" / db.name))
    assert same.returncode != 0 and db.read_bytes() == original, same
    for suffix in ("-jOuRnAl", "-WaL", "-sHm"):
        for reserved in (base / (db.name.upper() + suffix), base / "child" / ("unrelated" + suffix)):
            refused = run("storage", "compact", "--backup", str(reserved))
            assert refused.returncode != 0, refused
            assert not reserved.exists() and db.read_bytes() == original
        reverse_source = base / ("reverse.sqlite" + suffix)
        reverse_source.write_bytes(original)
        reverse_backup = base / "reverse.sqlite"
        reverse = subprocess.run([cli, "--db", str(reverse_source), "storage", "compact", "--backup", str(reverse_backup)], capture_output=True, text=True, env=env, timeout=15)
        assert reverse.returncode != 0, reverse
        assert not reverse_backup.exists() and reverse_source.read_bytes() == original
    backup = base / "synthetic.sqlite.pre-storage-123.backup.sqlite"
    compact = run("storage", "compact", "--backup", str(backup))
    assert compact.returncode == 0, compact
    result = json.loads(compact.stdout)
    assert set(result) == {"schema_version", "action", "before", "after", "backup_bytes"}, result
    assert set(result["before"]) == fields and set(result["after"]) == fields
    assert result["after"]["main_bytes"] < result["before"]["main_bytes"]
    for path in (db, backup):
        with sqlite3.connect(path) as c:
            assert c.execute("SELECT * FROM arbitrary ORDER BY id").fetchall() == rows
            assert c.execute("SELECT name FROM sqlite_master WHERE type='table'").fetchall() == [("arbitrary",)]
    refused = run("storage", "compact", "--backup", str(backup))
    assert refused.returncode != 0 and str(base) not in refused.stderr, refused
    # URI-shaped names are ordinary filesystem names, never SQLite URI options.
    uri_db = base / "file:synthetic?mode=rwc"
    uri = subprocess.run([cli, "--db", str(uri_db), "storage", "stats"], capture_output=True, text=True, env=env)
    assert uri.returncode != 0 and not uri_db.exists()
print("CLI storage tests passed")
