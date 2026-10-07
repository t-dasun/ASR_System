# Third-party components

Exact runtime, model and dataset pins are in [third_party/revisions.lock](third_party/revisions.lock). Fetched sources and weights remain local and ignored; preserve their full upstream notices when distributing them.

- yaml-cpp 0.8.0 and nlohmann/json 3.11.3: MIT; licenses remain in their fetched source checkouts.
- antirez/qwen-asr C CPU runtime: MIT, copyright 2026 Salvatore Sanfilippo; retain `third_party/qwen-asr/LICENSE`. The generated build-only decode guard does not modify the pinned vendor checkout.
- Official Qwen3-ASR-0.6B weights: Apache-2.0; downloaded model card and acquisition manifest identify the exact revision and files.
- Google FLEURS validation audio/transcriptions: CC-BY-4.0. Cite Google FLEURS and preserve attribution/license for derived WAV redistribution. Shard identities are in `datasets/raw/fleurs/source.json`; selected recordings are in the committed input manifests.
- OpenBLAS and OpenSSL: system libraries. Preserve notices for the actual binaries redistributed; they are not vendored here.
- Frontend: React/React DOM and Vite/Vitest are MIT; TypeScript and Playwright Core are Apache-2.0; React type definitions are MIT. Exact packages/integrities are in `frontend/package-lock.json`; review transitive package notices when distributing a bundle.
- Optional dataset tooling: PyArrow (Apache-2.0) and soundfile (BSD-3-Clause); native dependencies have their own notices. Optional JiWER scoring parity checks use its Apache-2.0 package.

Legacy ignored llama.cpp, converted model, or Python reference caches may remain on this workstation. They are outside the current build and experiment tools; their own licenses still apply if redistributed. This document is not a complete redistribution SBOM.

The generated CPU build also derives a resumable per-call streaming API from the pinned MIT-licensed Qwen C loop. `scripts/prepare_qwen_stream.py` retains shared model weights and separates mutable call state; vendor sources stay pristine and generated copies retain their original notices.
