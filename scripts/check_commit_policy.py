#!/usr/bin/env python3
"""Check that each new commit names one Issue and records passing tests."""

import argparse
import re
import subprocess
from pathlib import Path


ISSUE = re.compile(r"(?<![A-Za-z0-9])#([1-9][0-9]*)\b")
LINK = re.compile(r"\b(?:refs?|fixes?|closes?)\s+#([1-9][0-9]*)\b", re.IGNORECASE)
TEST = re.compile(r"^Tests:\s*(.*?)\s*$", re.MULTILINE)
RESULT = re.compile(r"^Test-Result:\s*(.*?)\s*$", re.MULTILINE)


def validate_message(message: str) -> list[str]:
    """Return policy violations without trusting the stated test result."""
    errors = []
    issues = set(ISSUE.findall(message))
    links = set(LINK.findall(message))
    if len(issues) != 1 or links != issues:
        errors.append("commit message must link exactly one Issue with refs/fixes/closes #N")

    tests = [value.strip() for value in TEST.findall(message)]
    if not tests or any(value.lower() in {"", "none", "not run", "skipped"} for value in tests):
        errors.append("record at least one executed test command in a Tests: line")

    results = [value.strip() for value in RESULT.findall(message)]
    if results != ["PASS"]:
        errors.append("record exactly one Test-Result: PASS line")
    return errors


def messages_in_range(revision_range: str) -> list[tuple[str, str]]:
    commits = subprocess.run(
        ["git", "rev-list", "--reverse", revision_range],
        check=True, capture_output=True, text=True,
    ).stdout.splitlines()
    return [
        (sha, subprocess.run(
            ["git", "show", "-s", "--format=%B", sha],
            check=True, capture_output=True, text=True,
        ).stdout)
        for sha in commits
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--message-file", type=Path, help="commit-msg hook input")
    source.add_argument("--range", dest="revision_range", help="Git revision range, for example main..HEAD")
    args = parser.parse_args()
    try:
        messages = ([
            (str(args.message_file), args.message_file.read_text(encoding="utf-8"))
        ] if args.message_file else messages_in_range(args.revision_range))
    except (OSError, subprocess.CalledProcessError) as error:
        parser.exit(2, f"cannot read commit messages: {error}\n")

    if not messages:
        parser.exit(2, "revision range contains no commits\n")
    failed = False
    for identity, message in messages:
        for error in validate_message(message):
            print(f"{identity}: {error}")
            failed = True
    if not failed:
        print(f"PASS: {len(messages)} commit message(s) contain one Issue and test evidence")
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
