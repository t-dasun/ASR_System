# Assignment questions and current implementation answers

**Latest evidence:** [Resumable final report](FINAL_REPORT_RESUMABLE.md) supersedes the older prefix measurements and sizing in this historical Q&A. It includes the current implementation decisions and assignment coverage.

A subsequent opt-in [resumable streaming implementation](RESUMABLE_STREAMING.md) now supplies repeated per-call updates over shared weights. Legacy prefix/capacity answers below still describe the original measured mode; new pilot measurements are separate.

This guide retains the assignment's question set and requested design decisions, updated for `dev-clean`. **Implemented** means code exists; **proposed** means additional production work; numerical capacity/quality claims require saved measurements. Historical result bundles were intentionally cleared during cleanup. Current evidence is summarized in the [final technical report](FINAL_REPORT.md); the saved capacity study is partial, with failures retained. New experiments write to `results/` and are documented in [Testing](TESTING.md).

The current model and WAV simulation paths are C++. The main runtime choices are native one-call workers and shared-prefix workers with multiple active sessions. See [Architecture](ARCHITECTURE.md) and [Run commands](CLI_QUICKSTART.md).

### Q1. Was a CPU-only, real-time, multilingual Qwen3-ASR proof of concept built?

**Implemented.** The main executable and inference path are C++20/C CPU code. WAV chunks can be paced through direct calls or WebSockets, with English, Indonesian and Mandarin language selection. The native runtime produces progressive revisions; the shared-prefix runtime produces one prefix preview and an EOF refinement. This is a working prototype, not a demonstrated production capacity guarantee.

### Q2. Does the user load a Linear PCM WAV, and does the system avoid one-shot offline transcription?

**Implemented, with different decoder semantics.** Browser WAV upload requires 16 kHz mono PCM16. The C++ reader can prepare other supported WAV formats before pacing. Media is released in chunks rather than submitted at time zero. Native streaming decodes progressively; shared-prefix inference performs a prefix pass while audio arrives and a whole-audio pass after EOF. The latter uses offline invocations inside a streaming transport and should be described that way.

### Q3. Are partial and final results distinguished in the UI?

**Implemented.** Events distinguish provisional partial snapshots from final text. The dashboard shows revisions and terminal status. Partial text can change; stable-prefix/word alignment is not available from this runtime.

### Q4. Are start, stop, reset, and visible stream state available?

**Implemented.** The browser has start, stop and reset, plus call state, acknowledgements and transcript events. Cancellation terminates the call; a new call has a new identity and independent buffers. Suite jobs can also be stopped through REST.

### Q5. What did the actual three-language browser demo show?

**Current UI evidence is saved** in [ui-controls-20261006/demo.json](../results/ui-controls-20261006/demo.json) and its screenshots. Three main C++ shared-prefix calls completed with two workers, 100 ms chunks and a selected 2 s preview, showing first-text/EOF arrival delays and completed-call WER/CER. Browser errors were zero; scores matched the Python evaluator. This short-input controls demo deliberately avoids the three known timeout clips and is not an accuracy/capacity study; the full curves still retain those failures.

### Q6. Why 16 kHz mono PCM16, 200 ms chunks, and a 2-second decode step?

**Design decisions.** 16 kHz mono signed PCM16 matches the runtime input contract. A 200 ms chunk contains 3,200 samples (6,400 PCM bytes), balancing transport overhead and pacing granularity. Native `decode_step_ms=2000` is independent of the transport chunk size. Shared `prefix_preview_ms=4000` produces a single preview and limits repeated prefix recomputation. Neither chunk interval implies a matching text-latency guarantee.

### Q7. How are pacing, buffering, backpressure, and EOF handled?

**Implemented.** C++ uses monotonic absolute sample-availability deadlines, bounded delivery queues and explicit lag/overflow failure. WebSocket acknowledgements provide flow control; sequence and sample counts validate EOF. An overload is surfaced rather than silently dropping media. EOF schedules final refinement; cancellation/timeouts are separate terminal outcomes.

### Q8. What is one call leg, and how are calls isolated and scheduled?

**Implemented.** A leg is one input stream with its own call ID, language, PCM buffer, sequence/sample watermark and event lifecycle. Native workers hold one active call/context. Shared workers hold several independent call sessions while serializing their decode jobs through one persistent context. New calls use least-active routing by default and retain worker affinity.

### Q9. How would real telephony replace the WAV simulator?

**Proposed.** A SIP/RTP or platform media gateway would decode negotiated codecs, restore media timing with a jitter buffer, normalize to 16 kHz mono PCM16 and submit the existing streaming contract. The current source is WAV/browser PCM; it does not implement telephony termination or codec negotiation.

### Q10. What happens with silence, VAD, end-of-utterance, long speech, interruptions, and jitter?

**Partially implemented.** There are buffer/lag limits, ordered chunks, explicit EOF, cancellation and session timeouts. General VAD, utterance segmentation, diarization and telephony jitter handling are absent. Shared-prefix calls accept at most 60 seconds of PCM; long conversations need bounded utterance segments and an evaluated VAD policy. Silence and interruptions require language-specific accuracy tests rather than an assumption that nonempty output is correct.

### Q11. Which Qwen variant, precision, and runtime were chosen, and why?

**Chosen baseline:** official pinned Qwen3-ASR-0.6B, BF16 CPU weights, pinned `antirez/qwen-asr` C runtime and OpenBLAS. Four compute threads per worker are the default. The smaller model and C integration meet the CPU/C++ prototype requirement. Exact revisions and weight hash remain in `third_party/revisions.lock`. This is a practical baseline, not a claim that this runtime/model is optimal.

### Q12. Was a C++ inference path feasible, and what are its limitations?

**Feasible and implemented.** Native C inference is wrapped by C++ engines and process helpers. The native context cannot be used concurrently without owning/isolating mutable state. The shared-prefix wrapper serializes independent offline prefix/final jobs, sharing weights without independent per-call streaming caches. It does not add batching, parallel decoder slots, stable-word alignment or GPU execution.

### Q13. Were at least two sensible Qwen configurations compared?

**Measured process/scheduling configurations:** one shared worker at total concurrency 1/2/4/8 and two workers at 1/2/4/8/16, on the same ten distinct WAVs per language. Results are in [FINAL_REPORT.md](FINAL_REPORT.md). Native/shared presets remain, but no current matched native/shared or thread-count performance comparison is claimed. A controlled comparison would strengthen runtime selection.

### Q14. Are cold start and model load separated from steady recognition?

**Implemented measurement separation.** Session startup, model load and shared model load are distinct fields. Persistent shared services load contexts before admitting suites; shared model-load metadata is not a per-call cost. Cold CLI plans and service startup logs are saved by the experiment driver. Keep cold-start data separate from warm-call percentiles.

### Q15. What Qwen streaming or CPU limitations were found, and what workaround was used?

**Runtime limitation and workaround.** The native streaming invocation owns mutable context/state and is not an independently resumable per-call cache scheduler. Shared-prefix workers buffer calls independently and serialize prefix/EOF invocations through one loaded context. This enables multiple active calls per worker, with lower model duplication, but changes streaming semantics and introduces queueing. Token-boundary cancellation/decode deadlines are generated in build sources; they cannot preempt a kernel.

### Q16. What exactly do the latency and RTF metrics mean?

**Definitions are in [Testing](TESTING.md).** First usable text is stream start to first nonempty partial or final. First partial excludes final-only calls. EOF delay is worker EOF receipt to final publication (controller request fallback), not first-to-last-text time. Effective RTF includes paced waiting and finalization; offline invocation wall RTF excludes pacing/ready-job wait. Internal active-compute RTF and stable-word timing are unavailable and remain null.

### Q17. What hardware, OS, software, and baseline settings produced the numbers?

**Current measured environment:** Ryzen 9 9955HX, 16 physical/32 logical CPUs, approximately 14.78 GiB total RAM, Linux CPU inference, BF16 weights and four compute threads per worker. The [final report](FINAL_REPORT.md) records kernel/compiler, inspected software versions and source/binary/model/input identities. Available RAM is not total RAM, and git HEAD alone does not identify the uncommitted source; use the saved source snapshot.

### Q18. What did the labelled accuracy evaluation show, and how was it scored?

**Primary accuracy uses completed calls only**, with failures reported separately. One worker/concurrency one: EN WER 6.52% (27/30 completed), ID WER 4.93% (24/30), ZH CER 3.25% (30/30). Supplementary failure-inclusive scores are 16.10%, 24.58%, 3.25%. Scores aggregate edits/reference units per language, not per-file percentage means. NFC, EN/ID casefold, punctuation/whitespace policy and edit alignments are retained. See the [final report](FINAL_REPORT.md); this ten-file validation cohort is not a production accuracy guarantee.

### Q19. What are the single/two-worker latency, throughput, RTF, and memory results?

**Current results are retained.** The [final report](FINAL_REPORT.md) includes all direct curves, per-language baseline/tail/resource tables, network failures and plots. One worker/concurrency one is the shared-runtime control. The measured study has 900 direct offered calls, 810 completions and 90 failures. No current native performance table is fabricated.

### Q20. Are CPU utilization, process CPU, cores, memory, queueing, errors, and dropped/late chunks observable?

**Implemented, with labelled limits.** Artifacts expose pacing lag, controller queues, submit/publication delays, runtime stages, process/host CPU and memory, worker/call status, errors and overflow counters. Shared workers expose active sessions and ready decode queues. Samples are periodic observations; summed process RSS is not unique physical memory. Vendor internal active-compute boundaries, alignment and stable text are unavailable.

### Q21. Does the system stay below real time under load, and was a saturation point found?

**Curves are measured; production saturation is not established.** Up to two workers/16 target concurrent calls were tested. Queueing and finalization tails worsened with occupancy. The study ended after an unhealthy worker in the network test, before workers 3–8 or a sustained run. No latency acceptance target was selected, so results describe the curve rather than maximum usable calls. Effective RTF includes pacing; invocation RTF is a different measure. See [failure/saturation analysis](FINAL_REPORT.md).

### Q22. Is there a repeatable multi-call load mechanism?

**Implemented in C++.** `load` prepares WAVs, generates call plans and paces PCM for bounded concurrent sessions. `--manifest` allows distinct recordings; absent a manifest, calls repeat one configured WAV. Direct mode calls the engine; network mode uses C++ WebSocket clients and an internal server. `serve` runs the same simulator through REST jobs. Python starts services and scores outputs, rather than running model inference.

### Q23. What CPU/vCPU, RAM, nodes, RTF, and p95 should be planned for 50, 100, 200, 500, and 1,000 concurrent legs?

**Conditional estimates are documented** in [CAPACITY_SIZING.md](CAPACITY_SIZING.md) and the [final report](FINAL_REPORT.md). Required workers are the maximum of admission demand and processing demand: `max(ceil(N/(k*u)), ceil(N*d/(q*e*u)))`; nodes are `ceil(W/workers_per_node)`. The guide includes all five requested call counts, shared memory, CPU/RAM/headroom assumptions and a hypothetical VAD sensitivity case. Finite-test goodput is only a provisional proxy; sustained RTF and p95 remain unvalidated. No production fleet count or market price is asserted.

### Q24. How should a valid sizing model account for shared memory, batching, contention, queues, NUMA, headroom, and diminishing returns?

**Use the separate slot and processing constraints in [CAPACITY_SIZING.md](CAPACITY_SIZING.md).** Memory sharing is local to a node; private contexts/buffers grow with workers/sessions, and weights are replicated across hosts. Four threads/worker are configured, not dedicated cores. There is no measured batching benefit. The guide reserves 30% headroom, states an unmeasured contention allowance, and uses two workers/node to avoid dense-host extrapolation. Current code submits silence, so submitted-audio demand cannot be reduced without a validated VAD/segmentation pipeline. NUMA, burst queues and p95 require target-hardware testing.

### Q25. What happens when capacity is exceeded, and how would horizontal scaling work?

**Implemented locally; horizontal scaling is proposed.** Full session slots reject admission; timeouts/queue failures surface terminal errors. Preflight can reject unsafe run plans before inference. Production needs a gateway with bounded admission, session affinity, health routing, drain-before-restart and multiple service nodes. A shared worker context is not automatically migrated after failure.

### Q26. What is the implemented POC architecture, and what is proposed for production?

**Implemented:** C++ WAV preparation/pacing, interchangeable engines, worker/session scheduling, WebSocket/REST transport, artifacts/metrics and a React dashboard. **Proposed production:** telephony ingress, VAD/segmentation, durable job/session metadata, authenticated routing, supervision/recovery, distributed affinity and SLO-based scaling. The current source map and flow are in [Architecture](ARCHITECTURE.md).

### Q27. Why WebSocket, and what are the API/result-delivery contracts?

**Design decision.** WebSocket provides bidirectional chunk acknowledgements, cancellation and revisions over one connection and integrates with a browser. `/v1/asr` carries live PCM and result events; `/v1/observe` carries observation data. REST plans/submits suites, exposes jobs/runtime/history and serves allowlisted artifacts. Text snapshots are provisional until final, with explicit call identity and sequencing. The obsolete report registry API was removed during cleanup.

### Q28. How are worker lifecycle, health, draining, failure recovery, and session affinity designed?

**Implemented lifecycle; limited recovery.** Calls are admitted onto fixed worker slots, keep affinity, and terminate explicitly on EOF, cancel, timeout or failure. Runtime status exposes health/process/session counts. Draining rejects new work. Native contexts are call-isolated children; shared contexts persist per worker. Shared worker failures affect assigned calls; automatic respawn/replay, durable resume and cross-node migration are not implemented.

### Q29. What production security and privacy controls are required?

**Proposed production controls.** Deploy behind authenticated TLS, tenant-aware admission/authorization and resource limits; protect transcript/audio storage with retention and access policies; audit artifact access; redact sensitive logs; manage model/dependency provenance and licenses. The loopback prototype and input/path bounds are not a production security boundary. No production security claim is made.

### Q30. What observability and operational signals are available or needed?

**Current signals:** call outcomes, event/runtime timelines, pacing/queue/submit/publication delays, per-worker session/queue/thread status, host/process CPU and memory, and persisted errors. **Needed operationally:** centralized metrics/traces, per-language SLO dashboards, queue age and saturation alerts, model/version rollout tracking, supervised worker restarts and retention controls. Null vendor measurements remain explicit.

### Q31. Is Qwen3-ASR automatically the best production choice? Which alternatives were compared?

**Qwen is not declared the production winner.** The [final report](FINAL_REPORT.md) provides a sourced design comparison of multilingual Whisper base/small through whisper.cpp and streaming bilingual Zipformer through sherpa-onnx. The named Zipformer covers EN/ZH, so ID needs a separate recognizer. Neither alternative was benchmarked locally. Current llama.cpp documentation lists Qwen3-ASR GGUF support; it is a candidate runtime experiment, not another ASR model family or demonstrated multiplexing gain.

### Q32. What would change for strict sub-second partials at hundreds of calls?

**Additional runtime work is required.** The current shared 4-second preview cannot meet strict sub-second first-text requirements. A runtime with independently resumable per-call streaming state, efficient incremental decode/batching, bounded queues and suitable hardware is needed; lowering the transport chunk interval alone is insufficient. Validate the resulting model/runtime on representative multilingual telephony audio before fleet sizing.

### Q32a. What code and design changes are required to share a loaded Qwen model across calls?

**Already implemented for prefix/EOF sharing.** New components are `src/engines/prefix/` (shared context, scheduling and process pool), `apps/asr_prefix_worker/` (persistent worker entry), runtime selection/service wiring in `apps/asr_cli/`, strict config/manifest support, pool status in the UI, and matrix/service checks in `tools/testing/`. Per-call buffers/state are isolated; model invocation is serialized; admission, cancellation, deadlines and bounded preview/EOF priority are explicit. Resumable native per-call caches are now implemented in opt-in `qwen_stream`; simultaneous batching remains future work. See [implementation and pilot checks](RESUMABLE_STREAMING.md).

### Q33. Where are the requested code, instructions, report, sizing, architecture, and alternatives?

**Current deliverables:** [final technical report](FINAL_REPORT.md), [sizing guide](CAPACITY_SIZING.md), [run guide](CLI_QUICKSTART.md), [architecture](ARCHITECTURE.md), [testing](TESTING.md) and this Q&A. The report embeds current plots and references saved raw artifacts, pins and source hashes. Current demo evidence, aggregate regression gaps and reliability failures are explicitly listed; documentation does not mark those activities complete.

### Q34. Can the important results be reproduced, and what verification passed?

**Reproduction paths are retained.** The clean build has 19 C++ contract tests, 17 Python checks (JiWER parity optional for manual runs), four frontend unit tests and browser checks. Pinned inputs/model identity remain. `scripts/regression.py --full` combines the maintained checks and real-model workflows with per-step logs and aggregate status; see [Testing](TESTING.md). Contract tests are distinct from model-quality evidence, and old result numbers are not carried forward as new measurements.

### Q35. What is the final recommendation, and which assignment requirements remain open?

**Recommendation:** use the shared-prefix pool as the current multi-call prototype, with explicit preview/final semantics and measured safe slot counts. Keep the native preset for progressive-streaming comparisons. Re-run per-language experiments for any performance report. Open assignment conclusions include production saturation/fleet sizing, strict sub-second text, long telephony conversations, production VAD/gateway/security/recovery and a controlled alternative-model comparison. The source is runnable; these deployment conclusions require additional evidence.
