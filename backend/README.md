# Backend modules

M5 adds `sessions/` (`SessionManager`), `scheduler/` (swappable least-active and round-robin policies), and `workers/` (executor boundary). A call is sticky to one worker generation until its handle is destroyed. The native executor creates a separate Qwen engine/context and process for each admitted call; the mock executor stays in-process for tests. Admission, draining, reset, deadline, and failure paths are covered by the M5 tests. See [M5 guide](../docs/m5-code.md).

This is not a long-lived/warm process pool: the native child is recreated for each call. `transport/` contains the M7 local REST/WebSocket service and the same versioned ingress used by M6 network-mode benchmarks; see its [protocol guide](transport/README.md). The M8 dashboard and production security/deployment remain ahead.
