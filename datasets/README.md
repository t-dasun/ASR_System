# Dataset inputs

The retained manifests are **test inputs**, not results. They describe English (`en`), Indonesian (`id`), and Mandarin (`zh`) recordings from pinned Google FLEURS validation data, with references, sample counts, and SHA256 hashes.

- `manifests/demo_calls.jsonl`: four distinct longer WAVs for the multi-call demonstration.
- `manifests/fleurs_tuning.jsonl`: 20 recordings per language.
- `manifests/fleurs_validation.jsonl`: 50 recordings per language, disjoint sentence IDs from tuning.
- `manifests/fleurs.summary.json`: selection/source identity and manifest checksums.
- `manifests/excluded_sentence_ids.json`: original demo sentence exclusions, applied across languages.

The validation holdout is not the official FLEURS test split. Raw Parquet and prepared WAV directories are ignored. Preparation verifies cached source hashes and refuses to overwrite differing files:

```bash
python3 -m venv .venv-data
.venv-data/bin/pip install -r tools/datasets/requirements.txt
.venv-data/bin/python tools/datasets/prepare_fleurs.py
```

This downloads missing pinned shards into `datasets/raw/fleurs` and prepares `datasets/prepared/fleurs`. Existing local WAV inputs were retained during cleanup. The matrix driver itself needs only standard-library Python and those WAVs. See [Testing](../docs/TESTING.md).

FLEURS is CC-BY-4.0. Preserve dataset attribution when sharing derived audio or results; see [third-party notices](../THIRD_PARTY_NOTICES.md).
