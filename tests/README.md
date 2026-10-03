# Tests

Run `ctest --preset dev-mock` or `ctest --preset release-cpu`. Seven fast model-free tests cover chunk deadlines, foundation/audio contracts, CLI artifacts, known metric timelines/quantiles, and replaceable resource sampling/loss. The native preset adds `native_adapter_contracts`, which uses a stand-in worker to check input/events, cancellation, timeout, crash, and pre-exec thread environment without model weights. `integration/audio_realtime_60.cpp` remains a manual 60-second gate. CTest needs no network/model/dataset/Python.

Offline M4 checks run with `.venv-reference/bin/python tests/unit/evaluation_test.py` (JiWER parity and Unicode/failure fixtures) and `.venv-reference/bin/python tests/unit/m4_workflow_test.py` (prepared manifest disjointness and selection/quantiles). The separate nine-call native measurement gate requires the local model and prepared data; see [M4 guide](../docs/m4-code.md).
