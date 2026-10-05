# Qwen time multiplexing gate

Status on 2026-10-05: **two complete-audio requests worked concurrently; live call time multiplexing is not established.** This is an experimental branch. The existing `antirez/qwen-asr` path remains the measured live-stream baseline.

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
