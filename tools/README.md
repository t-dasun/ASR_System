# Supporting tools

| Tool | Purpose |
|---|---|
| `models/acquire_model.py` | Acquire and verify pinned official weights |
| `datasets/prepare_fleurs.py` | Fetch pinned data and prepare deterministic disjoint WAV cohorts |
| `datasets/fleurs_source.py` | Shared source acquisition/hash helpers |
| `testing/run_shared_pool_matrix.py` | Start C++ services, run the four layouts, report per-language metrics |
| `testing/verify_pool_service.py` | Verify real-model two-worker/four-call network path and dashboard |
| `testing/capacity_stress.py` | Resource-guarded capacity sweep behind the matrix driver’s `--stress` mode |
| `testing/plot_capacity.py` | Export per-language capacity PNG/PDF figures from saved curves |
| `testing/scoring.py` | Unicode WER/CER normalization and alignment |
| `testing/metrics.py` | Timing field list and descriptive distributions |

The actual WAV simulation and inference are C++. These Python tools prepare inputs, orchestrate jobs, and evaluate saved outputs. See [Testing](../docs/TESTING.md).
