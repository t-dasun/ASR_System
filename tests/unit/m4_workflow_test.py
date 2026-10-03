import importlib.util
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/datasets"))
from prepare_m4_fleurs import select_cohorts

spec = importlib.util.spec_from_file_location("run_measured", ROOT / "tools/evaluation/run_measured.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class WorkflowTest(unittest.TestCase):
    def test_disjoint_selection(self):
        identities = {language: {str(i): i for i in range(100)} for language in ("en", "id", "zh")}
        selected = select_cohorts(identities, {"0", "1", "2"}, 42, 20, 50)
        self.assertEqual(len(selected), 70)
        self.assertFalse(set(selected) & {"0", "1", "2"})
        self.assertEqual(sum(v[0] == "tuning" for v in selected.values()), 20)
        self.assertEqual(selected, select_cohorts(identities, {"0", "1", "2"}, 42, 20, 50))

    def test_quantile_population(self):
        self.assertAlmostEqual(runner.quantiles([1, 2, 3, 4])["p95"], 3.85)
        self.assertIsNone(runner.quantiles([])["p99"])

    def test_materialized_cohorts(self):
        def read(name):
            return [json.loads(line) for line in (ROOT / "datasets/manifests" / name).read_text().splitlines()]
        tuning = read("fleurs_m4_tuning.jsonl")
        heldout = read("fleurs_m4_heldout_validation.jsonl")
        previous = read("fleurs_m0.jsonl")
        ids = lambda rows: {row["source_id"] for row in rows}
        self.assertFalse(ids(tuning) & ids(heldout))
        self.assertFalse((ids(tuning) | ids(heldout)) & ids(previous))
        for language in ("en", "id", "zh"):
            self.assertEqual(sum(row["language"] == language for row in tuning), 20)
            self.assertEqual(sum(row["language"] == language for row in heldout), 50)


if __name__ == "__main__":
    unittest.main()
