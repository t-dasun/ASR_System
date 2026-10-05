# Third-party components

The M1 foundation uses `yaml-cpp` 0.8.0 at commit `f7320141120f720aecc4c32be25586e7da9eb978` and `nlohmann/json` 3.11.3 at commit `9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03`, both under MIT. Their full notices remain in `third_party/yaml-cpp/LICENSE` and `third_party/json/LICENSE.MIT`. Preserve these when redistributing. Source checkouts are ignored; `scripts/fetch_foundation.sh` restores the pinned sources before an offline build. The CMake integration sets a compatible policy minimum and force-includes `<cstdint>` for yaml-cpp on GCC 16; vendor source is unmodified.

The native validation build uses `antirez/qwen-asr` at commit `924694251d9e0f18e5d86bbd06aa3ab5f870002d`, copyright 2026 Salvatore Sanfilippo, under the MIT license. Its full license is retained in the fetched source at `third_party/qwen-asr/LICENSE`; preserve that license when redistributing source or binaries.

The existing host OpenBLAS runtime is dynamically linked. The local development package is `openblas-devel-0.3.29-2.fc43.x86_64`, matching the installed OpenMP runtime package version. The extracted development package contains headers/libraries, not a complete redistribution notice bundle; obtain and preserve the applicable OpenBLAS notices before distributing binaries.

The acquired official Qwen3-ASR-0.6B model card declares Apache-2.0. Model weights are not committed. The model revision and artifact hashes are recorded in its acquisition manifest. The offline reference implementation is `QwenLM/Qwen3-ASR` at commit `7c6daf77a2421100f5fb066495372c00129d39ff`, under Apache-2.0; its license is retained in the local checkout at `third_party/qwen-reference/LICENSE`.

The optional time-multiplexing experiment uses `ggml-org/llama.cpp` at commit `806eee9841de5f2c20f9d43914117f157d2baacc` under MIT and a Q8 GGUF conversion of Qwen3-ASR-0.6B from `ggml-org/Qwen3-ASR-0.6B-GGUF` at revision `928ab958557df9aa2ef1c93e0e83c7ad0933fae2`. Its source, binary, GGUF model, and projector are local ignored artifacts; `third_party/revisions.lock` records their identities and hashes. Check the converted model's own terms and preserve the llama.cpp MIT notice before redistributing any experimental artifacts. This runtime is not yet integrated into the ASR service or its measured baseline.

The upstream sample recordings are used locally for smoke diagnostics only. They are not copied into this project's fixtures or presented as a newly licensed dataset.

The M8 frontend's exact direct package versions and transitive tarball
integrities are recorded in `frontend/package-lock.json` and installed with
`npm ci`. Direct packages at the current lock are React/React DOM 19.3.0
(MIT), Vite 8.3.2 (MIT), Vitest 5.0.3 (MIT), TypeScript 7.0.2
(Apache-2.0), Playwright Core 1.63.0 (Apache-2.0), and `@types/react` /
`@types/react-dom` 19.3.0 (MIT). These license labels come from the locked
package metadata. Before redistributing the frontend bundle or a container,
generate and review a complete transitive license inventory and include any
required notices; this file is not a completed software bill of materials.

OpenSSL is linked by the C++ transport for cryptographic helpers in the local
build; a production redistribution must also include the notices applicable
to the exact OpenSSL and OpenBLAS binaries packaged on the target host. Neither
system library is vendored by this repository.

The development recordings and transcriptions are from Google FLEURS, [`google/fleurs`](https://huggingface.co/datasets/google/fleurs), revision `70bb2e84b976b7e960aa89f1c648e09c59f894dd`, licensed CC-BY-4.0. The original data and derived 16 kHz PCM16 WAV files remain local and are not committed. Cite the FLEURS dataset when publishing results and preserve its attribution and license on any redistribution. The exact source shard URLs/hashes and selected recording IDs are recorded in `datasets/raw/fleurs_m0/source.json` and `datasets/manifests/fleurs_m0.jsonl`.
