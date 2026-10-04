import json
from pathlib import Path
import tempfile
import unittest

from tools.reports.capacity import analyze_screens


class CapacityScreenTest(unittest.TestCase):
    def make_suite(self, root, concurrency, *, chunk_ms=200, completed=True):
        directory = root / f"load_{concurrency}"
        directory.mkdir()
        fixture = root / "fixture.wav"
        fixture.write_bytes(b"fixture")
        spec = {"calls": 1, "concurrency": concurrency, "repetitions": 1, "warmups": 0,
                "mode": "direct", "languages": ["en"], "max_failure_rate": 0,
                "max_p95_send_lag_ms": 1000}
        (directory / "plan.json").write_text(json.dumps({"spec": spec}))
        (directory / "status.json").write_text('{"status":"COMPLETE"}')
        metrics = {"offered_calls": 1, "completed_calls": int(completed),
                   "measurement_wall_seconds": 2.5, "audio_seconds_per_wall_second": .48,
                   "first_usable_ms": {"p95": 1700}, "final_result_ms": {"p95": 2300},
                   "effective_rtf": {"p95": 1.9}, "call_max_send_lag_ms": {"p95": .5},
                   "sampled_peak_tree_rss_bytes": 1000}
        summary = {"status": "COMPLETE", "is_mock": False, "measurement_failures": 0,
                   "metrics": metrics, "slo": {"qualified": False},
                   "phases": [{"warmup": False, "calls": [{"status": "COMPLETE" if completed else "FAILED"}]}]}
        (directory / "summary.json").write_text(json.dumps(summary))
        call = directory / "call_0"
        call.mkdir()
        (call / "config.json").write_text(json.dumps({"model": {"runtime": "qwen_native"},
            "audio": {"path": str(fixture), "chunk_ms": chunk_ms,
                      "effective_samples": 19200, "realtime_pacing": True}}))
        return directory

    def test_comparable_curve_is_only_a_screen(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            first = self.make_suite(root, 1)
            second = self.make_suite(root, 2)
            result = analyze_screens([second, first])
            self.assertEqual(result["status"], "DIAGNOSTIC_SCREEN_ONLY")
            self.assertIsNone(result["safe_legs_per_node"])
            self.assertEqual([row["concurrency"] for row in result["rows"]], [1, 2])

    def test_changed_factor_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            first = self.make_suite(root, 1)
            second = self.make_suite(root, 2, chunk_ms=500)
            with self.assertRaises(ValueError):
                analyze_screens([first, second])

    def test_failed_call_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            first = self.make_suite(root, 1)
            second = self.make_suite(root, 2, completed=False)
            with self.assertRaises(ValueError):
                analyze_screens([first, second])


if __name__ == "__main__":
    unittest.main()
