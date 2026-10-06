import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.testing.metrics import distribution

class MetricsTest(unittest.TestCase):
    def test_type7_distribution(self):
        actual = distribution([4, 1, 3, 2])
        self.assertEqual(actual['count'], 4)
        self.assertEqual(actual['mean'], 2.5)
        self.assertEqual(actual['p50'], 2.5)
        self.assertAlmostEqual(actual['p95'], 3.85)
        self.assertEqual(actual['max'], 4)
        self.assertEqual(actual['unit'], 'ms')

    def test_missing_measurements_stay_missing(self):
        actual = distribution([], unit='ratio')
        self.assertEqual(actual['count'], 0)
        self.assertIsNone(actual['mean'])
        self.assertIsNone(actual['p95'])

if __name__ == '__main__':
    unittest.main()
