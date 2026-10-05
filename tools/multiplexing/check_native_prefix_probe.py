"""Check causal preview and language isolation across reversed two-call runs."""

import argparse
import json
from pathlib import Path


def by_language(path: Path):
    data = json.loads(path.read_text())
    if data.get("method") != "one_model_serial_prefix_redecode" or data.get("active_calls") != 2:
        raise ValueError(f"unexpected probe schema: {path}")
    return {call["language"]: call for call in data["calls"]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("original", type=Path)
    parser.add_argument("reversed", type=Path)
    args = parser.parse_args()
    original, reversed_run = by_language(args.original), by_language(args.reversed)
    if set(original) != set(reversed_run) or len(original) != 2:
        raise ValueError("different language sets")
    summary = {}
    for language in original:
        left, right = original[language]["events"], reversed_run[language]["events"]
        if [event["kind"] for event in left] != ["prefix_preview", "final"]:
            raise ValueError(f"missing preview/final in original {language}")
        if [event["kind"] for event in right] != ["prefix_preview", "final"]:
            raise ValueError(f"missing preview/final in reversed {language}")
        if not left[0]["text_before_eof"] or not right[0]["text_before_eof"]:
            raise ValueError(f"no nonempty pre-EOF preview for {language}")
        if left[0]["text"] != right[0]["text"] or left[1]["text"] != right[1]["text"]:
            raise ValueError(f"call-order-dependent text for {language}")
        summary[language] = {"pre_eof_both_orders": True, "text_invariant_to_order": True,
                             "first_text_ms": [round(left[0]["end_ms"], 1), round(right[0]["end_ms"], 1)]}
    print(json.dumps(summary, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
