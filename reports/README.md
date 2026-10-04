# Reports

Templates and selected final reports belong here. Raw diagnostic/benchmark evidence belongs under `results`, with measured and extrapolated claims separated.

The [final technical report](FINAL_TECHNICAL_REPORT.md) is the M11 handoff
document. It links the three-language browser demonstration, raw M10 results,
reproduction commands, and the proposed production design; it explicitly
leaves production capacity and sub-second latency unqualified.

## M10 evidence report

`tools/reports/m10.py` is an offline derived-artifact generator. It takes explicit
M9 source directories, verifies every checksum in the sealed measured holdout,
checks the measured manifest hash, and recomputes language-level paired error
rates from raw accuracy records. The two-worker load, stress, and thread-factor
screens are included as clearly labelled supporting evidence, not as qualified
capacity runs. Its output is a unique `results/reports/<report-id>/` directory
containing `report.json`, `report_metadata.json`, and standalone `report.html`.
That JSON is discoverable through the existing `/v1/reports` API when the
service output root is `results`.

Run it with explicit paths (the paths below identify the current measured set):

```bash
python3 tools/reports/m10.py \
  --holdout results/m9_fleurs_holdout5_20261004 \
  --load results/m9_two_worker_recovery_20261004/load_18db6142cac638c4_cb7053286bb2dc1df5e3d0aaef75519f \
  --stress-audit results/m9_stress_native_timeout150s_20261004/audit.json \
  --threads-comparison results/m9_qwen_threads2_tuning3/compare_threads4.json \
  --capacity-screen \
    results/m10_capacity_screen_20261005/load_18db68ccd684b375_7f442e1bb99b0dfa95b11d3769d58c42 \
    results/m10_capacity_screen_20261005/load_18db68dbcffe241f_23a0c002a06389c6911d8cb968eb5c11 \
  --output results/reports/<new-report-id>
```

The PDF view uses ReportLab and is generated from that `report.json`:

```bash
python3 tools/reports/render_pdf.py \
  results/reports/<new-report-id>/report.json \
  output/pdf/m10-evidence-and-sizing.pdf
```

The 20-call one/two-worker curve is a short English-fixture diagnostic screen:
20/20 calls completed at each point, but each run lasted under one minute.
`tools/reports/capacity.py` rejects non-comparable suites and never promotes a
diagnostic M6 SLO pass to production capacity. The report intentionally leaves
all 50/100/200/500/1,000-leg node counts and hardware requirements unknown:
no sustained multi-language saturation knee or declared product SLO exists yet.
The PDF build depends on `reportlab`; the JSON and HTML generator uses Python's
standard library plus the existing M9 comparison module.

## Transport/decoder latency–accuracy matrix

`tools/evaluation/latency_matrix.py` runs four native configurations (100/200 ms
audio transport chunks × 1,000/2,000 ms decoder steps) on the same three
FLEURS tuning clips per language. It uses `run_measured.py --set` so the C++
engine, scorer, resource sampler, and raw-artifact seal remain unchanged.

```bash
.venv-reference/bin/python tools/evaluation/latency_matrix.py \
  --output results/my_new_chunk_decode_matrix --per-language 3
```

The output contains `plan.json`, four independently sealed experiment
directories, `report.json`, `report.md`, and `latency_accuracy.svg`. The
2026-10-05 measured screen is in `results/chunk_decode_matrix_20261005/` with
an additional PNG rendering. Three clips per language screen the tradeoff but
do not establish tail latency, population accuracy, or concurrent capacity.
