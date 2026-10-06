"""Cleanup boundaries must protect assets and targets outside generated output."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "project_setup", Path(__file__).resolve().parents[2] / "scripts/setup.py"
)
setup = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(setup)


class SetupCleanupTest(unittest.TestCase):
    def test_results_keep_readme_and_do_not_follow_nested_symlink(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            results = root / "results"
            results.mkdir()
            (results / "README.md").write_text("guide")
            (results / "run").mkdir()
            (results / "run/result.json").write_text("{}")
            assets = root / "models"
            assets.mkdir()
            (assets / "weights").write_text("retain")
            (results / "external").symlink_to(assets, target_is_directory=True)
            with patch.object(setup, "ROOT", root):
                setup.clean_results()
            self.assertEqual([p.name for p in results.iterdir()], ["README.md"])
            self.assertEqual((assets / "weights").read_text(), "retain")

    def test_reject_cleanup_root_symlink_or_unapproved_asset_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            assets = root / "models"
            assets.mkdir()
            (assets / "weights").write_text("retain")
            (root / "build").symlink_to(assets, target_is_directory=True)
            (root / "results").symlink_to(assets, target_is_directory=True)
            with patch.object(setup, "ROOT", root):
                for path in [root / "build", root / "build/release-cpu", assets]:
                    with self.assertRaises(ValueError):
                        setup.remove_generated(path)
                with self.assertRaises(ValueError):
                    setup.clean_results()
            self.assertEqual((assets / "weights").read_text(), "retain")


if __name__ == "__main__":
    unittest.main()
