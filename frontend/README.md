# M8 engineering dashboard

This React/TypeScript/Vite console calls the existing C++ M7 service. It does not load Qwen in the browser and has no external runtime assets. Start the service first:

```bash
build/dev-mock/asr-cli serve --config configs/mock_baseline.yaml --port 8765
# or: build/release-cpu/asr-cli serve --config configs/qwen_native_single.yaml --port 8765
```

Then, from `frontend/`, run `npm ci`, `npm run dev`, and open `http://127.0.0.1:5173`. `npm run build` verifies types and creates a production bundle; `npm test` runs model-free contract tests. `npm run preview` serves the bundle at port 4173. The dashboard accepts only explicit `http://127.0.0.1:<port>` or `http://localhost:<port>` service origins and remembers the last valid loopback origin in browser local storage. The M7 server allows the dev/preview browser origins on loopback. It remains unsuitable for exposure outside the laptop.

The live call panel validates a local 16 kHz mono PCM16 RIFF/WAVE header and reads bounded PCM slices for one-credit, real-time paced WebSocket transport. Start/stop/reset use fresh IDs. A separate observation subscription provides transcript revisions and gap notifications. Browser latency and send lag use `performance.now()`; they are not mixed with server nanosecond timings.

The experiment editor submits the same M6 load/sweep service request as CLI commands. Resolve config and dry run are non-executing. Active suites have exclusive admission relative to interactive calls; stop is cooperative between calls. History shows side-by-side transcripts and server p50/p95 final latency from persisted suite summaries; allow-listed JSON/JSONL downloads include per-call events. Runtime telemetry is read every 2 seconds and charts publish every 5 seconds, independent of server sampling. Queue depth is explicitly marked unavailable rather than inferred. Structured console logs are dashboard events, not a server log ingestion endpoint.

Run `npm run test:e2e` from `frontend/` to start a local mock C++ service and Vite and exercise headless Chrome: EN/ID/ZH streaming to final, stop/reset, config resolution, load/sweep dry runs, two jobs, history comparison, download, log filters, and zero console errors. It requires `build/dev-mock/asr-cli` and system Chrome (`ASR_CLI`/`ASR_CHROME` can override paths). Mock timings establish UI plumbing only, not Qwen capacity. Run `npm run test:e2e:native` after building the release CPU binary and production dashboard to stream one prepared FLEURS tuning WAV per language through the real native engine in Chrome. That test passed all three languages with nonempty final transcripts and zero browser errors; it is an integration demo, not an accuracy evaluation.

For the repeatable native overhead screen, build the production bundle and release CPU binary, then run `npm run bench:ui-overhead`. It uses `configs/qwen_native_ui_overhead.yaml` and a 1.2-second speech fixture, warms once, and runs 10 calls in each closed/open/open/closed arm through one persistent C++ service. Its JSON report is `results/m8_dashboard_overhead.json`. The 2026-10-04 laptop run measured 20 native calls per condition: opening the actively polling dashboard changed pooled p50 final latency by +0.04%, p95 by +0.71%, mean suite wall time by +0.37%, and sampled service-tree peak RSS by −0.05%. These pass the predeclared 10% latency/wall and 5% RSS screening bounds, but are not a capacity or production SLO claim. Chrome's own memory is outside the service-tree RSS. See [M8 evidence](../docs/m8-code.md).

Set `ASR_DEMO_EVIDENCE_DIR=../results/my_new_demo` when running
`npm run test:e2e:native` to save `demo.json` (browser first-text timings,
hashes, transcripts) and completed-screen screenshots for all three languages.
The directory must not exist beforehand. The M11 evidence is in
`results/m11_three_language_demo_20261005_long/`; its evidence run requires
first text before audio EOF for each language. See the
[final technical report](../reports/FINAL_TECHNICAL_REPORT.md).

Production authentication/TLS, remote access, retention, and capacity qualification remain unimplemented.
