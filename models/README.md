# Model acquisition

```bash
python3 tools/models/acquire_model.py --metadata-only
python3 tools/models/acquire_model.py
```

The standard-library tool acquires official Qwen3-ASR-0.6B at the immutable revision in `third_party/revisions.lock`, verifies size/SHA256, and records identity in `models/qwen3-asr-0.6b/acquisition.json`. Existing unverified files and interrupted downloads are refused for inspection.

`python3 scripts/setup.py` invokes this tool and then builds. Verified cached models are checked by hash without re-querying optional metadata; missing model files are downloaded.

Weights are ignored by Git and retained locally during cleanup. The runtime is C/C++; this tool installs no Python inference framework. Model license: Apache-2.0; see the downloaded model card and [notices](../THIRD_PARTY_NOTICES.md).
