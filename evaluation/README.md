# Evaluation

`scoring.py` implements replaceable offline Unicode normalization and a bounded Levenshtein evaluator, aligned S/D/I evidence, English/Indonesian WER, and Mandarin code-point CER. It preserves human references and raw hypotheses, reports completed-only and failure-inclusive populations, and is independently cross-checked against JiWER. This module performs no inference. See [M4 guide](../docs/m4-code.md); capacity analysis remains later work.
