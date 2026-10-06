import io
import hashlib
from contextlib import redirect_stdout
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.models import acquire_model
from tools.datasets import fleurs_source as prepare_fleurs


class AcquisitionTest(unittest.TestCase):
    def test_model_new_acquisition_uses_locked_revision(self):
        pins = acquire_model.PIN
        metadata = {"sha": pins["revision"], "siblings": [
            {"rfilename": name, "size": 1, "lfs": {"sha256": "0" * 64}}
            for name in acquire_model.REQUIRED]}
        with tempfile.TemporaryDirectory() as temp:
            response = io.BytesIO(json.dumps(metadata).encode())
            with patch("urllib.request.urlopen", return_value=response) as request:
                with redirect_stdout(io.StringIO()):
                    acquire_model.acquire(Path(temp) / "model", metadata_only=True)
            self.assertIn("/revision/" + pins["revision"], request.call_args.args[0])
            manifest = json.loads((Path(temp) / "model" / "acquisition.json").read_text())
            self.assertEqual(manifest["revision"], pins["revision"])

    def test_verified_model_reused_without_optional_metadata_lookup(self):
        payload = b"pinned-test-model"
        sha = hashlib.sha256(payload).hexdigest()
        with tempfile.TemporaryDirectory() as temp:
            destination = Path(temp)
            manifest = {"model": acquire_model.MODEL, "revision": acquire_model.PIN["revision"],
                        "status": "verified", "files": []}
            for name in sorted(acquire_model.REQUIRED):
                (destination / name).write_bytes(payload)
                manifest["files"].append({"name": name, "sha256": sha, "expected_sha256": sha,
                                          "expected_size": len(payload)})
            (destination / "acquisition.json").write_text(json.dumps(manifest))
            with patch.dict(acquire_model.PIN, {"weights_sha256": sha}):
                with patch("urllib.request.urlopen") as request:
                    with redirect_stdout(io.StringIO()):
                        acquire_model.acquire(destination)
            request.assert_not_called()
            self.assertEqual(json.loads((destination / "acquisition.json").read_text())["status"], "verified")

    def test_existing_model_revision_mismatch_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            destination = Path(temp)
            (destination / "acquisition.json").write_text(json.dumps({"model": acquire_model.MODEL,
                "revision": "wrong", "files": []}))
            with patch("urllib.request.urlopen") as request:
                with self.assertRaises(RuntimeError):
                    acquire_model.acquire(destination, metadata_only=True)
            request.assert_not_called()

    def test_existing_fleurs_revision_mismatch_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            destination = Path(temp)
            (destination / "source.json").write_text(json.dumps({"dataset": "google/fleurs",
                "revision": "wrong"}))
            with patch.object(prepare_fleurs, "fetch_json") as fetch:
                with self.assertRaises(ValueError):
                    prepare_fleurs.acquire(destination)
            fetch.assert_not_called()

    def test_new_fleurs_acquisition_uses_locked_revision(self):
        payload = b"test-parquet-shard"
        sha = hashlib.sha256(payload).hexdigest()
        def listing(url):
            self.assertIn(prepare_fleurs.PIN["revision"], url)
            config = url.rsplit("/", 1)[-1]
            return [{"path": f"parquet-data/{config}/validation-00000.parquet",
                     "size": len(payload), "lfs": {"oid": sha}}]
        with tempfile.TemporaryDirectory() as temp:
            with patch.object(prepare_fleurs, "fetch_json", side_effect=listing) as fetch:
                with patch("urllib.request.urlopen", side_effect=lambda *args, **kwargs: io.BytesIO(payload)) as download:
                    with redirect_stdout(io.StringIO()):
                        metadata = prepare_fleurs.acquire(Path(temp))
            self.assertEqual(metadata["revision"], prepare_fleurs.PIN["revision"])
            self.assertEqual(fetch.call_count, 3)
            self.assertEqual(download.call_count, 3)


if __name__ == "__main__":
    unittest.main()
