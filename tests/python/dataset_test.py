import hashlib
import json
import sys
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.datasets.prepare_fleurs import select_cohorts

class DatasetTest(unittest.TestCase):
    def test_selection_is_disjoint_and_reproducible(self):
        identities = {language: {str(i): i for i in range(10)} for language in ['en','id','zh']}
        selected = select_cohorts(identities, {'0'}, 42, 2, 3)
        self.assertEqual(selected, select_cohorts(identities, {'0'}, 42, 2, 3))
        self.assertNotIn('0', selected)
        self.assertEqual(sum(cohort=='tuning' for cohort,rank in selected.values()), 2)
        self.assertEqual(sum(cohort=='heldout_validation' for cohort,rank in selected.values()), 3)

    def test_current_input_manifests_match_summary(self):
        summary = json.loads((ROOT/'datasets/manifests/fleurs.summary.json').read_text())
        groups = {}
        for name, cohort in summary['cohorts'].items():
            path = ROOT/cohort['manifest']
            self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), cohort['sha256'])
            rows = [json.loads(line) for line in path.read_text().splitlines()]
            groups[name] = {row['source_id'] for row in rows}
            self.assertEqual(len(rows), cohort['recordings'])
            self.assertEqual({lang:sum(row['language']==lang for row in rows) for lang in ['en','id','zh']},cohort['per_language'])
            self.assertTrue(all(row['file'].startswith('datasets/prepared/fleurs/') for row in rows))
        self.assertFalse(groups['tuning'] & groups['heldout_validation'])

if __name__ == '__main__':
    unittest.main()
