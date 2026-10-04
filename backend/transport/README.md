# M7 loopback API and streaming transport

`asr-cli serve --config configs/qwen_native_single.yaml --port 0` starts one C++ service on `127.0.0.1` and prints its assigned port. REST and WebSocket share that port and the same `SessionManager`/M6 load and sweep implementations as the CLI. `api_service.hpp/.cpp` owns REST routing and bounded asynchronous suite jobs; `websocket.hpp/.cpp` owns HTTP framing, v1 PCM transport, and a separate observation feed. The API does not import the Qwen runtime.

REST v1:

| Route | Meaning |
|---|---|
| `GET /v1/capabilities` | Engine/runtime and route capabilities, including MOCK flag |
| `GET /v1/runtime` | Interactive worker state and host CPU/RAM/process-tree RSS snapshot; queue unavailable |
| `GET /v1/jobs` | In-memory job IDs and current status, max 128 retained |
| `POST /v1/config/resolve` | Strict YAML plus supplied `{"overrides":["dataset.language=zh"]}`; no writes |
| `GET /v1/history`, `GET /v1/history/{id}` | Top-level run summaries/status; max 100 listed |
| `GET /v1/artifacts/{id}/{file}`, `/{id}/{call_id}/{file}` | Whitelisted suite and per-call JSON/JSONL artifacts, max 4 MiB |
| `GET /v1/reports`, `GET /v1/reports/{id}` | Existing `results/reports/{id}/report.json`; generation belongs to M10 |
| `POST /v1/suites/dry-run` | M6 load or sweep plan without output |
| `POST /v1/suites` | Start one bounded asynchronous M6 job; optional `idempotency_key` |
| `GET /v1/jobs/{id}`, `POST /v1/jobs/{id}/stop` | Job result/status and cooperative stop request |

Suite body example: `{"kind":"load","calls":2,"concurrency":1,"languages":["en"],"mode":"direct","overrides":["audio.realtime_pacing=true"]}`. For sweeps set `kind:"sweep"`, `strategy`, `axes` (`[{"key":"audio.chunk_ms","values":["100","200"]}]`), and optional `selected` cases. The server uses the fixed startup YAML; HTTP callers cannot change model path, audio path, or output directory. A shared admission gate prevents API benchmark suites and interactive calls from running simultaneously on the service. A stop request prevents the next call/case from starting; an already-running blocking native call remains bounded by its worker watchdog. Jobs are in-memory and limited to 128 retained records; artifacts survive in the configured output directory. Reusing an idempotency key with a different body returns 409.

`ws://127.0.0.1:<port>/v1/asr` accepts one live call per connection. After a masked JSON `start` (`v`, safe `run_id`/`call_id`, language, seed, max chunk samples, partial interval, 16 kHz sample rate), `ready` grants one credit. The client sends a masked `chunk` JSON descriptor then one masked binary little-endian PCM16 frame. The server validates sequence, sample offset/count and frame length, invokes the same session manager, and returns `ack` with one credit only after accepting the chunk. `event` and `observation` frames carry transcripts and runtime records. `eof` must include `expected_next_sequence` and `total_samples`; `cancel` is explicit. One `done` ends the connection. Duplicate EOF after `done` is not a second transaction: the server closes the call connection. Malformed or unmasked frames are rejected by closing the connection. The M6 `WebSocketEngine` is a scripted client for this exact protocol.

`ws://127.0.0.1:<port>/v1/observe?run_id=...&call_id=...&since=0` is the dashboard subscription path. It is separate from PCM ingress. Reconnect with the last `delivery_sequence` to replay retained `event`/`observation` messages. Each feed retains at most 128 messages; old messages are evicted with an explicit `gap` message on replay, and `observer_done` ends a terminal feed. Up to 128 feeds are retained, with terminal feeds evicted for new calls. A slow observer has its own socket thread and a two-second send timeout, so it cannot block audio ingress. The audio call socket itself is still a synchronous transport and should not be treated as an unbounded remote fan-out channel.

Safety bounds: IPv4 loopback only; HTTP headers ≤8 KiB, body ≤64 KiB, WebSocket frames ≤64 KiB, JSON controls ≤16 KiB; one chunk credit; origin allowlist `http://127.0.0.1` and `http://localhost` without port, plus exact local dev/preview ports 5173/4173 and the service port. Browser JSON requests receive an allow-listed CORS preflight. HTTP does not accept chunked requests. Paths are whitelisted IDs/files with symlink traversal refused. No TLS, authentication, remote exposure, multipart upload, persistent job database, or arbitrary report generation is supplied. This is a local engineering service, not a production Internet endpoint. M11 must specify TLS/auth/reverse proxy, retention, privacy, and production operations before deployment.

`websocket_contracts` and `api_service_contracts` exercise real localhost sockets. They cover incremental pre-EOF transcript delivery, ready/credit and EOF totals, disconnect/worker release, duplicate EOF termination, invalid origin/unmasked frames, observer replay and bounded gap reporting with an unread slow subscriber, REST resolution/history/report/artifact access, symlink/path denial, idempotent jobs, and stop state. Generic sandboxed CTest may deny loopback sockets; run these two tests with localhost permission.
