# Generated results

This directory starts empty except for this guide. Historical experiments were cleared on `dev-clean`.

CLI and service runs write their configured output here. The combined regression writes step logs/status to `regression_*`. Clear generated output with `python3 scripts/setup.py --clean-results --clean-only`. Example:

```bash
python3 tools/testing/run_shared_pool_matrix.py --output results/shared-pool --per-language 10
```

Generated files are ignored by Git. See [Testing](../docs/TESTING.md) for layouts, timing definitions, and the result bundle structure. Dataset manifests and model/dependency metadata are inputs, stored elsewhere.
