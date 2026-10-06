# CPU sizing for 50–1,000 concurrent call legs

Assignment basis: section 7 of `AIML_CPP_LEAD_ASR_Technical_Assignment_v2.docx`. This guide separates measured facts, session admission and conditional sustained capacity. It does not claim validated production capacity.

## 1. Measured basis

- CPU: Ryzen 9 9955HX, 16 physical / 32 logical CPUs; approximately 14.78 GiB RAM.
- Qwen3-ASR 0.6B, current shared-prefix C++ path, four compute threads per worker; one serial decode context per worker and up to eight admitted sessions. No inference batching.
- Same ten distinct WAVs per language, three repetitions per direct layout. Only one- and two-worker layouts measured. Two workers / 16 sessions repeat each ten-file language cohort twice per repetition. Warmup is separate.
- Peak loaded process-tree PSS is approximately 3.0 GiB for one worker and 4.4 GiB for two workers. Weights mapped from the same files can share physical pages on one host; separate hosts each need their own copy.
- At two workers / concurrency 16, completed-audio throughput is EN 1.333, ID 1.045 and ZH 6.610 audio seconds per wall second. English and Indonesian have 6/60 and 12/60 failed calls respectively. Throughput includes failure time, but counts only completed audio.
- At that layout, completed-call first-text p95 is EN 29.46 s, ID 29.60 s and ZH 17.26 s; EOF-to-final p95 is EN 73.58 s, ID 67.56 s and ZH 12.36 s. Completed-call effective RTF means are 3.45, 3.27 and 1.72 respectively. These are finite-call measurements, not stable live-call capacity.
- The two-worker network test exposed an unhealthy worker after `WebSocket send failed`. Its saved occupancy never exceeded eight per worker. Network reliability remains unresolved.

Evidence: [full report](../results/capacity_cpu_20261006_curves/report.md), [machine-readable curves](../results/capacity_cpu_20261006_curves/curve.json).

## 2. Simple equations

Define:

| Symbol | Meaning | Example assumption |
|---|---|---|
| N | Concurrent independent call legs | 50–1,000 |
| k | Session slots per worker | 8 maximum currently |
| u | Maximum planned capacity utilization | 0.70 (30% headroom) |
| d | Audio submitted for ASR per call per wall second | 1.0 if every PCM sample is submitted |
| q | Completed audio seconds per worker per wall second | 0.50 provisional, rounded below ID two-worker goodput / 2 |
| e | Throughput retained after contention / deployment differences | 0.80 illustrative assumption, unmeasured |
| w | Workers per node | 2, limiting extrapolation within a host |
| t | Compute threads per worker | 4 |

```text
Workers for session admission = ceil(N / (k × u))
Workers for processing demand = ceil(N × d / (q × e × u))
Required workers W = max(admission workers, processing workers)
Nodes H = ceil(W / w)
Configured compute threads = 4 × W
```

The processing check prevents sizing from session slots alone. With the example constants:

```text
W = max(ceil(N / 5.6), ceil(N × d / 0.28))
H = ceil(W / 2)
```

`q=0.50` is a provisional completed-audio goodput proxy from finite tests, not measured steady-state service capacity. `e=0.80` is an explicit estimate, not a measured scaling coefficient. The resulting formula is a planning scenario; queue stability and latency need production-shaped testing.

**Current code submits silence too, so use d=1.0.** Silence cannot be discounted merely because a caller is not speaking. A smaller d is conditional on implementing and validating VAD/utterance segmentation that actually avoids ASR work. That change would also require remeasuring q, particularly with repeated prefix decoding and different utterance lengths.

For a language mix, if comparable sustained q values become available:

```text
W_processing = ceil(N × sum(language_fraction[l] × d[l] / q[l]) / (e × u))
```

Use separate language demand estimates; do not average English WER with Mandarin CER. The example below uses the conservative Indonesian-derived proxy for every call, avoiding unsupported language-mix assumptions.

## 3. CPU and memory per node

CPU planning is based on thread configuration, not average measured CPU consumption:

```text
Logical CPU budget per node >= ceil((4 × w + 1) / 0.70)
```

The additional one CPU equivalent is an assumed allowance for gateway, simulation/transport and coordination. For two workers this is 13 logical CPU equivalents, rounded to a **16-logical-CPU node**. A vCPU here means a logical CPU of comparable performance; it does not imply equivalence to a dedicated physical core or arbitrary cloud vCPU. SMT, CPU frequency, memory bandwidth and NUMA can alter performance.

A rough loaded-memory envelope fitted to the measured PSS points is:

```text
ASR process-tree PSS GiB ≈ 1.6 + 1.5 × w
Provisioned RAM GiB >= (1.6 + 1.5 × w + 0.016 × active_sessions + 2) / 0.70
```

Here 1.6 GiB approximates shared/per-node memory and 1.5 GiB is incremental worker memory. This decomposition is inferred from only two layouts, not independently measured allocation categories. The extra 16 MiB/session is a conservative buffer reserve, not a measured slope, and may overlap memory already in the fitted envelope. Two GiB is an assumed OS/service reserve. For w=2 and 16 sessions the formula gives approximately 9.8 GiB; round to **16 GiB RAM/node**. This envelope is valid only as a short-call planning allowance; long conversations require bounded utterance buffers and remeasurement.

Select homogeneous **16 logical CPUs / 16 GiB RAM nodes, two workers each**, keeping both workers on one NUMA domain where possible. Do not infer eight-worker/node performance from this data. Shared memory is counted once per node; it is replicated across nodes.

## 4. Assignment table: continuous submitted PCM scenario

Assumptions: d=1, k=8, q=0.50, e=0.80, u=0.70; two workers per node, each node provisioned with 16 logical CPUs and 16 GiB RAM. Totals include rounding to full nodes.

| Concurrent legs | Required workers | Estimated logical CPUs / vCPU equivalents | RAM GiB | Nodes | Target RTF | Target p95 latency | Basis |
|---:|---:|---:|---:|---:|---|---|---|
| 50 | 179 | 1440 | 1440 | 90 | Sustained compute RTF <1; unverified | Not specified / not predicted | Conditional finite-test goodput extrapolation |
| 100 | 358 | 2864 | 2864 | 179 | Sustained compute RTF <1; unverified | Not specified / not predicted | Conditional finite-test goodput extrapolation |
| 200 | 715 | 5728 | 5728 | 358 | Sustained compute RTF <1; unverified | Not specified / not predicted | Conditional finite-test goodput extrapolation |
| 500 | 1786 | 14288 | 14288 | 893 | Sustained compute RTF <1; unverified | Not specified / not predicted | Conditional finite-test goodput extrapolation |
| 1000 | 3572 | 28576 | 28576 | 1786 | Sustained compute RTF <1; unverified | Not specified / not predicted | Conditional finite-test goodput extrapolation |

These large numbers are the result of a deliberately conservative all-audio scenario using the current low successful-audio goodput proxy. They are **not a procurement recommendation**. In particular, extra workers do not fix per-recording decode timeouts or transport faults. No successful continuous 50–1,000-call experiment has been run. Effective RTF measured from stream start to final includes pacing and finalization; it should not be confused with the sustained compute RTF target in this table. No p95 acceptance target was selected, and no equation here predicts a p95.

## 5. Slot capacity alone: why the small answer is insufficient

Ignoring processing demand and headroom, eight slots per worker gives:

| Calls | Minimum workers for slots only | Compute threads |
|---:|---:|---:|
| 50 | 7 | 28 |
| 100 | 13 | 52 |
| 200 | 25 | 100 |
| 500 | 63 | 252 |
| 1000 | 125 | 500 |

This is an admission limit, not real-time capacity. For example, 50 calls fit into seven workers, but that calculation says nothing about queueing or whether those workers can process incoming audio quickly enough.

## 6. Conditional VAD example

If a future validated pipeline submits only d=0.10 audio seconds per call-second and achieves the same q, all other assumptions unchanged:

| Calls | Conditional workers | Nodes | Total logical CPUs | Total RAM GiB |
|---:|---:|---:|---:|---:|
| 50 | 18 | 9 | 144 | 144 |
| 100 | 36 | 18 | 288 | 288 |
| 200 | 72 | 36 | 576 | 576 |
| 500 | 179 | 90 | 1440 | 1440 |
| 1000 | 358 | 179 | 2864 | 2864 |

This is a sensitivity example, not current capability or an assumed contact-center speech ratio. Ten percent submitted audio is not guaranteed; synchronized speech bursts can exceed average demand. Current code has no such validated VAD pipeline.

## 7. Production qualifications

- Recommend 30% planning headroom for CPU, memory and processing capacity, with bounded queues and admission control; validate that margin under representative bursts and failures. Thread reservations and observed utilization are different quantities.
- Existing two-worker scaling already shows contention/diminishing returns for English and Indonesian. Horizontal scaling keeps the measured worker density but introduces gateway, networking and load-balancing costs. The illustrative efficiency factor must be replaced with measurements on the intended nodes.
- Current session admission supports eight workers/service and eight sessions/worker, but only up to two workers were measured. A multi-node gateway with session affinity, health routing, draining and retry policy is required; this sizing document does not implement a cluster.
- Exceeded capacity produces growing decode queues, delayed first text/finals and transport/decode failures. Reject new sessions before queues grow without bound. Recovering live audio after worker failure requires an explicitly bounded replay buffer and transcript deduplication.
- Model sharing is local to a host. Distinct model variants, copies of weights, containers and NUMA placement may change physical memory and bandwidth. Keep four compute threads/worker as the measured configuration until a thread sweep supports another choice.
- Batching, quantization, language routing and genuine incremental decoding are possible future experiments. No capacity benefit from them is included here.
- The current shared-prefix engine is limited to short calls (up to 60 seconds of input), serial prefix/final whole-audio invocations and configured decode deadlines. Long calls, VAD, sustained sessions and network fault handling must be implemented and validated before production sizing can be endorsed.
- Completed-call WER/CER is the primary transcription-quality measure; failure rate remains a separate gate. Excluding failed calls from WER/CER does not remove their compute cost or make this system reliable.

Confidence: measured short-call resource costs are descriptive of this host; memory extrapolation and language-specific goodput are low-confidence with ten distinct files/language; steady-state capacity, large-cluster counts and p95 latency are unvalidated. Existing failures prevent a claim that the assignment’s production concurrency targets have been achieved.
