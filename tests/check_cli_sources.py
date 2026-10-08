"""Only synthetic roots and independent temporary databases; never daily index."""
import os
import pathlib
import sqlite3
import subprocess
import sys
import tempfile

cli = sys.argv[1]
def run(db, *args):
    env = os.environ.copy()
    env["EVERYTHING_LITE_DB"] = str(db)
    return subprocess.run([cli, "--db", str(db), *args], text=True, capture_output=True, env=env)

with tempfile.TemporaryDirectory(prefix="everything-cli-sources-") as temp:
    base = pathlib.Path(temp)
    db = base / "test.db"
    absent = run(db, "index", str(base / "missing"))
    assert absent.returncode != 0 and "完成" not in absent.stdout, absent
    assert absent.stderr.strip(), absent
    root = base / "root"
    root.mkdir()
    (root / "synthetic.txt").write_text("synthetic", encoding="utf-8")
    success = run(db, "index", str(root))
    assert success.returncode == 0 and "完成" in success.stdout and "写入" not in success.stdout, success
    listing = run(db, "sources", "list")
    assert listing.returncode == 0 and str(root) in listing.stdout and "id=" in listing.stdout, listing
    with sqlite3.connect(db) as connection:
        source_id = connection.execute("SELECT id FROM sources WHERE root=?", (str(root),)).fetchone()[0]
        before = connection.execute("SELECT * FROM files ORDER BY id").fetchall()
    verify = run(db, "sources", "verify", str(source_id))
    assert verify.returncode == 0 and "核验完成" in verify.stdout, verify
    with sqlite3.connect(db) as connection:
        row = connection.execute("SELECT identity,status,last_success FROM sources WHERE id=?", (source_id,)).fetchone()
        assert row[0].startswith("mac-volume:") and row[1] == 0, row
        assert connection.execute("SELECT * FROM files ORDER BY id").fetchall() == before
    for args in [("list", "extra"), ("verify",), ("verify", "0"), ("verify", "-1"), ("verify", "+1"),
                 ("verify", "1x"), ("verify", "1", "extra"), ("verify", "9223372036854775808"), ("other",)]:
        invalid_db = base / "invalid.db"
        invalid = run(invalid_db, "sources", *args)
        assert invalid.returncode != 0 and "核验完成" not in invalid.stdout, (args, invalid)
        assert not invalid_db.exists(), args
    missing = run(db, "sources", "verify", "999999")
    assert missing.returncode != 0 and "核验完成" not in missing.stdout, missing
    root.rename(base / "moved")
    offline = run(db, "sources", "verify", str(source_id))
    assert offline.returncode != 0 and "核验完成" not in offline.stdout, offline
    with sqlite3.connect(db) as connection:
        assert connection.execute("SELECT status FROM sources WHERE id=?", (source_id,)).fetchone()[0] == 1
        assert connection.execute("SELECT * FROM files ORDER BY id").fetchall() == before
    benchmark = run(db, "benchmark", str(base / "absent"))
    assert benchmark.returncode != 0 and "Indexed:" not in benchmark.stdout, benchmark
print("CLI sources tests passed")
