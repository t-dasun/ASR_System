# Model acquisition

Run `python3 tools/models/acquire_model.py --metadata-only` to review the official small Qwen checkpoint metadata, then omit that option to acquire it. The first query pins an immutable revision in `models/qwen3-asr-0.6b/acquisition.json`; subsequent invocations reuse it. Weight size and upstream SHA256 are verified, and every acquired artifact gets a local SHA256. Existing unverified files and interrupted partials are preserved for inspection, not silently overwritten.

Model data is ignored by Git. Review the downloaded model card's license independently of the native runtime's MIT license. This tool does not install Python ML dependencies or run inference.
