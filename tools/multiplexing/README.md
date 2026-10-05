# Qwen time multiplexing gate

For the **matched topology experiment**, run `python3 tools/multiplexing/compare_topologies.py --output results/time_multiplexing/my_matched_topologies --calls-per-wav 2`. This invokes the main C++ `asr-cli load` runner for the same EN/ID/ZH WAVs across native 1-worker/1-session, shared 1-worker/2-session, native 2-worker/1-session, and shared single-session control layouts. Python performs orchestration and offline scoring; it does not infer or pace audio. The recorded run completed 24/24 calls with identical final transcripts. See [matched report](../../docs/MATCHED_TOPOLOGY_COMPARISON.md), [full JSON matrix](matched_topology_comparison_20261005.json), [metric CSV](matched_topology_metrics_20261005.csv), and [per-call CSV](matched_topology_calls_20261005.csv). Model-internal stable-word/active-compute timings remain unavailable; shared ready-job queue wait and offline invocation wall timings are now instrumented.

Status on 2026-10-05: the branch has an **experimental shared-model WebSocket server** that accepts paced PCM for multiple active calls through serial prefix redecoding. A one-process 1/2/4/8-call screen found pre-EOF text for 1/1, 2/2, 4/4, and 6/8 calls respectively. This is not a validated production call capacity or native resumable streaming. See the [results and architecture](../../docs/TIME_MULTIPLEXING_RESULTS.md). The existing process-isolated `antirez/qwen-asr` path remains the main service baseline.

## Decision from the first gate

The pinned llama.cpp server loaded Qwen3-ASR-0.6B Q8 on CPU with `--parallel 2`. Two requests carrying the same *complete* 1.2-second English PCM WAV overlapped and both returned `language English<asr_text>And so my.`. Each first response text arrived around 451 ms after its HTTP request began; both requests completed around 567 ms. This proves the complete-audio endpoint can occupy two server slots on this one fixture. It does **not** measure first text from a paced call start: the full WAV is present in each request before inference begins. It also does not establish accuracy or sustained capacity. See [raw probe output](llama_complete_audio_two_calls_20261005.json).

The pinned server's `input_audio` path takes a complete base64 audio object or URL and turns it into one media marker before dispatch ([source](https://github.com/ggml-org/llama.cpp/blob/806eee9841de5f2c20f9d43914117f157d2baacc/tools/server/server-common.cpp#L1167-L1183)). The tested HTTP contract has no chunk/sequence/EOF input messages for an ongoing call. Setting `"stream": true` streams **output tokens after that complete input**; it is not incremental PCM ingestion. Therefore this endpoint fails the assignment's live-input gate even though its generic server supports parallel slots. A different llama.cpp API or a native extension would require its own causal-input proof.

## Reproduce the complete-audio screen

Source and GGUF hashes are pinned in [`third_party/revisions.lock`](../../third_party/revisions.lock); the source and model files stay out of Git. The Q8 model/projector pair totals approximately 1.02 GB on disk. From repository root:

```bash
git clone https://github.com/ggml-org/llama.cpp.git third_party/llama.cpp
git -C third_party/llama.cpp checkout 806eee9841de5f2c20f9d43914117f157d2baacc
hf download ggml-org/Qwen3-ASR-0.6B-GGUF \
  Qwen3-ASR-0.6B-Q8_0.gguf mmproj-Qwen3-ASR-0.6B-Q8_0.gguf \
  --revision 928ab958557df9aa2ef1c93e0e83c7ad0933fae2 \
  --local-dir models/qwen3-asr-0.6b-gguf
sha256sum models/qwen3-asr-0.6b-gguf/*.gguf
cmake -S third_party/llama.cpp -B build/llama-cpu -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DGGML_NATIVE=ON -DGGML_CUDA=OFF \
  -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF \
  -DLLAMA_BUILD_SERVER=ON -DLLAMA_CURL=OFF
cmake --build build/llama-cpu --target llama-server -j 8
```

Run the server on loopback:

```bash
build/llama-cpu/bin/llama-server \
  -m models/qwen3-asr-0.6b-gguf/Qwen3-ASR-0.6B-Q8_0.gguf \
  --mmproj models/qwen3-asr-0.6b-gguf/mmproj-Qwen3-ASR-0.6B-Q8_0.gguf \
  --host 127.0.0.1 --port 8766 --parallel 2 --ctx-size 4096 \
  --threads 4 --threads-batch 4 --no-warmup
```

In another terminal:

```bash
python3 tools/multiplexing/llama_audio_probe.py \
  --wav tests/fixtures/m8_jfk_1p2s.wav --concurrency 2 \
  --output results/time_multiplexing/my_two_calls.json
```

The probe uses only the Python standard library, refuses to overwrite an output file, checks PCM16/16 kHz/mono WAV format, and records both HTTP calls plus the input hash. Its `first_text_ms` is **request-to-first-output** with complete audio already uploaded. The historical run used Fedora Linux, Ryzen 9 9955HX, llama.cpp commit `806eee9`, Q8 GGUF model SHA-256 `bca259818b50ca7c4c05e9bdb35a5dc04fa039653a6d6f3f0f331f96f6aa1971`, and projector SHA-256 `41a342b5e4c514e968cb756de6cd1b7be39eff43c44c57a2ef5fc6522e36603d`.

## Next implementation gate

Keep the current `IASREngine` and paced WebSocket path as the reference. Before integrating llama.cpp as another engine, require an API that accepts sequential PCM chunks for the *same* call while withholding future audio and emits useful text before EOF. Then test two live call IDs at once, call isolation, cancellation, accuracy parity, latency, CPU, and memory. If llama.cpp cannot satisfy causal audio input without substantial native changes, refactor the current Qwen C runtime toward per-call stream state and a stepwise decoder, or compare a purpose-built online ASR runtime. Merely adding server slots or repeating complete-prefix WAV requests must remain labelled as a different experiment.

## Native one-model prefix scheduling experiment

The branch now includes `asr-native-prefix-multiplex`, a separate research executable linked to the existing pinned `antirez/qwen-asr` CPU library. It loads **one** Qwen context, starts two call timelines together, and serially calls the native offline decoder on each call's scheduled 4-second prefix. At each call's audio EOF it decodes that call's complete audio. Future audio is loaded by the WAV simulator but is not passed to inference before the scheduled prefix/EOF. The native `qwen_transcribe_stream_live` API is **not** used. This is causal **prefix redecoding**, not a continuation-capable stepwise decoder or an integrated ASR service.

The first run used a 12.58-second English clip and a 15.48-second Indonesian clip. English prefix text completed at 5.52 seconds from simultaneous call start, Indonesian at 6.90 seconds; both were nonempty and before their own EOF. Final results ended at 15.09 and 19.02 seconds. The model load took 803 ms with warm local cache; process RSS was 1.71 GiB after load and 2.66 GiB after both calls. These values are a two-call research observation, not a concurrency or memory SLO. [Original](native_prefix_two_calls_20261005.json) and [reversed-order](native_prefix_two_calls_reversed_20261005.json) records are tracked. Reversing which language ran first produced identical per-language preview and final text in this pair; the checker reports that invariant.

```bash
cmake --preset release-cpu
cmake --build --preset release-cpu --target asr-native-prefix-multiplex
build/release-cpu/asr-native-prefix-multiplex \
  models/qwen3-asr-0.6b \
  datasets/prepared/fleurs_m4/fleurs_en_us_validation_1605_16.wav English \
  datasets/prepared/fleurs_m4/fleurs_id_id_validation_1520_4.wav Indonesian \
  4000 > results/time_multiplexing/native_prefix_two_calls.json
build/release-cpu/asr-native-prefix-multiplex \
  models/qwen3-asr-0.6b \
  datasets/prepared/fleurs_m4/fleurs_id_id_validation_1520_4.wav Indonesian \
  datasets/prepared/fleurs_m4/fleurs_en_us_validation_1605_16.wav English \
  4000 > results/time_multiplexing/native_prefix_two_calls_reversed.json
python3 tools/multiplexing/check_native_prefix_probe.py \
  results/time_multiplexing/native_prefix_two_calls.json \
  results/time_multiplexing/native_prefix_two_calls_reversed.json
```

This shows a possible one-process/multiple-active-call scheduling policy without concurrent entry into unsafe native globals. It still retranscribes audio prefixes, so compute cost rises with preview frequency and long call duration. It lacks per-call input queues, incremental PCM IPC, cancellation isolation, bounded scheduling delay, sustained accuracy/load evidence, and native persistent streaming state. Those are required before it can replace the production-path engine or alter admission/sizing claims. The next engineering decision is whether measured prefix redecoding can meet the chosen latency/CPU budget; if not, expose resumable per-call state in a patched native runtime or adopt an online ASR runtime.

## Experimental two-call WebSocket server

The main `asr-cli` engine factory selects `PrefixMultiplexEngine` when `model.runtime=qwen_prefix`. Each connection has its own call ID, ordered PCM buffer, transcript events, EOF, and cancellation. The active-call limit is configurable from 1 to 8 through `workers.max_sessions_per_process`; one inference thread schedules ready calls round robin and never enters the native Qwen decoder concurrently. It decodes the first 4 seconds as a provisional preview, then the complete buffered call at EOF. The 60-second PCM limit is per call. This is **call time multiplexing by serial prefix redecoding**, not native decoder state switching or parallel inference. The standalone `asr-prefix-server` is an optional launcher for the same engine.

The **main C++ `run`, `load`, `sweep`, and `serve` commands** now use this runtime through [`qwen_prefix_shared.yaml`](../../configs/qwen_prefix_shared.yaml). `serve` provides the existing REST suite/jobs/history and audio/observer WebSocket routes. Its REST benchmark jobs reuse the loaded model under the service admission gate. `/v1/runtime` exposes active calls and queued decode jobs; `/v1/capabilities` explicitly reports process isolation and hard watchdog as unavailable. `serve-prefix` is a shortcut for this same service. Python scripts in this directory are test clients and resource samplers; ASR inference and load/sweep execution are C++.

The first paced two-call WebSocket probe sent 200 ms chunks on simultaneous English and Indonesian connections. Both shared worker ID `prefix_shared_0`. English preview arrived at 5.00 s before 12.58 s EOF; Indonesian preview at 6.40 s before 15.48 s EOF. Final events arrived at 15.06 s and 18.92 s, respectively. Both calls returned success, with 63/78 ACKs and no cross-call event IDs. A second run confirmed admission: a third call received `resource_exhausted` with zero credits while the first two remained active. Its preview order reversed (Indonesian 5.40 s, English 6.40 s) and both calls still completed. These are **two-pair observations**, not sustained capacity, p95 latency, or memory savings. [First raw transport record](prefix_websocket_two_calls_20261005.json); [admission run](prefix_websocket_two_calls_admission_20261005.json).

```bash
cmake --preset release-cpu
cmake --build --preset release-cpu --target asr-cli asr-prefix-server
build/release-cpu/asr-cli serve --config configs/qwen_prefix_shared.yaml --port 8767
# Equivalent standalone launcher:
# build/release-cpu/asr-prefix-server models/qwen3-asr-0.6b 8767 8 4000 4
# In another terminal:
python3 tools/multiplexing/probe_prefix_websocket.py \
  --english datasets/prepared/fleurs_m4/fleurs_en_us_validation_1605_16.wav \
  --indonesian datasets/prepared/fleurs_m4/fleurs_id_id_validation_1520_4.wav \
  --output results/time_multiplexing/my_prefix_websocket_two_calls.json
```

`probe_prefix_websocket.py` checks a **two-slot** server, so set `--set workers.max_sessions_per_process=2` on the main service for that probe. To verify the default eight-slot service, its REST runtime, exclusive live/suite gate, and four-call C++ network job:

```bash
python3 tools/multiplexing/probe_main_service.py --port 8767 \
  --english datasets/prepared/fleurs_m4/fleurs_en_us_validation_1605_16.wav \
  --indonesian datasets/prepared/fleurs_m4/fleurs_id_id_validation_1520_4.wav \
  --mandarin datasets/prepared/fleurs_m4/fleurs_cmn_hans_cn_validation_1559_6.wav \
  --output results/time_multiplexing/my_main_service_verification.json
```

The **20-call-per-level C++ screen** is generated by the main CLI, with the service stopped so the screen gets the host memory and CPU:

```bash
build/release-cpu/asr-cli sweep --config configs/qwen_prefix_shared.yaml \
  --set audio.path=../tests/fixtures/m8_jfk_1p2s.wav \
  --set output.directory=../results/time_multiplexing/my_main_cpp_screen \
  --strategy scale --axis concurrency=1,2,4,8 --calls 20 --languages en --mode direct
```

The final sweep completed 80/80 calls with **Qwen custom, OpenMP, and BLAS budgets all set to four**. See [verified-budget C++ screen](main_cpp_openmp4_screen_20261005.json), [eight-call long result](main_cpp_openmp4_long_n8_20261005.json), [three-language REST/network service verification](main_cpp_openmp4_service_verification_20261005.json), and [deadline failure](main_cpp_deadline_20261005.json). The [results document](../../docs/TIME_MULTIPLEXING_RESULTS.md) separates short EOF throughput from live-preview timing and labels earlier results that used OpenMP defaults.

The probes require Python `websockets`. Native model artifacts and prepared FLEURS WAVs are local inputs; see the main reproduction guide for their acquisition. The 1/2/4/8-call paced load screen, eight-slot admission, cancellation isolation, CPU/RSS readings, and limits are recorded in the [results document](../../docs/TIME_MULTIPLEXING_RESULTS.md). For example, repeat the four-call level against the eight-slot server (obtain its PID with `pidof asr-cli`):

```bash
python3 tools/multiplexing/prefix_load_probe.py --port 8767 \
  --server-pid "$(pidof asr-cli)" \
  --call en:datasets/prepared/fleurs_m4/fleurs_en_us_validation_1605_16.wav \
  --call id:datasets/prepared/fleurs_m4/fleurs_id_id_validation_1520_4.wav \
  --call en:datasets/prepared/fleurs_m4/fleurs_en_us_validation_1605_16.wav \
  --call id:datasets/prepared/fleurs_m4/fleurs_id_id_validation_1520_4.wav \
  --reference tools/multiplexing/prefix_load_n2_20261005.json \
  --output results/time_multiplexing/my_prefix_load_n4.json
python3 tools/multiplexing/probe_prefix_admission.py \
  --port 8767 --slots 8 --output results/time_multiplexing/my_admission_n8.json
python3 tools/multiplexing/probe_prefix_cancel.py --port 8767 \
  --english datasets/prepared/fleurs_m4/fleurs_en_us_validation_1605_16.wav \
  --indonesian datasets/prepared/fleurs_m4/fleurs_id_id_validation_1520_4.wav \
  --output results/time_multiplexing/my_cancel.json
```

Prefix redecoding cost grows with the number and length of previews; the current one-preview policy bounds that cost but gives no later pre-EOF revisions. Sustained mixed-duration load, held-out accuracy, target-host headroom, crash recovery, and watchdog/cancellation during blocking decode remain qualification work.
