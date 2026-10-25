import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from check_commit_policy import validate_message  # noqa: E402


class CommitPolicyTest(unittest.TestCase):
    valid = (
        "docs: define one rule (refs #8)\n\n"
        "Tests: python3 -m unittest discover -s tests -p test_commit_policy.py\n"
        "Test-Result: PASS\n"
    )

    def test_one_issue_and_passing_test_is_valid(self):
        self.assertEqual(validate_message(self.valid), [])

    def test_many_commits_may_name_the_same_issue(self):
        self.assertEqual(validate_message(self.valid.replace("define one rule", "add a check")), [])

    def test_second_issue_is_rejected(self):
        self.assertTrue(validate_message(self.valid.replace("refs #8", "refs #8 #9")))

    def test_missing_issue_link_is_rejected(self):
        self.assertTrue(validate_message(self.valid.replace("refs #8", "change")))

    def test_missing_or_failed_tests_are_rejected(self):
        self.assertTrue(validate_message(self.valid.replace("Tests: python3 -m unittest discover -s tests -p test_commit_policy.py\n", "")))
        self.assertTrue(validate_message(self.valid.replace("Test-Result: PASS", "Test-Result: FAIL")))
        self.assertTrue(validate_message(self.valid.replace("Tests: python3 -m unittest discover -s tests -p test_commit_policy.py", "Tests: not run")))

    def test_command_checks_message_file(self):
        with tempfile.TemporaryDirectory() as directory:
            message = Path(directory) / "message.txt"
            message.write_text(self.valid, encoding="utf-8")
            command = [sys.executable, str(ROOT / "scripts" / "check_commit_policy.py"),
                       "--message-file", str(message)]
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 0)
            hook = ["sh", str(ROOT / ".githooks" / "commit-msg"), str(message)]
            self.assertEqual(subprocess.run(hook, capture_output=True, cwd=ROOT).returncode, 0)
            message.write_text(self.valid.replace("refs #8", "refs #8 #9"), encoding="utf-8")
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 1)
            self.assertEqual(subprocess.run(hook, capture_output=True, cwd=ROOT).returncode, 1)


if __name__ == "__main__":
    unittest.main()
