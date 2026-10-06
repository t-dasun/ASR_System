# Dashboard

React/TypeScript/Vite client for the C++ service: live WAV streaming, suite planning/jobs, history/artifacts, transcript revisions, worker/session/resource status.

```bash
npm ci
npm run dev
```

Open `http://127.0.0.1:5173` and set the running service origin. Browser WAVs must be 16 kHz mono PCM16; frames are paced over `/v1/asr`. Shared worker count and session slots are fixed by service startup and reflected in the UI.

See [run commands](../docs/CLI_QUICKSTART.md) and [test instructions](../docs/TESTING.md). `npm run build`, `npm test`, and `npm run test:e2e` verify the frontend; the mock browser test starts its own service and Vite. Native and pool browser checks require prepared inputs/model or an existing pool service.
