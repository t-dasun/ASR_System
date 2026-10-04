import json
from pathlib import Path
import tempfile
import unittest

from tools.reports.m10 import TARGETS, verified_seal, write_outputs


class M10ReportTest(unittest.TestCase):
    def test_seal_rejects_modified_raw(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "status.json").write_text('{"status":"COMPLETE"}')
            (root / "raw.jsonl").write_text("first\n")
            import hashlib
            good = hashlib.sha256(b"first\n").hexdigest()
            (root / "checksums.json").write_text(json.dumps({"raw.jsonl": good}))
            self.assertEqual(verified_seal(root)["file_count"], 1)
            (root / "raw.jsonl").write_text("changed\n")
            with self.assertRaises(ValueError):
                verified_seal(root)

    def test_sizing_targets(self):
        self.assertEqual(TARGETS, (50, 100, 200, 500, 1000))

    def test_report_does_not_overwrite(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "report"
            with self.assertRaises(FileExistsError):
                output.mkdir()
                write_outputs({}, output)


if __name__ == "__main__":
    unittest.main()
