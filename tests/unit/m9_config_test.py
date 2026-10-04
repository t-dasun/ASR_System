from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.evaluation.compare_m9_configs import compare, one_factor


class M9ConfigTest(unittest.TestCase):
    def test_one_factor_and_paired_summary(self):
        left = ROOT / "configs/qwen_native_single.yaml"
        right = ROOT / "configs/qwen_native_threads2.yaml"
        self.assertEqual(one_factor(left, right), (4, 2))
        with tempfile.TemporaryDirectory() as directory:
            changed = Path(directory) / "changed.yaml"
            changed.write_text(right.read_text().replace("decode_step_ms: 2000", "decode_step_ms: 1000"))
            with self.assertRaises(ValueError):
                one_factor(left, changed)
        manifest = [{"id": "en1", "language": "en"}, {"id": "zh1", "language": "zh"}]

        def row(clip, language, edits, first, final):
            return {"id": clip, "language": language, "status": "COMPLETE",
                    "accuracy": {"metric": "cer" if language == "zh" else "wer",
                                 "reference_units": 10, "edits": edits},
                    "measurements": {"first_usable_transcript_ns": first,
                                     "final_result_ns": final}}

        a = [row("en1", "en", 1, 1_000_000, 2_000_000),
             row("zh1", "zh", 0, 2_000_000, 3_000_000)]
        b = [row("en1", "en", 2, 1_500_000, 2_500_000),
             row("zh1", "zh", 0, 2_500_000, 3_500_000)]
        result = compare(manifest, a, b, 4, 2)
        self.assertEqual(result["by_language"]["en"]["left_error_rate"], .1)
        self.assertEqual(result["by_language"]["en"]["right_p50_first_ms"], 1.5)
        with self.assertRaises(ValueError):
            compare(manifest, a, b[:-1] + [b[0]], 4, 2)


if __name__ == "__main__":
    unittest.main()
