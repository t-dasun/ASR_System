"""Contract checks for the provisional M0 reference-scoring policy."""
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "reference"))
from scoring import normalize, score  # noqa: E402


class ScoringTest(unittest.TestCase):
    def test_english_case_punctuation_and_whitespace(self):
        self.assertEqual(normalize("  Hello,  WORLD!\n", "en"), "hello world")
        self.assertEqual(score("Hello, world!", "hello world", "en")["rate"], 0)

    def test_indonesian_word_edits(self):
        result = score("satu dua tiga", "satu tiga", "id")
        self.assertEqual((result["deletions"], result["reference_units"]), (1, 3))
        self.assertAlmostEqual(result["rate"], 1 / 3)

    def test_chinese_characters_ignore_spacing_and_punctuation(self):
        result = score("你 好，世界。", "你好世界", "zh")
        self.assertEqual(result["metric"], "cer")
        self.assertEqual(result["rate"], 0)
        self.assertEqual(result["reference_units"], 4)

    def test_empty_hypothesis_counts_deletions(self):
        for language, text, units in (("en", "one two", 2), ("zh", "你好", 2)):
            with self.subTest(language=language):
                result = score(text, "", language)
                self.assertEqual(result["deletions"], units)
                self.assertEqual(result["rate"], 1)


if __name__ == "__main__":
    unittest.main()
