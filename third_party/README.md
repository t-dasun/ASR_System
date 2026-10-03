# Dependencies

Run `bash scripts/fetch_foundation.sh` once to fetch pinned yaml-cpp 0.8.0 and nlohmann/json 3.11.3 (MIT). These are required by both presets. Configure/build/test do not fetch anything; CMake checks exact revisions and rejects tracked vendor changes. Pins live in `revisions.lock`. YAML parsing and JSON serialization use these libraries; engine/core/audio contracts do not depend on YAML or Qwen. Scoped CMake-policy and compiler-include settings accommodate this host's CMake 4/GCC 16 without editing vendor code.

## Native dependencies

Run `bash scripts/fetch_native.sh` to fetch the pinned Qwen source. CMake rejects a different commit or tracked modifications. The upstream MIT license remains in `qwen-asr/LICENSE`. Model licensing is separate. Vendor source and build/download artifacts are ignored by the parent project.

The CPU build needs CMake, Ninja, GCC/G++, Git, pthreads, and OpenBLAS headers/library. On Fedora the normal setup is `sudo dnf install openblas-devel`. The validation build uses upstream-equivalent `-O3 -march=native -ffast-math`; the resulting binary is host-specific. GPU source files and definitions are excluded explicitly.

If administrator installation is unavailable, download the matching x86_64 development package into `third_party/packages`, verify it with `rpm -K`, and extract it into `third_party/openblas-sdk` using `rpm2cpio` and `cpio`. CMake searches that directory for headers and uses the installed runtime library. Record the package version and hash with build evidence. Never extract unreviewed packages into `/`.

This host's initial local header package is `openblas-devel-0.3.29-2.fc43.x86_64.rpm`. The native runtime passed M0 feasibility gates and is selected for a process-isolated prototype adapter, not production use; see [the runtime decision](../docs/decisions/0001-native-qwen-runtime.md) and [the code/artifact guide](../docs/m0-code.md).
