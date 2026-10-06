# Build dependencies

The CPU build needs CMake, Ninja, a C++20 compiler, Git, Python 3.11 or newer, OpenSSL development files, pthreads, and OpenBLAS headers/runtime. Both presets need pinned yaml-cpp and nlohmann/json; `release-cpu` additionally needs the Qwen C CPU runtime and model weights.

The combined entry point is `python3 scripts/setup.py --frontend`; it fetches missing pinned assets and builds. The individual commands remain available:

```bash
bash scripts/fetch_foundation.sh
bash scripts/fetch_native.sh
python3 tools/models/acquire_model.py
cmake --preset release-cpu
cmake --build --preset release-cpu
```

Exact identities are in `revisions.lock`. Fetch scripts refuse a different existing revision; CMake rejects tracked vendor changes. Configure/build do not download dependencies. For a model-free build, use `dev-mock` and omit native/model acquisition.

On Fedora, install OpenBLAS headers with `sudo dnf install openblas-devel`. The existing host uses OpenBLAS 0.3.29 and local matching headers in `third_party/openblas-sdk`; CMake searches that path. Other distributions can use their OpenBLAS development package. OpenSSL and OpenBLAS are system libraries.

Only reviewed Qwen CPU sources are compiled, with OpenBLAS, `-O3 -march=native -ffast-math`. Binaries are host-specific. GPU targets and experimental llama/reference tools are outside the build.

The vendor checkout remains pristine. `scripts/prepare_qwen_guard.py` derives CPU sources in the build directory with an optional token-boundary cancellation/decode deadline guard; shared-prefix engines enable it. This cannot interrupt encoder/prefill or an individual kernel. See [Architecture](../docs/ARCHITECTURE.md).

Dependency checkouts/downloads are ignored. Their licenses must be preserved when redistributing; see [notices](../THIRD_PARTY_NOTICES.md).
