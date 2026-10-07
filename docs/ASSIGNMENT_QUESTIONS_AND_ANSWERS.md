# Assignment questions and answers

**Canonical evidence:** [report.md](../report.md). This companion uses the same completed resumable measurements and does not define a competing final report.

**Measured scope:** twelve layouts; 1,170/1,170 direct calls; sixteen separate warmups; 55/60 WebSocket calls. Primary direct accuracy: English WER 10.24%, Indonesian WER 35.20%, Mandarin CER 12.43%. Ten unique WAVs per language are reused with balanced repetitions. Completed protocol status is not an accuracy threshold.

Implemented, measured, proposed and conditional statements are distinguished below. Baseline prefix values appear only as a labelled configuration comparison in the canonical report and the archive.

### Q1. Was a CPU-only, real-time, multilingual Qwen3-ASR proof of concept built?

Implemented. The primary runtime is Qwen3-ASR-0.6B on CPU through C inference kernels, C++20 service/worker integration and a React UI. EN/ID/ZH were measured using paced audio. The resumable runtime preserves independent call state while sharing model weights; production capacity is not inferred from completion alone.

### Q2. Does the user load a Linear PCM WAV, and does the system avoid one-shot offline transcription?

Implemented. Browser audio uses 16 kHz mono PCM16. Supported WAV formats can be prepared by the C++ reader. Audio becomes available progressively in 200 ms chunks; resumable recognition steps execute during arrival. The main experiment is not a one-shot upload followed by offline transcription.

### Q3. Are partial and final results distinguished in the UI?

Implemented. The UI distinguishes provisional partial snapshots and final output, with revisions and stream state. Older observer revisions cannot overwrite newer/final text. Stable word timing is not supplied by the runtime.

### Q4. Are start, stop, reset, and visible stream state available?

Implemented. Start, stop and reset manage explicit call identity, cancellation and independent buffers. The UI shows acknowledgements, transcript revisions and terminal state. REST suite jobs also support cancellation.

### Q5. What did the actual three-language browser demo show?

The [resumable browser demonstration](../results/resumable-ui-20261006/demo.json) completed EN/ID/ZH calls with two workers/two slots each, 100 ms browser chunks and 2-second decode steps. It displayed six to eight revisions per call and no browser errors. These focused UI examples are separate from the ten-file-per-language capacity study.

### Q6. Why 16 kHz mono PCM16, 200 ms chunks, and a 2-second decode step?

16 kHz mono PCM16 matches the model/media contract. A 200 ms chunk is 3,200 samples or 6,400 bytes, balancing delivery granularity and overhead. A 2-second decode step controls model readiness, not transport pacing. Four compute threads per worker, zero initial withheld chunks and no EOF whole-audio refinement define the measured preset; text latency is not guaranteed by chunk size.

### Q7. How are pacing, buffering, backpressure, and EOF handled?

Monotonic absolute readiness deadlines preserve the media clock. The measured delivery caps are eight chunks, 1,000 ms and 32,000 bytes; the effective time/byte cap holds about five full 200 ms chunks. Lateness above 5 ms is counted; delivery lag above 1,000 ms or queue overflow fails explicitly. EOF drains remaining resumable steps and produces final output; the measured preset does not perform whole-audio refinement.

### Q8. What is one call leg, and how are calls isolated and scheduled?

A call leg is one independently streamed source with its own ID, language, audio cursor, mutable caches/tokens, sequence watermarks and event lifecycle. Least-active admission spreads calls across healthy workers and maintains affinity. Each worker executes one oldest-ready quantum then requeues the call; at most one model step runs per worker at a time.

### Q9. How would real telephony replace the WAV simulator?

A proposed SIP/RTP/provider gateway would decode negotiated codecs, restore timestamped media timing through a bounded jitter buffer, normalize PCM and use the streaming API. Codec negotiation, telephony termination and network packet-loss concealment are not implemented in the WAV/browser POC.

### Q10. What happens with silence, VAD, end-of-utterance, long speech, interruptions, and jitter?

Explicit EOF, cancellation, ordered chunks, buffer/lag limits and session timeouts are implemented. General VAD, long-call segmentation, diarization and RTP jitter buffering are proposed. The shared input limit is 60 seconds; long conversations need bounded utterances, evaluated VAD pre-roll/hangover and overlap/deduplication. Six late chunks and zero audio overflows were recorded in measured summaries; loopback tests do not establish WAN-jitter tolerance.

### Q11. Which Qwen variant, precision, and runtime were chosen, and why?

The selected baseline is pinned Qwen3-ASR-0.6B with official BF16 weights, a pinned antirez/qwen-asr CPU C runtime and OpenBLAS. C++ wrappers own transport, scheduling and processes. Four compute threads per worker provide a reproducible budget. Model/runtime identity is recorded in third_party/revisions.lock; this selection is not a demonstrated optimum over model sizes or quantization.

### Q12. Was a C++ inference path feasible, and what are its limitations?

C/C++ inference is implemented. Generated native streaming state borrows immutable weights while owning each call’s mutable encoder/decoder buffers and token history. Completed encoder windows and unchanged decoder prefills are reused; growing partial windows can be re-encoded. Multi-call tensor batching, stable words and per-call word timestamps are not implemented.

### Q13. Were at least two sensible Qwen configurations compared?

The complete resumable study compares one worker at total concurrency 1–8 and two workers at total concurrency 2/4/8/16, with the same model/precision and four compute threads per worker. These twelve layouts produce thirty-six direct language points. No matched native-runtime, model-size, quantization or thread sweep is claimed.

### Q14. Are cold start and model load separated from steady recognition?

Cold dry-run plans, service startup/load metadata, loaded idle observations and warmups are separate from measured suites. The initial one-worker context-load metadata is 0.344 s; idle PSS is about 1.269–1.271 GiB for one worker and 2.413 GiB for two. Mapped weights and cached pages mean these values are not a controlled cold-cache startup distribution.

### Q15. What Qwen streaming or CPU limitations were found, and what workaround was used?

The original monolithic native streaming loop was adapted to explicit create/step/text/destroy state so calls can alternate without replaying complete prefixes. One ready quantum releases the worker after advancing call state. Growing-window recomputation, kernel-level blocking and bounded token work remain; cooperative cancellation checks do not forcibly interrupt a matrix kernel.

### Q16. What exactly do the latency and RTF metrics mean?

First text is stream start to the first nonempty partial or final; this does not prove semantic correctness. Final latency is stream start to final publication. EOF delay is worker EOF receipt to final publication. Streaming invocation RTF sums step wall time and optional refinement over input duration, excluding ready-queue wait; effective RTF includes pacing and queueing. Server and browser arrival timestamps are separate. Stable-word and vendor active-compute measurements remain unavailable.

### Q17. What hardware, OS, software, and baseline settings produced the numbers?

AMD Ryzen 9 9955HX, 16 physical cores/32 logical CPUs, approximately 14.78 GiB total RAM, Linux kernel 7.2.7-200.fc44.x86_64 and GCC 16.2.1 20260819. BF16 weights, four runtime/BLAS threads per worker, no CPU affinity, 200 ms chunks and 2-second decode steps. Report Section 2 lists software pins, environment evidence and hashed reproduction artifacts.

### Q18. What did the labelled accuracy evaluation show, and how was it scored?

Primary completed-only quality is **EN WER 10.24%, ID WER 35.20%, ZH CER 12.43%**. All direct calls complete, so direct failure-inclusive scores equal completed-only scores. Corpus edits/reference units are computed separately per language under NFC/casefold/punctuation policy M0. The common nine-recording English subset scores 5.98%; that is a labelled subset rather than the ten-recording primary score. See report Sections 5, 9.1 and Appendix D.

### Q19. What are the single/two-worker latency, throughput, RTF, and memory results?

The final direct study contains **1,170 offered / 1,170 completed calls**, with sixteen separate warmups. Network suites complete **55/60 calls**. One-worker/one-call first-text means are 2.678 s EN, 2.638 s ID and 2.546 s ZH. Peak measured process-tree PSS is 4.157 GiB. Full latency/resource/throughput tables, figures and per-WAV means are in report Sections 6–9 and Appendices A/D.

### Q20. Are CPU utilization, process CPU, cores, memory, queueing, errors, and dropped/late chunks observable?

Raw artifacts retain host/process CPU, RSS/PSS, pacing/submit/publication delays, call/worker state, queue waiting, errors and overflow counters. Process-tree CPU equivalents include all process threads and service/simulator overhead; four configured compute threads is not a hard CPU quota. Appendix A supplies resource percentiles. RSS can double-count mapped pages; PSS apportions shared physical memory.

### Q21. Does the system stay below real time under load, and was a saturation point found?

Configured worker compute budgets saturate while throughput reaches diminishing returns; the full 32-logical-CPU host ceiling was not tested. At two workers/sixteen calls, EOF p95 is 46.56 s EN, 73.22 s ID and 52.75 s ZH, despite completed finals. No latency pass/fail target was selected. The finite WAV study is not a continuous steady-state capacity guarantee; effective RTF and invocation RTF must be distinguished.

### Q22. Is there a repeatable multi-call load mechanism?

C++ load/serve prepares WAVs, paces independent calls and performs inference directly or through internal WebSocket clients. A manifest supplies distinct recordings. The Python matrix driver starts services and scores saved results; --resumable-matrix selects the twelve-layout grid with ten unique WAVs per language and three repetitions. At sixteen concurrent calls, complete cohorts repeat twice per repetition for equal file weighting.

### Q23. What CPU/vCPU, RAM, nodes, RTF, and p95 should be planned for 50, 100, 200, 500, and 1,000 concurrent legs?

The canonical conditional sizing is report Section 10: W=max(ceil(N/(k*u)),ceil(N*d/(q*e*u))) and H=ceil(W/2). With k=8, u=.70, q=1.0, e=.80 and d=1, the 50/100/200/500/1000 scenarios give 45/90/179/447/893 nodes, each provisionally 16 comparable logical CPUs and 16 GiB RAM. q is a finite-test goodput proxy; fleet capacity and p95 are not validated or predicted.

### Q24. How should a valid sizing model account for shared memory, batching, contention, queues, NUMA, headroom, and diminishing returns?

Session admission and processing demand are independent constraints. Weights share physical pages within a host; mutable state grows with workers/calls and weights replicate across nodes. The sizing model credits no batching gain, reserves 30% headroom, includes an unmeasured efficiency allowance and limits worker density to two/node. Silence demand remains d=1 until VAD actually avoids inference and its goodput is remeasured. NUMA, CPU equivalence, bursts and long calls need target-hardware validation.

### Q25. What happens when capacity is exceeded, and how would horizontal scaling work?

Full slots reject admission, and queue/lag/deadline failures are explicit. Preflight and live resource guards bound unsafe experiments. Proposed horizontal scaling uses admission by healthy capacity and queue age, call affinity, warm readiness, graceful draining and supervised worker recovery. Live decoder state is not automatically migrated across workers/nodes.

### Q26. What is the implemented POC architecture, and what is proposed for production?

Implemented: React dashboard, C++ API/transport and paced WAV simulator, persistent model workers, per-call resumable state, routing/scheduling and resource/timing artifacts. Proposed: telephony ingress, validated VAD/utterance segmentation, supervised recovery, authenticated multi-node routing and queue-age scaling. Report Sections 3/11 contain both diagrams and distinguish POC from production design.

### Q27. Why WebSocket, and what are the API/result-delivery contracts?

WebSocket supports browser-compatible bidirectional PCM, acknowledgements, cancellation and result revisions. REST handles configuration, plans, suite jobs and status. Results carry call identity and event sequencing. At two workers/sixteen sessions, five network calls are admission failures and lack the expected terminal failure event; these outcomes remain distinct from direct inference and transcript quality.

### Q28. How are worker lifecycle, health, draining, failure recovery, and session affinity designed?

Workers retain session affinity and expose process, health, active-session and ready-queue counts. One worker is in-process; two workers use persistent helper processes. Calls terminate on EOF/cancel/timeout/failure; draining rejects new admission. Automatic worker respawn, durable replay and cross-node state transfer are not implemented. A worker failure affects its assigned calls.

### Q29. What production security and privacy controls are required?

Proposed controls include TLS, authenticated tenant authorization, bounded payloads, restricted media/artifact access, encrypted storage, configurable retention, redacted logs and audit records. Local path/size limits and loopback operation are not a production security/compliance certification.

### Q30. What observability and operational signals are available or needed?

Available signals include call outcomes, revisions/timelines, chunk delivery and model-ready queue delays, worker occupancy/thread budgets, host/process CPU and memory, and errors. Production needs centralized traces, per-language quality/failure dashboards, queue-age/utilization alerts, lifecycle/restart telemetry and retention controls. Missing vendor metrics are not represented as zero.

### Q31. Is Qwen3-ASR automatically the best production choice? Which alternatives were compared?

Qwen is not declared the production winner. Report Section 12 compares multilingual Whisper via whisper.cpp with streaming Zipformer via sherpa-onnx. The named bilingual Zipformer covers EN/ZH, requiring a separate ID route. Neither alternative was benchmarked locally; quantization, domain vocabulary, timestamps, model licensing and deployment maintenance need checkpoint-specific assessment.

### Q32. What would change for strict sub-second partials at hundreds of calls?

The measured 2-second audio step has an availability floor above a strict sub-second target; smaller transport chunks alone cannot solve it. A suitable incremental recognizer, short bounded compute turns, quality-tested quantization/VAD and bounded batching waits need evaluation on production-shaped multilingual audio and target nodes.

### Q32a. What code and design changes are required to share a loaded Qwen model across calls?

Independent resumable caches are implemented through the generated C state API, existing shared engine/pool/IPC, persistent worker helper, qwen_stream preset, factory/config wiring, streaming metrics and UI decode-step control. Per-call mutable state is isolated while weights are shared. Steps alternate serially within a worker; simultaneous tensor batching is a separate future change.

### Q33. Where are the requested code, instructions, report, sizing, architecture, and alternatives?

[report.md](../report.md) is the sole canonical final report, including results, sizing, architecture, alternatives, per-WAV tables and UI evidence. README, CLI_QUICKSTART, ARCHITECTURE, TESTING and RESUMABLE_STREAMING are implementation/run companions. Superseded narratives and prefix coefficients are explicitly retained under [archive](archive/README.md).

### Q34. Can the important results be reproduced, and what verification passed?

The [reproduction package](../results/resumable-matrix/reproducibility/README.md) preserves source available at report finalization, evaluated executables, generated native sources and file-level hashes, alongside pinned model/input/config identities. Development checks include nineteen C++ CTest checks plus frontend and Python validation. No complete all-in-one full regression or controlled clean-machine startup is established. Source capture at finalization is distinguished from evaluated binary identity.

### Q35. What is the final recommendation, and which assignment requirements remain open?

The resumable C/C++ runtime is the measured multi-call prototype. Lower occupancy gives faster first/final text; extra slots increase queued work rather than compute capacity. Production selection requires stronger language/domain quality evidence, robust network admission/failure delivery, sustained/long-call testing and evaluated telephony/VAD/security/recovery. These are explicit validation requirements, not claimed completed features.
