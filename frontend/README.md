# Dashboard

React/TypeScript/Vite client for the C++ service: live WAV streaming, suite planning/jobs, history/artifacts, transcript revisions, worker/session/resource status.

```bash
npm ci
npm run dev
```

Open `http://127.0.0.1:5173` and set the running service origin. Browser WAVs must be 16 kHz mono PCM16; frames are paced over `/v1/asr`. Shared worker count and session slots are fixed by service startup and reflected in the UI.

See [run commands](../docs/CLI_QUICKSTART.md) and [test instructions](../docs/TESTING.md). `npm run build`, `npm test`, and `npm run test:e2e` verify the frontend; the mock browser test starts its own service and Vite. Native and pool browser checks require prepared inputs/model or an existing pool service.

## Live controls and measurements

Start `asr-cli serve` first (default port 8080), then connect the dashboard to `http://127.0.0.1:8080`. The frontend cannot start a model process by itself. Chunk size controls media pacing; native decode step and shared-prefix preview interval control different decoder behaviors. Shared mode still produces one preview and one EOF refinement.

First-text and EOF-to-final arrival measurements use the browser clock. Optional reference text enables completed-call WER/CER. Suites/history show worker timings and use manifest references where available; scoring coverage and failures remain visible.

`npm run test:e2e:shared` verifies real Qwen UI calls in three languages, with two workers, 100 ms chunks and a 2 s preview. This is a controls/integration test on short inputs, not a capacity benchmark. Set `ASR_DEMO_EVIDENCE_DIR` to a new directory to save screenshots and `demo.json`. It excludes the three previously identified timeout recordings from its demonstration selection; they remain in the capacity study.
