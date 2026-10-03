# Results storage

M4 adds typed recognition publication time and additional `runtime_timing`, `system_metrics`, `calls`, `workers`, and `errors` JSONL streams in both repository implementations. Resource samples are collected in memory and persisted after sampling stops; additional streams flush when the call is sealed. Offline experiments wrap call directories with dataset snapshots, accuracy/alignment records, provenance and SHA256 checksums. See [M4 artifact guide](../docs/m4-code.md). Full crash recovery and asynchronous service persistence remain future work.

M1 implements `IResultRepository`, an in-memory test repository, and an exclusive-directory file repository for config/environment/events/summary/status. M2 adds per-chunk `audio_timing.jsonl`. Status replacement uses rename; event and timing writes are flushed. Crash recovery, production buffering, manifests, and history querying remain pending. M0 diagnostics retain their separate Python artifact writers.
