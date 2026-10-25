"""Check links in the development policy documents changed with the workflow."""

import re
import unittest
from pathlib import Path
from urllib.parse import unquote


ROOT = Path(__file__).resolve().parents[1]
DOCUMENTS = (ROOT / "CONTRIBUTING.md", ROOT / "docs" / "DEVELOPMENT.md")
LINK = re.compile(r"\[[^\]]+\]\(([^)]+)\)")


class WorkflowDocsTest(unittest.TestCase):
    def test_relative_document_links_exist(self):
        for document in DOCUMENTS:
            with self.subTest(document=document.name):
                text = document.read_text(encoding="utf-8")
                for target in LINK.findall(text):
                    if target.startswith(("https://", "http://", "#")):
                        continue
                    path = unquote(target.split("#", 1)[0])
                    self.assertTrue((document.parent / path).is_file(), f"{document}: {target}")

    def test_documented_hook_targets_exist(self):
        self.assertTrue((ROOT / ".githooks" / "commit-msg").is_file())
        self.assertTrue((ROOT / "scripts" / "check_commit_policy.py").is_file())


if __name__ == "__main__":
    unittest.main()
