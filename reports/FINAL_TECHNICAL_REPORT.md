# CPU streaming ASR: final technical report

**Evaluation date:** 2026-10-05  
**System:** C++20 CPU service and native Qwen3-ASR-0.6B worker; local browser dashboard  
**Result:** functioning three-language prototype; production latency/capacity **not qualified**

## 1. Executive decision

The prototype accepts paced 16 kHz mono PCM, emits provisional transcript revisions over WebSocket, performs final refinement at EOF, and records per-call timing/resource evidence. A real browser-to-C++-service-to-native-worker demo completed in English, Indonesian, and Mandarin. The model is usable for controlled local evaluation, but the evidence does **not** support a sub-second first-text claim, production call-volume sizing, or deployment on a public network. The correct production decision is a gated continuation, not a 50–1,000-leg procurement estimate.

## 2. Scope and requirement disposition

| Assignment area | Delivered evidence | Boundary |
|---|---|---|
| CPU-only multilingual ASR | Pinned Qwen3-ASR-0.6B native runtime; EN/ID/ZH FLEURS validation | Small validation cohorts, not representative traffic |
| Progressive streaming | Paced WAV/PCM, revisions, WebSocket flow control, EOF refinement | Transport chunk length is not decode latency; first text is seconds |
| C++ backend and UI | Modular engine/session, worker manager, REST/WebSocket service, browser dashboard | Loopback prototype only; no phone ingress/auth/TLS |
| Reliability and load | Direct/network harness, staggered calls, sweeps, system telemetry | Short diagnostic screens only; no saturation knee/endurance qualification |
| Accuracy and comparison | WER for EN/ID, CER for ZH, paired clean/simulated-telephone run; two researched alternatives | No real telephone/Common Voice cohort or locally measured alternative adapter |
| Sizing and operations | Explicit unknowns for 50/100/200/500/1,000 legs; proposed production architecture | No defensible node specification/count yet |
| Reproduction | Pinned vendors/model/dataset; tests; runbook; version-constrained Python env; browser evidence | Fresh builds verified on same host, not an independent clean machine |

## 3. Architecture and modularity

The present path is `WAV/browser or CLI -> C++ PCM/chunk adapter -> session/worker scheduler -> isolated native Qwen process -> transcript/metrics/artifact store -> WebSocket or CLI consumer`. C++ interfaces separate audio delivery, engine lifecycle, session routing, configuration, measurement, storage, and transport, so the mock engine and native Qwen adapter can be tested through the same contract. The model process owns one inference slot; the scheduler admits a bounded number of sessions and isolates worker failure. The browser exercises the public local protocol, while Python is confined to dataset preparation, official reference validation, accuracy auditing, and report generation. It is not the measured inference service. See [architecture/contracts](../docs/planning/ARCHITECTURE_AND_CONTRACTS.md), [native adapter](../docs/m3-code.md), [session manager](../docs/m5-code.md), [protocol](../backend/transport/README.md), and [production proposal](../docs/m11-production-architecture.md).

The production proposal adds a separate telephony/media gateway, authenticated TLS/WSS ingress, sticky per-call admission, bounded queues, durable idempotent transcript delivery, explicit overload, security/privacy controls, and fleet observability. None of these proposed components is presented as deployed functionality.

## 4. Hardware, software, and evidence provenance

The measured host is a Ryzen 9 9955HX laptop (16 physical cores/32 logical CPUs), approximately 14.8 GiB RAM, Linux Fedora kernel 7.2.7, GCC 16.2.1, and CMake 4.3.0. The preflight snapshot recorded a `powersave` governor; frequency/thermal variability is not controlled. See [host capture](../results/m11-host.json). The native build uses host CPU instructions and must be rebuilt/retested for a different CPU. The pinned model revision is `5eb144179a02acc5e5ba31e748d22b0cf3e303b0`; the 1.88 GB weight file SHA-256 is `79d6cbd4c98c7bbffe9db2edac07f56cd6637d0d5944b27f6c2b8353840323ea`. Vendor/data revisions are in [`third_party/revisions.lock`](../third_party/revisions.lock), and the principal YAML SHA-256 is `33cc18e7831e2ef1407dfaa114ea291483aa6003b29d9a0a71665f9d803c7594`.

Results are local evidence, generally ignored by Git: [M10 machine-readable report](../results/reports/m10_evidence_20261005_v5/report.json), [HTML](../results/reports/m10_evidence_20261005_v5/report.html), [PDF](../output/pdf/m10-evidence-and-sizing.pdf), and [M11 browser evidence](../results/m11_three_language_demo_20261005_long/demo.json). Preserve these directories when handing off the project; a source-only Git clone will not contain all raw data, model weights, or screenshots. The M10 report records source run IDs and SHA-256s, but not every underlying raw file has a checksum seal; the report labels those distinctions.

## 5. Measurement method and metric semantics

Audio is delivered on its scheduled chunk deadlines rather than handed to inference as a whole file at time zero. First usable text is measured from the paced stream start; final result is measured separately. Client first-text values use the browser clock and are not subtracted from server monotonic timestamps. A 200 ms transport chunk is a network/audio framing parameter, **not** a 200 ms inference guarantee. WER is used for English/Indonesian and CER for Mandarin; the denominators and edit counts are in the raw accuracy files. CPU/RSS measurements are sampled, so short peaks can be missed. Throughput below is total audio seconds processed divided by wall seconds for the repeated fixture; it is not a sustainable concurrent-call rating. Effective RTF includes the paced-call path and is not active decoder compute RTF, because active-decode boundaries are unavailable from the native API. See [measurement guide](../docs/m4-code.md), [load guide](../docs/m6-code.md), and [M10 methodology](../docs/m10-code.md).

## 6. Three-language live browser demonstration

The [native end-to-end evidence bundle](../results/m11_three_language_demo_20261005_long/demo.json) ran Chrome through the local C++ WebSocket service and real Qwen worker, with the dashboard preview. Each call reached `completed`, acknowledged all chunks, emitted multiple revisions and a non-empty final transcript; browser errors were zero. The test asserted that first text appeared **before audio EOF** in every language. The bundle includes source/config/binary/model hashes and [EN](../results/m11_three_language_demo_20261005_long/en-stream-complete.png), [ID](../results/m11_three_language_demo_20261005_long/id-stream-complete.png), and [ZH](../results/m11_three_language_demo_20261005_long/zh-stream-complete.png) screenshots.

| Language | Clip duration | Chunks ACKed | Revisions | Browser first text | Final transcript |
|---|---:|---:|---:|---:|---|
| EN | 15.36 s | 77/77 | 31 | 7.206 s | “The stretch between Point Marion and Fairmont presents the most challenging driving conditions on the Buffalo-Pittsburgh highway, passing frequently through isolated backwoods terrain.” |
| ID | 15.84 s | 80/80 | 34 | 7.203 s | “Teknologi menawarkan solusi dengan karier wisata virtual. Siswa dapat melihat artefak museum, mengunjungi akuarium, dan mengagumi seni yang indah dari duduk di kelas mereka.” |
| ZH | 15.58 s | 78/78 | 28 | 7.201 s | “滑雪是许多滑雪爱好者的主要旅游活动。这些爱好者有时也被称为滑雪狂人，他们计划整个假期都在某个地点滑雪。” |

These are three individual illustrative calls with a warm local service, not p50/p95 statistics. The UI streams the WAV without playing its sound through speakers.

## 7. Accuracy and channel robustness

The paired screen used five distinct FLEURS **validation** clips per language, each tested clean and after a simulated telephone μ-law transform: 30 calls total. It did not use the official test split, a real phone channel, or Common Voice (approved Mozilla Data Collective archives were unavailable). Consequently, the numbers are diagnostic, not population error estimates. Source: [M10 report holdout section](../results/reports/m10_evidence_20261005_v5/report.json) and [M9 evidence matrix](../docs/m9-evidence-matrix.md).

| Language | Metric | Clean | Simulated telephone | Clean first-text p50 | Telephone first-text p50 |
|---|---|---:|---:|---:|---:|
| EN | WER | 5.8% | 12.6% | 7.20 s | 7.00 s |
| ID | WER | 5.7% | 14.8% | 9.61 s | 9.24 s |
| ZH | CER | 5.6% | 2.5% | 7.20 s | 7.20 s |

The Mandarin telephone reduction is an observed result on five clips, not evidence that telephone audio improves accuracy generally. All 30 diagnostic calls completed. The combined first-usable p50/p95 were 7.20/10.20 s. Strict sub-second first text was **not demonstrated**.

## 8. Concurrency, memory, and bottlenecks

The bounded native screen repeated one 1.2-second English WAV for 20 calls at each point. One and two workers completed 20/20 each under a diagnostic failure/send-lag gate. Two-worker throughput improved but memory roughly doubled and first-text tail grew. These points are less than one minute each and cannot show steady thermal behavior or a saturation knee. A three-worker run was rejected by conservative RAM preflight. Source: [M10 capacity screen](../results/reports/m10_evidence_20261005_v5/report.json).

| Concurrent workers | Completed | Audio s / wall s | First-text p95 | Final p95 | Sampled peak process-tree RSS |
|---:|---:|---:|---:|---:|---:|
| 1 | 20/20 | 0.467 | 1.686 s | 2.293 s | 2.59 GiB |
| 2 | 20/20 | 0.770 | 2.041 s | 2.936 s | 5.17 GiB |

The throughput ratio was 1.65×, not linear 2×. The memory cost of isolated model processes and seconds-scale decode cadence are the central measured constraints. A separate three-call staggered-arrival screen with two workers completed 3/3; it is a functionality/recovery screen, not sustained capacity. A 93.38-second synthetic replay/silence/interrupt stress case had 7.00-second first usable text and 111.54-second final result, with no human reference or WER/CER. The four-vs-two-thread sequential tuning screen did not reveal an accuracy advantage for two threads, but run order, cache, and thermals were uncontrolled; neither setting is a proven optimum. The dominant uncertainty is real mixed-language concurrent demand at a declared SLO, not whether the current harness can generate calls.

## 9. Capacity and hardware sizing: measured versus unknown

No safe calls-per-node figure was established. Required inputs are an agreed first/final latency and error SLO, representative call duration/language/channel mix, sustained runs through a failure/latency knee, reserve/headroom, and representative server hardware. The present laptop's one/two-worker diagnostic screen cannot be linearly scaled. “vCPU” would mean a logical hardware thread, but its throughput is CPU-model-specific. The [M10 sizing output](../results/reports/m10_evidence_20261005_v5/report.json) deliberately labels all requested tiers as follows:

| Simultaneous legs | 50 | 100 | 200 | 500 | 1,000 |
|---|---|---|---|---|---|
| Required node count/spec | Unknown | Unknown | Unknown | Unknown | Unknown |
| Classification | Insufficient evidence | Insufficient evidence | Insufficient evidence | Insufficient evidence | Insufficient evidence |

This is a negative but traceable capacity result, not a claim that the workload is impossible. The implementation can be reassessed after the admission envelope is measured.

## 10. Alternative ASR paths

Two CPU-capable research candidates were reviewed in [the M9 alternative matrix](../docs/m9-alternatives.md): multilingual Whisper `small` through whisper.cpp, and omniASR-CTC-300M INT8 through sherpa-onnx. Both offer a plausible C/C++ integration path and documented EN/ID/ZH language support. Neither adapter is implemented or measured here. Whisper's repeated-window preview and sherpa's offline/segmented recognizer must **not** be called equivalent to persistent causal streaming without a common partial-text contract. Model size/RSS statements in the research matrix are upstream estimates, not laptop benchmarks. The next fair comparison must pin artifacts, use the same manifests and paced protocol, record pre-EOF behavior, and compare WER/CER, latency, RSS, CPU, and multi-call load under the same SLO.

## 11. Production and on-premise design

The [production architecture proposal](../docs/m11-production-architecture.md) specifies gateway codec normalization; authenticated, rate-limited ingress; sticky call ownership; bounded admission and explicit overload; drain/crash semantics; event idempotency; telemetry; and separate access/retention controls for audio versus transcripts. Localhost binding and Origin checks in the current prototype are **not** authentication. The repository does not include SIP/RTP termination, TLS, tenant authorization, durable storage, rolling upgrades, or production compliance. On-premise deployment requires mirrored and verified third-party assets, target-CPU rebuild, offline-start tests, a threat/privacy review, and a measured capacity gate. Model and data licenses are listed in [`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md).

## 12. Reproduction and verification

Follow the [M11 reproduction runbook](../docs/m11-reproduction.md) from repository root. The lock file pins vendor/model/FLEURS revisions; [`tools/reference/constraints-linux-py312.txt`](../tools/reference/constraints-linux-py312.txt) captures 97 resolved Python distribution versions (not wheel hashes), and `frontend/package-lock.json` pins the dashboard dependency graph. The reference environment passed `uv pip check` (98 installed packages compatible). On 2026-10-05 fresh **out-of-tree build directories on the same host** passed 14/14 mock and 16/16 native CTest cases with localhost permission. The native browser end-to-end test passed EN/ID/ZH; a fresh native CLI smoke call completed with zero overflow/late chunks. Frontend unit tests passed 4/4 and its production build succeeded. These facts do not constitute an independent clean-machine bootstrap; system OpenBLAS, compiler, wheel artifacts, browser, and host CPU still need to be captured/revalidated there.

The repository intentionally excludes large model/data/evidence files from Git. To submit reproducible results, deliver the source together with the evidence/model acquisition instructions and the local evidence bundle or an access-controlled export. Do not publish call audio/transcripts from real users by default.

## 13. Recommendation and unresolved gates

The M11 handoff package is complete as a documented, runnable **prototype** with a three-language demo, raw evidence links, conservative analysis, reproduction runbook, and production design. Full assignment qualification remains **partial**: Common Voice/real telephone access is absent, alternatives are research-only, sub-second text is not met, no sustained multi-language saturation run or production SLO exists, and an independent clean-machine test was not performed. Before any production promise, choose quantitative SLOs, obtain approved channel data, rerun a statistically useful held-out cohort, measure repeated mixed-load saturation/endurance on target CPUs, validate failure/drain/security gates, and then compute node counts with explicit headroom. Preserve unknowns until measurements justify replacing them.
