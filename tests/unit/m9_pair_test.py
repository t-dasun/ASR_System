import json
import math
from pathlib import Path
import sys
import tempfile
import unittest
import wave
from array import array

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.datasets.prepare_m9_pairs import digest, prepare
from tools.evaluation.compare_m9_pairs import compare


class M9PairTest(unittest.TestCase):
    def test_prepare_balanced_pairs_and_compare(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            manifest = directory / "source.jsonl"
            rows = []
            for language in ("en", "id", "zh"):
                wav_path = directory / f"{language}.wav"
                samples = array("h", [round(5000 * math.sin(i * .03)) for i in range(3201)])
                with wave.open(str(wav_path), "wb") as wav:
                    wav.setnchannels(1)
                    wav.setsampwidth(2)
                    wav.setframerate(16000)
                    wav.writeframes(samples.tobytes())
                rows.append({"id": f"clip_{language}", "language": language, "file": str(wav_path),
                             "sha256": digest(wav_path), "reference": "hello", "reference_field":
                             "raw_transcription", "num_samples": len(samples), "selection_rank": 0})
            manifest.write_text("\n".join(json.dumps(row) for row in rows) + "\n")
            output = directory / "pairs"
            prepare(manifest, 1, output)
            combined = [json.loads(line) for line in (output / "combined.jsonl").read_text().splitlines()]
            self.assertEqual(len(combined), 6)
            self.assertEqual({row["pair_id"] for row in combined},
                             {"clip_en", "clip_id", "clip_zh"})
            self.assertTrue(all(digest(row["file"]) == row["sha256"] for row in combined))
            results = []
            for row in combined:
                is_tel = row["condition"] != "clean"
                results.append({"id": row["id"], "language": row["language"],
                                "status": "COMPLETE", "accuracy": {
                                    "metric": "cer" if row["language"] == "zh" else "wer",
                                    "reference_units": 2, "edits": 1 if is_tel else 0},
                                "measurements": {"first_usable_transcript_ns": 1_500_000 if is_tel else 500_000,
                                                 "final_result_ns": 2_000_000 if is_tel else 1_000_000}})
            report = compare(combined, results)
            self.assertEqual(report["by_language"]["en"]["absolute_error_rate_delta"], .5)
            self.assertEqual(report["by_language"]["zh"]["metric"], "cer")
            self.assertEqual(report["by_language"]["en"]["telephone_p50_first_ms"], 1.5)
            with self.assertRaises(ValueError):
                compare(combined, results[:-1])
            with self.assertRaises(ValueError):
                compare(combined, results[:-1] + [results[0]])
            wrong_metric = [{**row, "accuracy": {**row["accuracy"], "metric": "cer"}}
                            if row["language"] == "en" else row for row in results]
            with self.assertRaises(ValueError):
                compare(combined, wrong_metric)


if __name__ == "__main__":
    unittest.main()
