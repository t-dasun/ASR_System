import random
import importlib.util
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.testing.scoring import AccuracyEvaluator, IdentityNormalizer, aggregate


class ScoringTest(unittest.TestCase):
    def test_unicode_policy(self):
        scorer = AccuracyEvaluator()
        for ref, hyp, lang in (("Cafe\u0301 STRASSE", "CAFÉ Straße", "en"),
                               ("satu-dua, tiga", "satu dua tiga", "id"),
                               ("你 好，𠀀界。", "你好𠀀界", "zh")):
            self.assertEqual(scorer.score(ref, hyp, lang)["edits"], 0)
        self.assertEqual(scorer.score("𠀀", "", "zh")["reference_units"], 1)
        self.assertEqual(scorer.score("don't", "dont", "en")["normalized_reference"], "don t")
        self.assertEqual(AccuracyEvaluator(IdentityNormalizer()).score("Hi!", "hi", "en")["rate"], 1)

    def test_empty_and_failures(self):
        scorer = AccuracyEvaluator()
        empty = scorer.score("", "false output", "en")
        self.assertIsNone(empty["rate"])
        self.assertEqual(empty["insertions"], 2)
        rows = [{"language": "en", "status": "FAILED", "accuracy": scorer.score("one two", "", "en")},
                {"language": "en", "status": "COMPLETE", "accuracy": scorer.score("three", "three", "en")}]
        result = aggregate(rows)["en"]
        self.assertEqual(result["completed_only"]["rate"], 0)
        self.assertEqual(result["offered_failure_inclusive"]["rate"], 2 / 3)
        self.assertEqual(result["failures"], 1)

    @unittest.skipUnless(importlib.util.find_spec("jiwer"), "Optional JiWER parity check requires jiwer")
    def test_alignment_and_independent_reference(self):
        import jiwer
        scorer = AccuracyEvaluator()
        rng = random.Random(42)
        for lang, alphabet in (("en", ["one", "two", "three"]), ("id", ["satu", "dua", "tiga"]),
                               ("zh", ["你", "好", "𠀀"])):
            for _ in range(100):
                ref = " ".join(rng.choices(alphabet, k=rng.randrange(10)))
                hyp = " ".join(rng.choices(alphabet, k=rng.randrange(10)))
                actual = scorer.score(ref, hyp, lang)
                r, h = actual["normalized_reference"], actual["normalized_hypothesis"]
                expected = jiwer.process_characters(r, h) if lang == "zh" else jiwer.process_words(r, h)
                self.assertEqual(actual["edits"], expected.substitutions + expected.deletions + expected.insertions)
                units = list(r) if lang == "zh" else r.split()
                self.assertEqual([op["reference"] for op in actual["alignment"] if op["reference"] is not None], units)
                units = list(h) if lang == "zh" else h.split()
                self.assertEqual([op["hypothesis"] for op in actual["alignment"] if op["hypothesis"] is not None], units)


if __name__ == "__main__":
    unittest.main()
