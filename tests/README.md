# Maintained tests

- `unit/`: C++ audio, metrics, configuration, load/sweep, and scheduler contracts.
- `integration/`: C++ native/pool worker stubs, session lifecycles, REST/WebSocket contracts.
- `python/`: acquisition pins/model reuse, input manifest consistency, scoring/timing, and cleanup boundaries.
- `fixtures/`: small WAV/manifest inputs; they are required test data.

Use `python3 scripts/regression.py` for the combined suite, or `--full` for real-model workflows. Individually run `ctest --preset release-cpu` and `python3 -m unittest discover -s tests/python -p '*_test.py' -v`. Browser tests live in `frontend/tests/`. Real-model corpus experiments live in `tools/testing/`. See [Testing](../docs/TESTING.md) for their different scopes.
