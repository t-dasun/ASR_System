# Resumable streaming: implementation and pilot results

**Completed full study:** [Final resumable report](FINAL_REPORT_RESUMABLE.md) covers all twelve layouts, 1,170 direct calls, network failures, language-separated graphs and updated sizing. The pilot evidence below remains a separate focused check.

## What changed

The main C++ path now supports `model.runtime=qwen_stream`, alongside the existing native and prefix presets. One loaded weight set per worker is borrowed by independent per-call contexts. Each call owns its encoder-window cache, decoder KV/prefill buffers, token history, result accumulator, language/prompt metadata and scratch buffers. Model weights are not copied per call. One worker executes one decode quantum at a time; this is time multiplexing, not batching.

`scripts/prepare_qwen_stream.py` derives a resumable API from the pinned native loop in generated CPU build sources. The vendor checkout stays pristine. Create/step/EOF/destroy operations preserve state across scheduling turns. Each turn processes at most one audio step, then requeues behind already waiting calls. Expired calls are retired first. EOF may require several steps if the decoder has fallen behind. Cancellation/decode deadlines are checked between generated tokens; individual kernels remain blocking.

**This is resumable windowed streaming, not a strictly incremental encoder.** Completed local-attention windows and unchanged decoder prefills are reused. The current partial encoder window can be re-encoded as more audio arrives, following the native algorithm. Passing cumulative PCM to the step API does not invoke full-prefix offline transcription. This wrapper retains its 60-second input bound; production VAD/long-call segmentation and automatic recovery remain separate work.

## Run it

```bash
cmake --build --preset release-cpu
build/release-cpu/asr-cli serve \
  --config configs/qwen_stream_shared.yaml \
  --manifest datasets/manifests/demo_calls.jsonl \
  --set workers.processes=2 --set workers.max_sessions_per_process=2 \
  --port 8081
```

Open the dashboard at `http://127.0.0.1:5173`, set service origin to `http://127.0.0.1:8081`, and click Connect. It should report `qwen_stream_process_pool` and show **Decode step**. Transport chunks are a separate control. Two workers with two slots each admit four concurrent calls. Use suite mode for multiple paced C++ WAV calls; separate browser clients can also stream independent calls.

```bash
# One worker, two active sessions, through C++ WebSockets.
build/release-cpu/asr-cli load --config configs/qwen_stream_shared.yaml \
  --manifest datasets/manifests/demo_calls.jsonl \
  --calls 4 --concurrency 2 --languages en,id,zh --mode network

# Verify the running service; choose a new output directory.
.venv-reference/bin/python tools/testing/run_resumable_check.py \
  --port 8081 --output results/resumable-check-new
```

Cold CLI planning retains its conservative worker-plus-reserve memory gate. A two-worker cold CLI run was rejected while other models were resident. The successful two-worker check used the loaded service and its warm preflight; no memory guard was disabled. The CLI marks weights loaded only after engine construction.

## Output and quality settings

| Setting | New preset | Meaning |
|---|---:|---|
| `model.decode_step_ms` | 2000 | Audio per scheduling quantum; 1–8 seconds supported |
| `model.stream_unfixed_chunks` | 0 | Earlier provisional emission; 2 matches the native cold-start withholding policy |
| `model.max_new_tokens` | 32 | Native generation bound per step |
| `model.refine_final` | false | Finish streaming state without a separate whole-recording pass |
| `workers.max_sessions_per_process` | 8 configured maximum | Independent mutable state per slot; not eight-call validated capacity |

To keep a whole-recording final refinement, start the service with `--set model.refine_final=true`. It preserves progressive partials but adds EOF work and may reintroduce offline decode timeouts. To match native initial withholding, use `--set model.stream_unfixed_chunks=2`; at a two-second step, first published text can move toward six seconds plus computation.

No setting guarantees WER/CER improvement. Worker/thread count, refinement and withholding policy are startup settings. The step interval can vary per call. Early nonempty text may be wrong or contain a filler: it should not be equated with meaningful correct recognition.

## Same-input pilot comparison

Both modes used the same four demo WAVs, 200 ms PCM chunks, four compute threads and two concurrent calls over C++ WebSockets. Prefix preview and streaming step were both two seconds. Prefix mode refines the whole recording; streaming uses zero withheld chunks and no separate EOF refinement. These are different output/quality policies.

One pass, two English files and one file per other language: **pilot evidence, not the ten-file/language capacity study or a p95/SLO validation**. All four calls completed in both runs. Primary scores use completed calls and retained references. First-text and EOF figures below are worker/controller measurements, not browser arrival timings.

| Recording | Lang | Prefix first / EOF s | Stream first / EOF s | Prefix WER/CER % | Stream WER/CER % | Stream partials |
|---|---|---|---|---:|---:|---:|
| fleurs_en_us_validation_1605_16 | en | 2.76 / 2.40 | 3.52 / 2.24 | 7.69 | 7.69 | 6 |
| fleurs_id_id_validation_1520_4 | id | 3.52 / 3.50 | 2.87 / 1.78 | 8.33 | 16.67 | 7 |
| fleurs_cmn_hans_cn_validation_1559_6 | zh | 4.46 / 2.00 | 2.89 / 1.81 | 0.00 | 10.71 | 7 |
| fleurs_en_us_validation_1523_142 | en | 2.66 / 4.07 | 2.75 / 1.56 | 5.26 | 0.00 | 5 |

The streaming pilot produced repeated updates and shorter EOF delays on these recordings. First-text results and final quality varied. The original capacity/sizing coefficients must not be replaced with a four-recording mixed-language goodput value. [Paired raw summaries](../results/resumable-comparison/comparison.json).

The separate one-call Indonesian refinement check completed, changed WER from 16.67% in the unrefined pilot to 8.33%, and recorded **3.42 seconds of additional refinement**. Worker EOF delay was 4.59 seconds. Its concurrency differs from the paired pilot, so this is a functionality/quality observation, not an isolated latency-effect estimate. [Raw result](../results/resumable-refinement-check/).

## Verification evidence

- **C++ real-model isolation test:** two interleaved English/Indonesian streams exactly matched the native streaming loop configured with the same step/token/withholding settings. Prefill reuse was positive. Token-boundary cancellation did not contaminate a subsequent stream. [Log](../results/resumable-comparison/state-isolation.log).
- **One worker / two sessions:** four network calls completed with 5–7 nonempty partial events each, and 253–479 reused prefill tokens. [C++ artifacts](../results/resumable-smoke-1w2s/).
- **Two workers / two sessions each:** four simultaneous calls completed; snapshots recorded four active sessions, two on each worker. Sample, identity, event, progressive-output and reuse checks passed. [Status](../results/resumable-check-2w2s-v2/status.json), [calls](../results/resumable-check-2w2s-v2/calls.json).
- **Real UI:** EN/ID/ZH calls showed 6–8 revisions, arrival delays and reference-based WER/CER; zero browser errors. [UI evidence](../results/resumable-ui-20261006/demo.json).
- Existing C++ contracts passed 19/19; new UTF-8 and step-timing checks passed. Frontend build and seven tests passed. Python ran 17 checks, with optional JiWER parity skipped in the system environment.

## Measurements and limits

`stream_decode_wall_ns` sums every step; `stream_decode_queue_wait_ns` sums scheduler wait. `stream_decode_steps` and `stream_reused_prefill_tokens` are counters. `stream_invocation_wall_rtf` includes all step wall time plus optional final refinement divided by unique audio duration. Legacy offline-prefix RTF is null for this mode. Effective RTF includes pacing and finalization; internal kernel-active RTF/word alignment remain unavailable.

Browser arrival delays include transport and browser scheduling, and use browser timestamps. They are separate from worker EOF timing. Late observer events cannot overwrite newer/final text. Incomplete trailing UTF-8 bytes stay in state until complete; malformed or final-incomplete text fails explicitly.

The original 900-call curves remain a **legacy prefix** study. Same-ten-WAV/language accuracy/capacity curves, sustained runs, overload recovery and large-node sizing have not been revalidated for this mode. Demo inputs do not establish resolution of the previously failing recordings. Automatic respawn, hard kernel preemption and long-call segmentation are not added by this implementation.

## Files and reproducibility

New files: `scripts/prepare_qwen_stream.py`, `configs/qwen_stream_shared.yaml`, `tests/integration/resumable_state_test.cpp`, `tools/testing/run_resumable_check.py`, `src/core/include/asr/core/utf8.hpp` and this guide. Existing shared engine/pool/IPC, factory/config/service/CLI, telemetry and frontend controls/tests are extended. Python generates build extensions and orchestrates/scores; inference and WAV simulation remain C/C++.

```bash
# Actual-model native-loop parity, cache and cancellation check.
build/release-cpu/resumable-state-test models/qwen3-asr-0.6b \
  datasets/prepared/fleurs/fleurs_en_us_validation_1605_16.wav \
  datasets/prepared/fleurs/fleurs_id_id_validation_1520_4.wav

# Browser evidence, using a new directory and a free preview port 4173.
cd frontend
ASR_DEMO_EVIDENCE_DIR=../results/resumable-ui-new npm run test:e2e:resumable
```

Branch: `resumable-streaming`. The new runtime is opt-in; existing native/prefix configurations remain available. Generated results are local and Git-ignored. Untested call counts are not capacity claims.
