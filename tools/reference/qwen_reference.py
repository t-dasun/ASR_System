#!/usr/bin/env python3
"""Official offline CPU reference, sequential calls, same local checkpoint."""
import argparse
import json
import os
from pathlib import Path
import time

LANGUAGES = {"en": "English", "zh": "Chinese", "id": "Indonesian"}


def run(args):
    os.environ["HF_HUB_OFFLINE"] = "1"
    os.environ["TRANSFORMERS_OFFLINE"] = "1"
    os.environ["CUDA_VISIBLE_DEVICES"] = ""
    import torch
    from qwen_asr import Qwen3ASRModel

    if torch.version.cuda is not None:
        raise RuntimeError("Validation requires CPU-only PyTorch wheel")
    torch.set_num_threads(args.threads)
    torch.set_num_interop_threads(1)
    torch.manual_seed(42)
    if args.wav:
        records = [{"id": "vendor_smoke", "file": str(args.wav), "language": args.language,
                    "cohort": "exploratory"}]
    else:
        records = [json.loads(line) for line in args.manifest.read_text().splitlines()]
    if args.cohort != "all":
        records = [row for row in records if row["cohort"] == args.cohort]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x") as output:
        start = time.monotonic()
        model = Qwen3ASRModel.from_pretrained(str(args.model), device_map="cpu", dtype=torch.bfloat16,
                                            attn_implementation="eager", max_inference_batch_size=1,
                                            max_new_tokens=256, local_files_only=True)
        model.model.eval()
        model.model.generation_config.do_sample = False
        load_seconds = time.monotonic() - start
        for row in records:
            start = time.monotonic()
            result = model.transcribe(audio=row["file"], language=LANGUAGES[row["language"]])[0]
            record = {"id": row["id"], "language": row["language"], "text": result.text,
                      "detected_language": result.language, "elapsed_s": time.monotonic() - start,
                      "model_load_s": load_seconds, "dtype": "bfloat16", "device": "cpu",
                      "torch": torch.__version__, "threads": args.threads, "mode": "offline",
                      "max_new_tokens": 256, "status": "COMPLETE"}
            output.write(json.dumps(record, ensure_ascii=False) + "\n")
            output.flush()
            print(f"{row['id']}: {record['elapsed_s']:.2f}s", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, default=Path("datasets/manifests/fleurs_m0.jsonl"))
    parser.add_argument("--wav", type=Path, help="Single local smoke input instead of manifest")
    parser.add_argument("--language", choices=LANGUAGES, default="en", help="Language for --wav")
    parser.add_argument("--model", type=Path, default=Path("models/qwen3-asr-0.6b"))
    parser.add_argument("--cohort", choices=["exploratory", "acceptance", "all"], default="exploratory")
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--output", type=Path, required=True)
    run(parser.parse_args())
