# Architecture and code map

The opt-in `qwen_stream` runtime now adds resumable per-call state over shared model weights, reusing this pool and transport. See [implementation/pilot evidence](RESUMABLE_STREAMING.md). Descriptions of `qwen_prefix` below remain the legacy preview/EOF path used by the original capacity study.

## Runtime flow

```text
Browser / external client -- WebSocket PCM --> asr-cli serve
                                               |
CLI load --> paced C++ WAV call simulator ------+--> IASREngine
             direct: method calls              |      |
             network: internal WS clients      |      +-- MockEngine
                                               |      +-- SessionManager --> native worker per call
REST /v1/suites --> same C++ simulator ----------+      +-- PrefixMultiplexEngine (one worker)
                                                      +-- PrefixProcessPool --> prefix workers

Events --> transcript/UI + per-call artifacts + timing/resource measurements
Python matrix driver --> starts serve, posts suites, reads artifacts, scores transcripts
```

The main C++ inference path remains intact. Python neither sends every PCM chunk nor runs Qwen. The model is Qwen3-ASR-0.6B; the inference library is pinned `antirez/qwen-asr` C CPU code using OpenBLAS. `llama.cpp` and `whisper.cpp` are not current runtime dependencies.

## Worker and session ownership

`qwen_native` uses a native child/context for one active call on each occupied worker slot. Its progressive decoder and EOF refinement are the comparison baseline.

`qwen_prefix` loads one persistent context per worker and keeps each call's PCM, language, result revisions, deadlines, and lifecycle separately. One worker lives in the service process; two or more workers use persistent `asr-prefix-worker` children. Calls stay on the worker chosen at admission. This shares a context between active calls by serializing decode jobs. It is not simultaneous decoding, batched inference, or independent streaming KV caches.

Each call can receive one preview after the configured 4-second prefix, followed by whole-audio refinement at EOF. WAV chunks still arrive in real time. Preview frequency therefore differs from native streaming. Calls are bounded to 60 seconds of audio in this shared-prefix implementation.

The 4-second prefix is the default and the setting used in the capacity study. Live sessions may now select a 1–20 second preview interval through `prefix_preview_ms`; native sessions may select a 1–8 second `decode_step_ms`. Optional fields propagate through WebSocket and worker IPC, with zero retaining defaults. Shared model weights and thread/worker settings stay persistent while the per-call preview threshold changes. Browser arrival metrics use browser timestamps, and optional reference scoring follows the report's normalization policy.

## Scheduling decisions

- `least_active` routes new calls to the worker with fewer active sessions, spreading calls across free workers before sharing. `round_robin` is an alternative.
- A worker runs one decode job at a time. Ready previews use oldest-ready order.
- At most two previews can bypass a waiting EOF job; then the oldest EOF gets service. This reduces first-text delay while bounding finalization starvation.
- Expired calls are retired before choosing work. Admission respects configured session slots.
- Priority changes job order; it cannot interrupt an individual encoder/prefill/kernel operation.

The pinned vendor source stays unmodified. `scripts/prepare_qwen_guard.py` generates build copies with an optional token-boundary cancellation/decode deadline guard. Shared decoding enables it. This is cooperative cancellation, not a hard watchdog; a stalled kernel still requires an external supervisor. Native calls have process isolation and a watchdog. Shared worker failure is surfaced to affected calls; automatic worker respawn is not implemented.

## Source locations

| Area | Files |
|---|---|
| CLI parsing, single calls, load/sweeps | `apps/asr_cli/main.cpp` |
| Runtime selection, worker status, draining | `apps/asr_cli/engine_runtime.cpp` |
| Service startup, REST suite wiring, runtime snapshots | `apps/asr_cli/service.cpp` |
| Worker executable entry points | `apps/asr_native_worker/`, `apps/asr_prefix_worker/` |
| Audio conversion and paced delivery | `src/audio/` |
| Call/session lifecycle and worker routing | `src/backend/sessions/`, `scheduler/`, `workers/` |
| HTTP/WebSocket protocol and admission gate | `src/backend/transport/` |
| Runtime contract and implementations | `src/engines/` |
| Single/load/sweep runners | `src/benchmark/` |
| Strict YAML resolution | `src/config/` |
| Timing, CPU/memory sampling | `src/observability/` |
| Artifact publication | `src/storage/` |
| Dashboard | `frontend/src/` |
| Dataset/model inputs, scoring and experiment driver | `tools/` |

The public include paths remain `asr/...`; moving libraries under `src/` does not change their interfaces. CMake is split into foundation/core libraries, runtime targets, and tests.

## Service contracts

Live WebSocket ingress is `/v1/asr`; observation is `/v1/observe`. Calls have a unique identity and ordered media/sample watermarks. The protocol explicitly represents readiness, chunks, acknowledgements, EOF, cancellation, revisions, and terminal results. Partial text is provisional and can be replaced; it is not a stable word alignment.

REST exposes capabilities/config resolution, runtime snapshots, load/sweep planning and jobs, stopping jobs, history, and allowlisted artifacts. Important routes: `/v1/capabilities`, `/v1/config/resolve`, `/v1/runtime`, `/v1/suites/dry-run`, `/v1/suites`, `/v1/jobs`, `/v1/jobs/{id}`, `/v1/jobs/{id}/stop`, `/v1/history`, `/v1/artifacts/{id}/{file}`. The obsolete report registry API was removed; suite artifacts remain available.

## Limits and production work

This is a CPU prototype with bounded inputs. There is no SIP/RTP gateway, general VAD, diarization, autoscaler, durable job database, authentication, TLS termination, or cross-node routing. Production needs those facilities plus failure recovery and an evaluated language/latency/accuracy policy. Current sessions share model memory within each worker; separate workers retain separate contexts. Adding slots reduces duplicated model contexts, but does not create additional compute capacity.
