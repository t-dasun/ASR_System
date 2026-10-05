# CPU Real-Time Multilingual ASR Assignment: Questions and Evidence-Based Answers

This answers the questions and requested design decisions in `AIML_CPP_LEAD_ASR_Technical_Assignment_v2.docx` against the implementation and results available on 2026-10-05. **Measured** means a recorded local run; **implemented** means code and tests exist; **proposed** means a production design without deployment evidence; **unverified** means the requested conclusion cannot be supported yet. The principal synthesis is the [final technical report](../reports/FINAL_TECHNICAL_REPORT.md); raw results linked below are local artifacts and may be absent from a source-only Git clone because `results/` is ignored.

## 1. Objective and working demonstration

### Q1. Was a CPU-only, real-time, multilingual Qwen3-ASR proof of concept built?

**Answer — implemented and demonstrated.** The C++20 service accepts progressively paced PCM from a browser, passes it through session management to an isolated native Qwen3-ASR-0.6B CPU worker, and pushes transcript revisions and a final result over WebSocket. A real Chrome-to-C++-to-Qwen run completed in English, Indonesian, and Mandarin, with first text before audio EOF, all chunks acknowledged, and zero browser errors. This proves a functioning prototype, not production-grade latency or capacity. Evidence: [three-language demo](../results/m11_three_language_demo_20261005_long/demo.json), [native adapter](m3-code.md), [transport](../backend/transport/README.md).

### Q2. Does the user load a Linear PCM WAV, and does the system avoid one-shot offline transcription?

**Answer — yes for the main path.** The browser validates and slices a 16 kHz mono PCM16 WAV, sends ordered chunks at audio-time deadlines, and receives revisions while the file is still being sent. The browser does not play the WAV through speakers. The C++ WAV layer can additionally normalize PCM8/16/24/32 or float32, 8–192 kHz, and 1–8 channels: it applies the configured mono policy, a windowed-sinc resampler, and PCM16 quantization. WAV preparation itself is bounded and done before pacing; the measured stream does not hand the entire WAV to the engine at time zero. Evidence: [browser audio path](../frontend/README.md), [WAV and pacing design](m2-code.md), [demo](../results/m11_three_language_demo_20261005_long/demo.json).

### Q3. Are partial and final results distinguished in the UI?

**Answer — yes, with a semantic limit.** The native token callback emits provisional full-text snapshots with increasing revision numbers; text can be revised or truncated. A separate full-audio inference pass after EOF produces the final result. These partials are not stable prefixes or word-timestamped results. The UI shows evolving revisions and the final state. Evidence: [native adapter](m3-code.md), [dashboard](../frontend/README.md), [demo](../results/m11_three_language_demo_20261005_long/demo.json).

### Q4. Are start, stop, reset, and visible stream state available?

**Answer — implemented.** The streaming panel owns WAV/language/chunk selection, start/stop/reset, acknowledgments, revisions, and terminal state. The transport has explicit `start`, `ready`, `chunk`, `ack`, `eof`, `cancel`, and `done` states. A stop/cancel ends the call; reset starts a fresh call identity rather than reusing model state. Evidence: [dashboard](../frontend/README.md), [WebSocket contract](../backend/transport/README.md), [delivery contract](m2-code.md).

### Q5. What did the actual three-language browser demo show?

| Language | Audio length | Chunks ACKed | Revisions | Browser first text | Outcome |
|---|---:|---:|---:|---:|---|
| English | 15.36 s | 77/77 | 31 | 7.206 s | Completed, nonempty final |
| Indonesian | 15.84 s | 80/80 | 34 | 7.203 s | Completed, nonempty final |
| Mandarin | 15.58 s | 78/78 | 28 | 7.201 s | Completed, nonempty final |

**Answer — measured, three illustrative calls.** These are individual warm-service demonstrations, not latency percentiles or an accuracy cohort. Transcripts and screenshots are saved in the [demo bundle](../results/m11_three_language_demo_20261005_long/demo.json).

## 2. Streaming and audio design decisions

### Q6. Why 16 kHz mono PCM16, 200 ms chunks, and a 2-second decode step?

**Answer — implemented design choice.** 16 kHz mono signed PCM16 is the normalized ASR input and the WebSocket wire format. A 200 ms frame contains 3,200 samples and gives manageable transport/flow-control granularity. The native model's configured 2,000 ms decode step is independent of transport framing: 200 ms chunks do **not** imply 200 ms text latency. Four compute threads, a 45-second call watchdog, and EOF refinement are the baseline settings. Evidence: [baseline YAML](../configs/qwen_native_single.yaml), [audio contract](m2-code.md), [native adapter](m3-code.md).

### Q7. How are pacing, buffering, backpressure, and EOF handled?

**Answer — implemented for the prototype.** A monotonic clock releases samples at absolute deadlines; late work does not shift the media timeline. A bounded audio controller defaults to eight chunks, 1,000 ms or 32,000 bytes queued, 1,000 ms maximum send lag, and a 5 ms late threshold. Exceeding a bound fails the call explicitly instead of silently dropping audio. The browser/server protocol grants one chunk credit after the previous chunk is accepted. Normal EOF includes expected next sequence and total sample count; the worker then refines the final transcript. Cancellation and timeout are separate terminal paths. Evidence: [M2 design](m2-code.md), [WebSocket protocol](../backend/transport/README.md), [native worker](m3-code.md).

### Q8. What is one call leg, and how are calls isolated and scheduled?

**Answer — implemented.** One independently streamed source is one call leg with its own call ID, sequence/sample watermark, session, worker ownership, and result stream. The current native adapter uses one model context and inference slot per isolated process; a bounded session manager admits/schedules work and can recycle worker slots. Process failure, cancellation, and watchdog expiry end that call explicitly. Same-process concurrent calls, shared model weights, and batching are not implemented or validated **in this adapter**; this is not a claim that Qwen3-ASR-0.6B as a model cannot support them in another runtime. Evidence: [session design](m5-code.md), [native adapter](m3-code.md), [production proposal](m11-production-architecture.md).

### Q9. How would real telephony replace the WAV simulator?

**Answer — proposed.** A SIP/RTP or existing-platform media gateway would terminate the call, validate timestamps/sequence, decode negotiated codecs such as G.711, resample to 16 kHz mono PCM16, and feed the same call-leg contract. Codec counters and discontinuities would be retained separately. The current service has no SIP/RTP termination; its μ-law test transform is simulated channel degradation, not a production telephony connector. Evidence: [production architecture](m11-production-architecture.md), [telephone simulation](m9-code.md).

### Q10. What happens with silence, VAD, end-of-utterance, long speech, interruptions, and jitter?

**Answer — partly implemented, partly proposed.** Exact digital zero has a narrow empty-final rule; general silence/VAD and end-of-utterance detection are not implemented. VAD remains off in measured Qwen runs. A 93.38-second replay/silence/interruption fixture completed with a 150-second watchdog, 467/467 chunks, zero overflow/late chunks, 7.00-second first text, and 111.54-second final result; the original 45-second watchdog correctly failed that long call. Network delay, disconnect, timeout, and overload have model-free contract tests, but production RTP jitter needs a timestamped jitter buffer, gap policy, and explicit cancellation/restart semantics. Overlapping speakers, barge-in, and genuine speech segmentation need a versioned VAD/diarization policy and separate evaluation. Evidence: [M9 stress and fault results](m9-code.md), [M9 evidence matrix](m9-evidence-matrix.md), [production architecture](m11-production-architecture.md).

## 3. Model and runtime selection

### Q11. Which Qwen variant, precision, and runtime were chosen, and why?

**Answer — implemented choice.** The project uses pinned Qwen3-ASR-0.6B with the native C/C++ CPU path, BF16 weights, four compute threads, and one process/model instance per inference slot. The smaller variant and C++ integration fit the CPU-only prototype and avoid a Python inference service in the measured path. The official Python implementation is used only for reference validation and data tooling. This selection is a pragmatic baseline, not proof that Qwen is the best production model. The pinned weight file is about 1.88 GB; exact revision and hashes are in the [final report](../reports/FINAL_TECHNICAL_REPORT.md) and [pin file](../third_party/revisions.lock). Evidence: [runtime decision](decisions/0001-native-qwen-runtime.md), [adapter](m3-code.md), [config](../configs/qwen_native_single.yaml).

### Q12. Was a C++ inference path feasible, and what are its limitations?

**Answer — yes, measured.** `asr-native-worker` links the pinned native Qwen implementation; C++ owns PCM ingestion, live decoding, event emission, and final refinement. A reader thread stages incoming PCM while the vendor live call blocks. The current adapter keeps one model per worker, uses a bounded 20-second staging buffer, and retains full audio for EOF refinement. It does not expose active decode boundaries, stable-prefix timing, word timestamps, shared-weight concurrency, or general no-speech detection. Evidence: [native adapter](m3-code.md), [measurement guide](m4-code.md), [worker source](../engines/native/src/worker_main.cpp).

### Q13. Were at least two sensible Qwen configurations compared?

**Answer — yes, as a limited one-factor screen.** The same nine clean FLEURS tuning clips were run with four versus two compute threads. Accuracy edit totals were unchanged: EN 1/59 words, ID 2/44 words, ZH 0/73 characters. Four-thread to two-thread p50 first text changed from 7.20→7.44 s (EN), 9.09→9.72 s (ID), and 7.20→7.40 s (ZH); p50 final changed from 10.06→10.76 s, 10.59→11.54 s, and 12.51→13.48 s. The runs were sequential with uncontrolled cache/thermal order, so this does not establish an optimal thread count or a capacity advantage. Evidence: [comparison JSON](../results/m9_qwen_threads2_tuning3/compare_threads4.json), [M9 runbook](m9-code.md).

### Q14. Are cold start and model load separated from steady recognition?

**Answer — instrumented, with limits.** Startup is session-creation request to worker-ready; model load is the `qwen_load` call interval; live invocation and EOF refinement are separate intervals. Process CPU and sampled RSS are also recorded. The report does not claim a statistically controlled cold-cache versus warm-cache distribution: OS file cache, frequency, and thermal state were not controlled. Do not treat model load as part of first-text timing after worker readiness, or the paced live-invocation wall time as active decoder compute time. Evidence: [metric definitions](m4-code.md), [raw call artifact description](m4-code.md), [final report](../reports/FINAL_TECHNICAL_REPORT.md).

### Q15. What Qwen streaming or CPU limitations were found, and what workaround was used?

**Answer — measured and implemented.** The live API yields seconds-scale, revisable provisional text and may truncate it; the adapter records revisions and performs a separate full-audio EOF call for a final result. This raises final latency and retains audio. The model API does not expose active compute RTF or stable word alignment. One process per slot isolates failures but duplicates model memory. The baseline has no GPU dependency. Evidence: [adapter](m3-code.md), [final report](../reports/FINAL_TECHNICAL_REPORT.md), [M10 capacity screen](m10-code.md).

## 4. Measurement methodology and results

### Q16. What exactly do the latency and RTF metrics mean?

**Answer — defined in the C++ measurement layer.** First usable text is first nonblank partial or final publication minus paced stream origin. Final latency is final publication minus stream origin. Finalization delay is final publication minus worker EOF receipt (or controller EOF request when that boundary is unavailable). Scheduled final lag is final publication minus scheduled availability of the last sample. Effective RTF is final latency divided by unique source duration and **includes pacing and EOF refinement**; it is not active decoder compute RTF. Browser first-text uses `performance.now()` and is reported separately from server monotonic timestamps. The result files include type-7 mean/p50/p95/p99 distributions, units, counts, and populations where available. Evidence: [M4 metric definitions](m4-code.md), [browser timing](../frontend/README.md).

### Q17. What hardware, OS, software, and baseline settings produced the numbers?

**Answer — recorded host and config.** Ryzen 9 9955HX laptop, 16 physical cores/32 logical CPUs, about 14.8 GiB RAM, Fedora Linux kernel 7.2.7, GCC 16.2.1, CMake 4.3.0, native Qwen3-ASR-0.6B BF16-weight CPU path, four model threads, 200 ms PCM chunks, 2,000 ms decode step, one slot per process, no CPU affinity. The preflight governor was `powersave`; frequency and thermals were not controlled. Vendor/model/data revisions and frontend/Python dependency constraints are pinned separately. A new CPU requires a rebuild and new benchmarks. Evidence: [host snapshot](../results/m11-host.json), [config](../configs/qwen_native_single.yaml), [pin file](../third_party/revisions.lock), [reproduction guide](m11-reproduction.md).

### Q18. What did the labelled accuracy evaluation show, and how was it scored?

**Answer — measured on a small FLEURS validation cohort.** Five distinct held-out validation clips per language were each run clean and after a deterministic simulated telephone transform: 30 completed calls. Error rate is total substitutions + deletions + insertions divided by total reference units. EN/ID use words; ZH uses Unicode characters. Normalization uses NFC, case folding for EN/ID, punctuation removal (spaces for EN/ID), whitespace collapsing, and no Chinese spaces. The paired cohort is neither official FLEURS test nor real contact-center audio. Evidence: [scorer](../evaluation/scoring.py), [paired raw comparison](../results/m9_fleurs_holdout5_20261004/comparison.json), [M10 report](../results/reports/m10_evidence_20261005_v5/report.json).

| Language | Clean | Simulated telephone | Reference units | Clean first-text p50 | Telephone first-text p50 |
|---|---:|---:|---:|---:|---:|
| EN WER | 6/103 = 5.83% | 13/103 = 12.62% | 103 words | 7.20 s | 7.00 s |
| ID WER | 5/88 = 5.68% | 13/88 = 14.77% | 88 words | 9.61 s | 9.24 s |
| ZH CER | 9/161 = 5.59% | 4/161 = 2.48% | 161 characters | 7.20 s | 7.20 s |

The Mandarin telephone improvement on five clips is an observation, not a general channel benefit. Across all 30 calls, first-usable p50/p95 were 7.20/10.20 s. Failure-inclusive and completed-only denominators are stored separately; all 30 calls completed.

### Q19. What are the single/two-worker latency, throughput, RTF, and memory results?

**Answer — measured diagnostic screen.** Each point offered 20 repeats of one 1.2-second English WAV. All 40 calls completed. These are short screens below one minute per point, not steady-state capacity. Source: [M10 JSON](../results/reports/m10_evidence_20261005_v5/report.json), [one-worker raw summary](../results/m10_capacity_screen_20261005/load_18db68ccd684b375_7f442e1bb99b0dfa95b11d3769d58c42/summary.json), [two-worker raw summary](../results/m10_capacity_screen_20261005/load_18db68dbcffe241f_23a0c002a06389c6911d8cb968eb5c11/summary.json).

| Active workers | First text mean / p50 / p95 / p99 | Final mean / p50 / p95 / p99 | Effective RTF p95 | Audio s / wall s | Sampled peak process-tree RSS |
|---:|---|---|---:|---:|---:|
| 1 | 1.673 / 1.670 / 1.686 / 1.691 s | 2.267 / 2.263 / 2.293 / 2.298 s | 1.91 | 0.467 | 2.59 GiB |
| 2 | 1.926 / 1.918 / 2.041 / 2.060 s | 2.778 / 2.778 / 2.936 / 2.957 s | 2.45 | 0.770 | 5.17 GiB |

Two workers gave 1.65× aggregate throughput, while p95 first text rose 355 ms and memory roughly doubled. Effective RTF over 1 here includes the deliberate 1.2-second audio schedule plus final refinement; it must not be interpreted as active compute RTF. The audio-seconds/wall-seconds figure includes suite overhead and is not a sustainable call rating.

### Q20. Are CPU utilization, process CPU, cores, memory, queueing, errors, and dropped/late chunks observable?

**Answer — largely instrumented, with missing boundaries.** A Linux `/proc` sampler records host/process-tree CPU and RSS at configurable intervals (200 ms default), and worker observations include user/system CPU time and peak RSS. Chunk timing records scheduled/read/send lag, queue watermark, lateness, and overflow; load summaries record failures, throughput, and latency distributions. Sampled RSS may miss peaks or overcount shared pages. Active decoder CPU/RTF and true model backlog are unavailable from the vendor API; the dashboard's live queue depth is deliberately `null`. The 20-call capacity screens had zero failures and p95 max send lag of 0.640/0.734 ms. A three-worker trial was rejected by conservative RAM preflight, so no CPU saturation knee was measured. Evidence: [measurement design](m4-code.md), [M10 results](m10-code.md), [dashboard telemetry](m8-code.md).

### Q21. Does the system stay below real time under load, and was a saturation point found?

**Answer — not established.** The short 1.2-second fixture had effective RTF p95 of 1.91/2.45 at one/two workers, while aggregate audio throughput was 0.467/0.770 audio seconds per wall second. Those metrics include pacing, refinement, and suite overhead and do not by themselves prove sustained inability or ability to serve continuous calls. There was no representative mixed-language sustained sweep to a measured latency/failure knee. Three workers were RAM-preflight rejected at about 11.0 GiB required versus about 8.5 GiB available at that check. Evidence: [M10 capacity analysis](m10-code.md), [machine-readable report](../results/reports/m10_evidence_20261005_v5/report.json).

## 5. Load testing and CPU capacity

### Q22. Is there a repeatable multi-call load mechanism?

**Answer — implemented.** `asr-cli load-dry-run`, `load`, `sweep-dry-run`, and `sweep` offer seeded direct or WebSocket calls, concurrency levels, preflight, and stored per-call/suite artifacts. The browser can submit the same load/sweep jobs to the local API. A two-worker staggered three-call native screen completed 3/3 with worker recycling; the 20-call one/two-worker screens are the stronger short diagnostic pair. Reproduction commands and configurations are in the [M11 runbook](m11-reproduction.md) and [M10 guide](m10-code.md). This is a load mechanism, not a qualified production capacity curve.

### Q23. What CPU/vCPU, RAM, nodes, RTF, and p95 should be planned for 50, 100, 200, 500, and 1,000 concurrent legs?

**Answer — unverified; no defensible numerical sizing is available.** The assignment asks for those rows, but the current laptop screens do not provide safe calls per node, target production RTF/p95, a measured failure knee, or headroom. A vCPU would mean one logical hardware thread, but that does not transfer throughput across CPU models. Filling these cells by multiplying the 1–2-worker result would be misleading. The [M10 sizing records](../results/reports/m10_evidence_20261005_v5/report.json) deliberately set each field to `null` with `INSUFFICIENT_EVIDENCE`.

| Concurrent legs | Estimated vCPU | RAM/node | Nodes/specification | Target RTF | Target p95 latency | Basis/confidence |
|---:|---|---|---|---|---|---|
| 50 | Unknown | Unknown | Unknown | Undefined | Undefined | 1–2-worker diagnostic only; none |
| 100 | Unknown | Unknown | Unknown | Undefined | Undefined | Same; none |
| 200 | Unknown | Unknown | Unknown | Undefined | Undefined | Same; none |
| 500 | Unknown | Unknown | Unknown | Undefined | Undefined | Same; none |
| 1,000 | Unknown | Unknown | Unknown | Undefined | Undefined | Same; none |

#### Current architecture cost and conditional sizing calculation

The current configuration permits **one active call, one model instance, and one inference slot per worker process**. Thus `L` simultaneous admitted legs require at least `L` active Qwen worker processes across the fleet **with this implementation**. The limit is imposed by the selected native runtime and adapter design: the pinned runtime uses process-global thread-pool and scratch/cache state, and same-process concurrent inference has not been shown safe. The session manager therefore validates these three limits as exactly one. This does **not** establish that the Qwen3-ASR-0.6B model inherently forbids concurrent sessions, shared weights, or batching in another runtime or a redesigned adapter. Sequential reuse of one loaded model across calls is also not implemented here: each admitted native session creates a fresh child/context. Evidence: [native runtime decision](decisions/0001-native-qwen-runtime.md), [session manager](../backend/sessions/src/session_manager.cpp), [M5 design](m5-code.md).

Each worker is configured for four model compute threads, plus runtime/transport threads, so `4 × L` is a count of configured **model threads across the fleet**, not a vCPU requirement. The checkpoint occupies about 1.88 GB on disk. In the short screen, sampled controller-plus-worker process-tree RSS was 2.59 GiB at one active worker and 5.17 GiB at two; the observed increase was about 2.58 GiB. These are sampled laptop values, not portable per-worker memory guarantees. The existing load preflight uses a more conservative **3 GiB per native worker plus 2 GiB reserve**, using available RAM at the time of the check. Evidence: [baseline config](../configs/qwen_native_single.yaml), [preflight rule](m6-code.md), [M10 screen](m10-code.md).

For a target node, define `M_available` as RAM available to this service after the OS and unrelated applications, `R` as an explicit reserve, and `K_memory` as the preflight worker limit. The current preflight approximation is `K_memory = floor((M_available - 2 GiB) / 3 GiB)`, subject to a nonnegative result. A real admission limit must also satisfy `K_slots` (configured workers) and `K_slo` (the largest **sustained** mixed-call concurrency that meets the declared latency, accuracy, error, CPU, queue, and memory limits). With headroom fraction `h` between 0 and 1, a candidate per-node admission limit is:

```text
K_node = max(0, floor(h × min(K_memory, K_slots, K_slo)))
N_base(L) = ceil(L / K_node)                 if K_node > 0
N_total(L) = N_base(L) + N_failure_reserve  after a failure-domain policy is chosen
```

Here `K_slo` and `h` are **unknown**, so this formula cannot yet produce qualified `N_total` values. Headroom must be checked against a measured load knee; treating a 20-call diagnostic pass as `K_slo = 2` would be unjustified. To show only the process topology, if one *assumes* two continuously safe calls per identical node with no headroom or failover reserve, the arithmetic is as follows. It is **not** a deployment estimate or a claim that the laptop sustains two real calls indefinitely:

| Concurrent legs `L` | Required Qwen processes | Conditional nodes at 2 slots/node, no reserve | Configured model threads fleet-wide |
|---:|---:|---:|---:|
| 50 | 50 | 25 | 200 |
| 100 | 100 | 50 | 400 |
| 200 | 200 | 100 | 800 |
| 500 | 500 | 250 | 2,000 |
| 1,000 | 1,000 | 500 | 4,000 |

The conditional node column is a **slot count only**. It excludes target-host CPU efficiency, memory baseline and shared pages, network/gateway cost, long-call context, queue reserve, model-load replacement capacity, and node-failure reserve. The measured 1→2-worker throughput gain was 1.65×, which already shows why doubling worker count is not a validated throughput multiplier. A production bill of materials requires the missing `K_slo`, `h`, node specification, and reserve policy.

### Q24. How should a valid sizing model account for shared memory, batching, contention, queues, NUMA, headroom, and diminishing returns?

**Answer — proposed method, awaiting measurements.** First declare latency/accuracy/error and memory SLOs plus a representative language, duration, silence, and codec mix. On target server CPUs, measure repeatable staggered load through the first p95/p99, queue-age, error, CPU, or RSS knee, then repeat at sustained duration with failure/drain disturbances. Choose an admission limit **below** that measured boundary with a validated reserve; node count becomes `ceil(offered simultaneous legs / validated safe legs per node)`, then add failure-domain reserve and test the resulting fleet. Test shared model weights/batching separately because the present process-per-slot design duplicates roughly model-sized memory; measure latency benefit and isolation rather than assuming it. Account for per-core/NUMA locality, thread oversubscription, queue growth, and the observed 1.65× rather than 2× two-worker throughput. No numerical headroom percentage or node specification is justified yet. Evidence: [capacity method](m10-code.md), [production design](m11-production-architecture.md).

### Q25. What happens when capacity is exceeded, and how would horizontal scaling work?

**Answer — prototype behavior implemented; fleet behavior proposed.** Current audio bounds and load preflight reject/terminate overload explicitly; accepted calls do not silently accumulate unbounded PCM. A production ingress should admit only against measured reserved worker slots and memory, return an explicit overload/retry response before taking media, and keep each active call sticky to its owner node. Add identical worker nodes only after target-hardware safe capacity is known; a gateway/controller can route and drain calls, but more gateways cannot remove worker CPU/RAM limits. Worker crash loses mutable decoder state, so report terminal failure and start a new call context only with explicit client policy. Evidence: [audio bounds](m2-code.md), [load/capacity finding](m10-code.md), [production design](m11-production-architecture.md).

## 6. Architecture and production decisions

### Q26. What is the implemented POC architecture, and what is proposed for production?

**Answer — architecture below.** Current components are modular C++ audio, engine, session/scheduler, storage, measurement, and transport layers, with a React dashboard and a process-isolated Qwen worker. The second path is a proposal, not an installed deployment. Evidence: [architecture/contracts](planning/ARCHITECTURE_AND_CONTRACTS.md), [production design](m11-production-architecture.md).

```text
POC: Browser WAV -> paced PCM WebSocket -> C++ session/scheduler
                                      -> isolated native Qwen worker
                                      -> transcript + metrics -> WebSocket/UI + local artifacts

Proposed: SIP/RTP -> codec/media gateway -> authenticated TLS/WSS ingress
                                      -> sticky admission/call owner
                                      -> bounded queue -> CPU ASR worker fleet
                                      -> idempotent transcript delivery + observability
```

### Q27. Why WebSocket, and what are the API/result-delivery contracts?

**Answer — implemented choice.** A single bidirectional WebSocket carries binary PCM and asynchronous transcript/observation events with ordered sequence/sample offsets, one-credit ACK flow control, and explicit EOF/cancel. A separate observer WebSocket can replay retained events by delivery sequence, with bounded history and explicit gap notification. REST handles local configuration resolution, jobs, runtime snapshots, history, and artifacts. This matches browser streaming and avoids per-chunk HTTP request overhead. A production telephony gateway could instead use an internal streaming RPC, but that is unimplemented and needs a measured contract. Evidence: [M7 transport](../backend/transport/README.md).

### Q28. How are worker lifecycle, health, draining, failure recovery, and session affinity designed?

**Answer — partial implementation plus proposal.** The prototype uses process isolation, bounded admission, watchdog, cancellation, crash detection, worker recycling, and a local runtime endpoint. Production design adds health/admission state, no-new-calls draining, sticky call assignment, terminal failure on lost decoder context, and idempotent downstream events keyed by call ID plus sequence. It does not claim transparent state migration or deployed distributed load balancing. Evidence: [session manager](m5-code.md), [M7 API](../backend/transport/README.md), [M11 design](m11-production-architecture.md).

### Q29. What production security and privacy controls are required?

**Answer — proposed, not implemented.** The current service binds loopback and checks Origin; neither authenticates users. Production requires TLS/WSS and service identity, tenant/call authorization, admission quotas and rate limits, bounded frames, encryption at rest, limited privileges/egress, verified model artifacts, default no raw-audio retention, separate transcript/audio access, defined consent/retention/deletion, PII-aware logs, and an auditable on-premise release process. The current local artifacts may contain transcript/source paths and are not a regulated storage design. Evidence: [M11 security plan](m11-production-architecture.md), [current transport limits](../backend/transport/README.md).

### Q30. What observability and operational signals are available or needed?

**Answer — implemented locally, expanded in proposal.** The prototype records per-call timings, resource samples, raw events, result revisions, load errors, send lag, and service runtime snapshots. Production should also trace gateway receipt, queue age, admission/rejection, per-core saturation, worker restarts, first stable text, publish delay, failures/no-output, and p50/p95/p99 with denominators, without putting raw transcript text in metric labels. Alerts should cover queue growth, headroom loss, crashes, and SLO breaches. Evidence: [measurement](m4-code.md), [dashboard](m8-code.md), [M11 operations](m11-production-architecture.md).

## 7. Alternatives and independent recommendation

### Q31. Is Qwen3-ASR automatically the best production choice? Which alternatives were compared?

**Answer — no.** The [alternative analysis](m9-alternatives.md) researches multilingual Whisper `small` with whisper.cpp and omniASR-CTC-300M INT8 with sherpa-onnx for EN/ID/ZH, native integration, model footprint, quantization, license, and deployment complexity. Neither is installed as an adapter or benchmarked locally; upstream memory/file figures are not host measurements. Whisper's repeated-window microphone example and sherpa's offline/segmented path are not equivalent to persistent causal streaming. Timestamps, hotwords/domain vocabulary, partial stability, CPU p95/RSS, and maintenance burden require same-protocol validation. Whisper `small` is the first candidate for a fair local adapter test; sherpa is a plausible segmented/INT8 baseline. Keep Qwen as the measured incumbent until those trials finish.

### Q32. What would change for strict sub-second partials at hundreds of calls?

**Answer — the current Qwen path would not meet that requirement on present evidence.** The 30-call paired validation first-text p50 is 7.20 s, and the browser demonstrations are about 7.2 s. A new low-latency streaming/VAD or language-routed architecture, smaller/quantized model, shared-weight or batched worker strategy, and a different admission envelope would need to be tested. Evaluate true pre-EOF first text, stability, WER/CER, CPU, RSS, and p95/p99 at hundreds of representative calls before recommending it. Windowed offline preview must be labelled as provisional if used. This is a hypothesis and experiment plan, not a measured alternative result. Evidence: [M10 sub-second finding](m10-code.md), [alternative matrix](m9-alternatives.md), [production design](m11-production-architecture.md).

### Q32a. What code and design changes are required to share a loaded Qwen model across calls?

**Answer — two different goals require different changes.** *Warm sequential reuse* would keep one child process and model loaded between calls, then reset all decoder/audio/prompt state before the next call. This may reduce repeated startup and loading, but still serves only one active call per process. It requires a persistent worker lifecycle, a per-call `start`/`eof`/`cancel` IPC protocol, model/context reset guarantees, idle/restart policy, and repeated-call leak and cross-language isolation tests. The current manager creates a fresh child/context for each admitted session.

*True concurrent call sharing* requires more than changing `workers.max_sessions_per_process` from 1. The pinned native runtime has process-global thread-pool and BF16 scratch/cache state, and its live transcription call blocks while processing an audio stream. Concurrent access must first be made safe: separate immutable model weights from per-call encoder/decoder/live-audio state; move mutable scratch/cache to per-call or safely scheduled storage; define a reentrant or stepwise inference API; and establish whether a shared thread pool or microbatcher can meet per-call deadlines. This could require a maintained patch/fork of the selected native runtime or another runtime with those guarantees. A single global inference mutex may allow sharing weights and warm reuse, but it serializes decode and does not by itself create concurrent compute capacity.

The C++ layer would then need a long-lived worker that routes interleaved PCM/control and transcript events by call ID, a versioned [worker IPC header](../engines/native/src/wire.hpp) with call identity and cancel/error semantics, a multi-session [native engine](../engines/native/src/native_engine.cpp), and a [session manager](../backend/sessions/src/session_manager.cpp) that tracks slots, per-call queues, deadlines, and failures independently. Admission must use measured model memory plus per-session state, not today's 3-GiB-per-process preflight rule. The browser WebSocket can remain one connection per call because multiplexing would occur behind the API boundary. Revalidate output parity, mixed-language simultaneous calls, callback isolation, cancel/crash behavior, memory growth, first/final latency, throughput, and CPU saturation before changing the sizing formula. Evidence for the current constraints: [runtime decision](decisions/0001-native-qwen-runtime.md), [worker implementation](../engines/native/src/worker_main.cpp), [M5 layout](m5-code.md).

A runtime switch is another experiment, and it need not change the **model**: current [llama.cpp multimodal documentation](https://github.com/ggml-org/llama.cpp/blob/master/docs/multimodal.md) lists a Qwen3-ASR-0.6B GGUF, while its [server documentation](https://github.com/ggml-org/llama.cpp/blob/master/tools/server/README.md) describes parallel slots and continuous batching. Those server features do not establish incremental PCM ingestion, pre-EOF partial transcripts, or CPU call capacity for Qwen3-ASR; they need a same-protocol test. [whisper.cpp](https://github.com/ggml-org/whisper.cpp) instead runs the **Whisper model family** and can use separate inference states, but its [stream example](https://github.com/ggml-org/whisper.cpp/blob/master/examples/stream/README.md) repeatedly transcribes audio windows. That is a different ASR behavior and must be benchmarked and labelled accordingly.

## 8. Deliverables, reproducibility, and remaining gaps

### Q33. Where are the requested code, instructions, report, sizing, architecture, and alternatives?

**Answer — delivered as prototype artifacts.** Runnable source is under [`frontend/`](../frontend/README.md), [`backend/`](../backend/README.md), [`engines/`](../engines/README.md), and [`benchmark/`](../benchmark/README.md). Setup/model acquisition is in the [README](../README.md) and [M11 reproduction guide](m11-reproduction.md). The [technical report](../reports/FINAL_TECHNICAL_REPORT.md) and [M10 JSON/HTML/PDF report](../results/reports/m10_evidence_20261005_v5/report.json) contain methodology, results, and the explicitly unknown sizing rows. Architecture is in [contracts](planning/ARCHITECTURE_AND_CONTRACTS.md) and [production proposal](m11-production-architecture.md); alternatives are in the [M9 comparison](m9-alternatives.md). The [browser demo bundle](../results/m11_three_language_demo_20261005_long/demo.json) contains transcripts, timing, hashes, and screenshots. Large model/data/raw evidence is ignored by Git and must accompany a handoff separately.

### Q34. Can the important results be reproduced, and what verification passed?

**Answer — same-host reproducibility demonstrated, independent-machine bootstrap unverified.** The [runbook](m11-reproduction.md) gives pinned acquisition, build, CTest, accuracy, load, report, and native browser-demo commands. The M9 paired experiment has a 335-file seal; the M10 report checks it and records source run IDs/hashes. Fresh out-of-tree builds on the same host passed 14/14 mock and 16/16 native CTest cases; frontend passed 4/4 unit tests and a production build; the native three-language browser test and fresh CLI smoke passed. These do not prove a first-time clean machine install or sustained production behavior. Evidence: [implementation status](IMPLEMENTATION_STATUS.md), [final report](../reports/FINAL_TECHNICAL_REPORT.md), [runbook](m11-reproduction.md).

The key native build, short load-screen, and demo entry points below run from the repository root after the pinned dependencies/model are acquired. Use fresh output directories; the exact historical run IDs, file hashes, and report-generation command are in the [M10 report instructions](../reports/README.md) and [M11 runbook](m11-reproduction.md).

```bash
cmake --preset release-cpu
cmake --build --preset release-cpu
ctest --preset release-cpu --output-on-failure

./build/release-cpu/asr-cli load-dry-run --config configs/qwen_native_single.yaml \
  --set audio.path=../tests/fixtures/m8_jfk_1p2s.wav \
  --set workers.processes=2 --calls 20 --concurrency 2 --languages en \
  --max-failure-rate 0 --max-p95-send-lag-ms 1000
./build/release-cpu/asr-cli load --config configs/qwen_native_single.yaml \
  --set audio.path=../tests/fixtures/m8_jfk_1p2s.wav \
  --set workers.processes=2 --set output.directory=../results/my_load \
  --calls 20 --concurrency 2 --languages en \
  --max-failure-rate 0 --max-p95-send-lag-ms 1000

./build/release-cpu/asr-cli serve --config configs/qwen_native_single.yaml --port 8765
# In a second terminal, from frontend/:
ASR_DEMO_EVIDENCE_DIR=../results/my_demo npm run test:e2e:native
```

### Q35. What is the final recommendation, and which assignment requirements remain open?

**Answer — continue as a gated CPU prototype; do not procure or promise production call volumes yet.** The live demo, C++ CPU integration, measurement path, test tools, and small multilingual accuracy screen are real. Production qualification remains open for an agreed SLO, real telephone/Common Voice or comparable approved speech, statistically useful held-out cohorts, local alternative-model benchmarks, sub-second first text, sustained mixed-language saturation/endurance, a validated headroom and node model, independent clean-machine bootstrap, and telephony/security/retention integration. The first decision gate is a representative target-hardware experiment with declared latency/accuracy/error limits; only then replace the sizing table's unknowns. Evidence: [final report](../reports/FINAL_TECHNICAL_REPORT.md), [implementation status](IMPLEMENTATION_STATUS.md), [M10 capacity finding](m10-code.md).
