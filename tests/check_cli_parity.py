#!/usr/bin/env python3
"""Check portable CLI results using an isolated, synthetic file tree.

Output excludes host paths, directory sizes, and modification times so the JSON
can be compared across builds. No existing settings or database are changed.
"""

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def check(binary):
    with tempfile.TemporaryDirectory(prefix="everything-lite-parity-") as directory:
        base = Path(directory)
        root = base / "sample"
        files = {
            "report.txt": b"report",
            "REPORT.pdf": b"pdf",
            "砀例甲/砀例甲.txt": "合成样例".encode("utf-8"),
            "示例工匠/示例工匠.pdf": b"sample",
            "space dir/space $() `quote`; name.txt": b"safe argv",
            "onlypath/neutral.bin": b"12345678901234567890",
        }
        for relative, content in files.items():
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        environment = dict(os.environ)
        environment.pop("EVERYTHING_LITE_SEARCH_TRACE", None)

        def run(*arguments):
            return subprocess.run(
                [str(binary), "--db", str(base / "index.db"), *arguments],
                check=True, capture_output=True, text=True, encoding="utf-8",
                env=environment, timeout=30,
            ).stdout

        run("index", str(root))
        expected = {
            "report type:file": ["REPORT.pdf", "report.txt"],
            "砀例甲 type:file": ["砀例甲/砀例甲.txt"],
            "示例工匠 type:file": ["示例工匠/示例工匠.pdf"],
            "ext:pdf": ["REPORT.pdf", "示例工匠/示例工匠.pdf"],
            "onlypath type:file": [],
            "path:onlypath type:file": ["onlypath/neutral.bin"],
            "matchpath: onlypath type:file": ["onlypath/neutral.bin"],
            "size:>15b type:file": ["onlypath/neutral.bin"],
            "type:dir 砀例甲": ["砀例甲"],
            "space type:file": ["space dir/space $() `quote`; name.txt"],
        }
        results = {}
        for query, paths in expected.items():
            rows = []
            for line in run("search", query, "--limit", "100").splitlines():
                if not line.startswith(("[F] ", "[D] ")):
                    continue
                name, size, absolute = line[4:].split("\t", 2)
                relative = Path(absolute).relative_to(root).as_posix()
                row = {"path": relative, "name": name, "directory": line.startswith("[D]")}
                if not row["directory"]:
                    row["size"] = size
                rows.append(row)
            rows.sort(key=lambda row: row["path"])
            actual = [row["path"] for row in rows]
            assert actual == sorted(paths), (query, actual, sorted(paths))
            results[query] = rows
        return results


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()
    result = json.dumps(check(arguments.binary.resolve()), ensure_ascii=False,
                        indent=2, sort_keys=True) + "\n"
    if arguments.output:
        arguments.output.write_text(result, encoding="utf-8")
    else:
        print(result, end="")
