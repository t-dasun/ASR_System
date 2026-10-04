import json
from pathlib import Path
import tempfile
import unittest

from tools.evaluation.latency_matrix import median, summarize_case, svg_chart


class LatencyMatrixTest(unittest.TestCase):
    def test_median(self):
        self.assertIsNone(median([]))
        self.assertEqual(median([3, 1, 2]), 2)
        self.assertEqual(median([4, 1, 2, 3]), 2.5)

    def test_failed_call_counts_for_accuracy_not_latency(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp) / "chunk100_decode1000"
            directory.mkdir()
            (directory / "status.json").write_text('{"status":"FAILED"}')
            rows = [
                {"language": "en", "status": "COMPLETE", "checks": {"valid": True},
                 "accuracy": {"edits": 1, "reference_units": 10},
                 "measurements": {"first_usable_transcript_ns": 2_000_000_000,
                                  "final_result_ns": 4_000_000_000},
                 "resources": {"sampled_peak_tree_rss_bytes": 123}},
                {"language": "en", "status": "FAILED", "checks": {"valid": False},
                 "accuracy": {"edits": 10, "reference_units": 10},
                 "measurements": {}, "resources": {}},
            ]
            (directory / "accuracy.jsonl").write_text(
                "\n".join(json.dumps(row) for row in rows) + "\n")
            result = summarize_case(directory, 100, 1000)
            self.assertEqual(result["completed"], 1)
            self.assertEqual(result["by_language"]["en"]["error_rate"], 11 / 20)
            self.assertEqual(result["by_language"]["en"]["first_text_p50_s"], 2)
            self.assertEqual(result["overall_final_p50_s"], 4)
            self.assertEqual(result["max_sampled_tree_rss_bytes"], 123)

    def test_svg_handles_missing_cases(self):
        rows = [{"id": f"chunk{chunk}_decode{decode}", "by_language": {
            language: {"first_text_p50_s": None, "final_p50_s": None,
                       "error_rate": None, "metric": "CER" if language == "zh" else "WER"}
            for language in ("en", "id", "zh")}}
            for chunk, decode in ((200, 2000), (100, 2000), (200, 1000), (100, 1000))]
        self.assertIn("<svg", svg_chart(rows))


if __name__ == "__main__":
    unittest.main()
