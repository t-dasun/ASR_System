#!/usr/bin/env python3
"""Main C++ multi-input, multi-worker ASR verification and per-language report.
Python starts services, submits C++ suites and scores saved transcripts only.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.testing.scoring import AccuracyEvaluator
from tools.testing.metrics import distribution, TIMINGS

# Layout tuples are (workers, session slots per worker, total concurrency).
CASES = {"shared_1w_1s": (1, 1, 1), "shared_1w_2s": (1, 2, 2),
         "shared_2w_1s": (2, 1, 2), "shared_2w_2s": (2, 2, 4)}

def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def write(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n")

def request(port, route, body=None):
    req = urllib.request.Request(f"http://127.0.0.1:{port}{route}",
        data=None if body is None else json.dumps(body).encode(),
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=15) as response:
        return json.load(response)

def read_calls(suite, sources):
    scorer = AccuracyEvaluator()
    calls = []
    for phase in suite["phases"]:
        for item in phase["calls"]:
            source = sources[item["call"]["recording_id"]]
            path = Path(item["directory"])
            summary = item.get("summary") or json.loads((path/"summary.json").read_text())
            events = [json.loads(line) for line in (path / "events.jsonl").read_text().splitlines()]
            chunks = [json.loads(line) for line in (path / "audio_timing.jsonl").read_text().splitlines()]
            text = summary.get("transcript", "") if item["status"] == "COMPLETE" else ""
            checks = {"real_model": summary.get("is_mock", False) is False,
                "exact_samples": summary.get("audio_samples", summary.get("measurements", {}).get("audio_duration_ns", 0) // 62500) == source["num_samples"],
                "one_final": (sum(e["kind"] == "final" for e in events) == 1 if item["status"] == "COMPLETE" else sum(e["kind"] == "failed" for e in events) == 1),
                "call_identity": all(e["run_id"] == item["run_id"] and e["call_id"] == item["run_id"] + "_call_0" for e in events),
                "ordered_sequences": [e["sequence"] for e in events] == list(range(len(events))),
                "publication_order": all(e["published_ns"] >= e["produced_ns"] for e in events),
                "no_overflow": summary.get("audio_overflows", 0) == 0,
                "availability_pacing": all(c["sent_ns"] >= c["scheduled_ready_ns"] for c in chunks)}
            calls.append({"recording_id": source["id"], "language": source["language"], "run_id": item["run_id"],
                "worker_id": summary.get("worker_id") or (events[0]["worker_id"] if events else None), "status": item["status"], "transcript": text,
                "checks": checks, "failure": summary.get("error"), "before_eof_text": any(e["before_eof"] and e["text"].strip() for e in events),
                "accuracy": scorer.score(source["reference"], text, source["language"]),
                "measurements": summary.get("measurements", {}), "directory": str(path),
                "raw_delays_ns": {"send_lag": [c["lag_ns"] for c in chunks],
                    "controller_queue_wait": [c["sent_ns"] - c["enqueued_ns"] for c in chunks],
                    "submit_wall": [c["submit_returned_ns"] - c["submit_started_ns"] for c in chunks],
                    "publication_delay": [e["published_ns"] - e["produced_ns"] for e in events]}})
    return calls

def summarize(output, plan, jobs):
    for job in jobs:
        for call in job["calls"]:
            if not call.get("worker_id"):
                events = [json.loads(line) for line in (Path(call["directory"])/"events.jsonl").read_text().splitlines()]
                if events: call["worker_id"] = events[0]["worker_id"]
        cpu_seconds = cpu_wall = 0.0
        for phase in job["suite"].get("phases", []):
            raw = Path(job["suite"]["directory"]) / phase["resource_samples_path"]
            samples = [json.loads(line) for line in raw.read_text().splitlines()]
            for before, after in zip(samples, samples[1:]):
                interval = (after["timestamp_ns"] - before["timestamp_ns"]) / 1e9
                cpu_wall += interval
                cpu_seconds += interval * sum(p["cpu_core_equivalents"] or 0 for p in after["processes"])
        job["sampled_cpu_core_seconds"] = cpu_seconds
        job["cpu_sampled_wall_seconds"] = cpu_wall
    baseline = {c["recording_id"]: c["transcript"] for j in jobs if j["case"] == "shared_1w_1s"
                for c in j["calls"] if c["status"] == "COMPLETE"}
    groups = {}
    lines = ["# Shared worker pool: per-language C++ results", "",
        "Main C++ `serve`/REST `run_load` performs WAV preparation, chunk pacing, routing and inference. Python orchestrates and scores saved text.", "",
        f"Same {plan['per_language']} unique heldout WAVs per language in each layout. One run per WAV per layout. Native live decoding is not included in this new matrix; all four layouts use the same prefix policy and priority scheduler. One-worker layouts run the model in the service process; two-worker layouts use persistent child processes and IPC.", "",
        "Services load contexts once before the suites. Admission/startup excludes context loading. Cold CLI preflight plans are saved separately; these are warm service measurements. Four compute threads per worker; two workers have eight configured threads total. No hard OS CPU quota.", ""]
    for language in ("en", "id", "zh"):
        lines += [f"## {language}", "", "| Layout | Complete | Pre-EOF text | First text mean / p95 s | Final mean / p95 s | EOF delay mean s | Error | Peak RSS GiB | Audio s / wall s |", "|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
        for name in CASES:
            selected = [j for j in jobs if j["case"] == name and j["language"] == language and j["kind"] == "language"]
            calls = [c for j in selected for c in j["calls"]]
            if not calls:
                continue
            timings = {key: distribution([c["measurements"][key] / 1e6 for c in calls
                if c["status"] == "COMPLETE" and c["measurements"].get(key) is not None]) for key in TIMINGS if key.endswith("_ns")}
            for key in ("effective_rtf", "offline_decode_wall_rtf", "inference_compute_rtf"):
                timings[key] = distribution([c["measurements"][key] for c in calls
                    if c["status"] == "COMPLETE" and c["measurements"].get(key) is not None], "ratio")
            context_loads = {c["worker_id"]: c["measurements"].get("shared_model_load_ns") for c in calls}
            timings["shared_model_load_ns"] = distribution([v/1e6 for v in context_loads.values() if v is not None],
                population="unique reused model contexts, loaded before these calls")
            delays = {key: distribution([v/1e6 for c in calls for v in c["raw_delays_ns"][key]],
                population="recognition events" if key == "publication_delay" else "delivered chunks")
                for key in ("send_lag", "controller_queue_wait", "submit_wall", "publication_delay")}
            edits = sum(c["accuracy"]["edits"] for c in calls)
            units = sum(c["accuracy"]["reference_units"] for c in calls)
            peak = max(j["suite"]["metrics"].get("sampled_peak_tree_rss_bytes", 0) or 0 for j in selected)
            wall = sum(j["suite"]["metrics"]["measurement_wall_seconds"] for j in selected)
            audio = sum(j["suite"]["metrics"]["completed_audio_seconds"] for j in selected)
            group = {"calls": calls, "timings": timings, "chunk_event_delays": delays,
                "accuracy": {"metric": "CER" if language == "zh" else "WER", "edits": edits,
                    "reference_units": units, "rate": edits/units if units else None},
                "unique_wavs": len({c["recording_id"] for c in calls}),
                "completed": sum(c["status"] == "COMPLETE" for c in calls),
                "pre_eof_text": sum(c["before_eof_text"] for c in calls),
                "all_checks": all(all(c["checks"].values()) for c in calls) and all(j["suite"].get("measurement_failures", 0) == 0 for j in selected),
                "sampled_peak_tree_rss_bytes": peak, "audio_seconds_per_wall_second": audio/wall,
                "sampled_mean_cpu_cores": sum(j["sampled_cpu_core_seconds"] for j in selected) / sum(j["cpu_sampled_wall_seconds"] for j in selected),
                "sampled_peak_cpu_cores": max(p["resources"]["max_tree_cpu_core_equivalents"] for j in selected for p in j["suite"]["phases"]),
                "sampled_peak_tree_pss_bytes": max(p["resources"]["sampled_peak_tree_pss_bytes"] for j in selected for p in j["suite"]["phases"]),
                "exact_final_matches_single": sum(c["transcript"] == baseline.get(c["recording_id"]) for c in calls)}
            groups[f"{name}/{language}"] = group
            first, final = timings["first_usable_transcript_ns"], timings["final_result_ns"]
            lines.append(f"| {name} | {group['completed']}/{len(calls)} | {group['pre_eof_text']}/{len(calls)} | {first['mean']/1000:.3f} / {first['p95']/1000:.3f} | {final['mean']/1000:.3f} / {final['p95']/1000:.3f} | {timings['finalization_ns']['mean']/1000:.3f} | {group['accuracy']['metric']} {group['accuracy']['rate']*100:.2f}% | {peak/2**30:.2f} | {audio/wall:.3f} |")
        lines += ["", "### Stage timings (means in ms for completed calls)", "",
            "| Stage | " + " | ".join(CASES) + " |", "|---|" + "---:|"*len(CASES)]
        for key in ("startup_ns", "shared_model_load_ns", "prefix_decode_wall_ns", "prefix_decode_queue_wait_ns",
                    "eof_refinement_wall_ns", "eof_decode_queue_wait_ns", "runtime_queue_wait_ns", "offline_decode_wall_ns"):
            values = []
            for case in CASES:
                group = groups.get(f"{case}/{language}")
                value = group["timings"][key]["mean"] if group else None
                values.append("unavailable" if value is None else f"{value:.3f}")
            lines.append("| " + key + " | " + " | ".join(values) + " |")
        lines += ["", "Shared-model load is reused context metadata, not a new model load per call. Failed-call decode durations remain in the per-call JSON and the phase CPU/throughput measurements.", "",
            "| Layout | Configured compute threads | Mean / peak CPU cores | Peak PSS GiB | Effective RTF mean | Offline wall RTF mean |", "|---|---:|---:|---:|---:|---:|"]
        for case,(workers,slots,concurrency) in CASES.items():
            group = groups.get(f"{case}/{language}")
            if not group: continue
            lines.append(f"| {case} | {workers*4} | {group['sampled_mean_cpu_cores']:.2f} / {group['sampled_peak_cpu_cores']:.2f} | {group['sampled_peak_tree_pss_bytes']/2**30:.2f} | {group['timings']['effective_rtf']['mean']:.3f} | {group['timings']['offline_decode_wall_rtf']['mean']:.3f} |")
        lines += ["", "### Chunk/event delay p95 (ms)", "", "| Delay | " + " | ".join(CASES) + " |", "|---|" + "---:|"*len(CASES)]
        for key in ("send_lag", "controller_queue_wait", "submit_wall", "publication_delay"):
            values = []
            for case in CASES:
                group = groups.get(f"{case}/{language}")
                value = group["chunk_event_delays"][key]["p95"] if group else None
                values.append("unavailable" if value is None else f"{value:.3f}")
            lines.append("| " + key + " | " + " | ".join(values) + " |")
        lines += [""]
    lines += ["## Scheduling and limitations", "",
        "Least-active worker routing spreads calls across healthy workers before sharing a worker. Inside each worker, the oldest ready preview receives priority; at most two previews may bypass a waiting EOF refinement. The oldest EOF job then runs. Expired calls are retired first. A native decode cannot be preempted mid-invocation, so priority affects only the next job.", "",
        "Failed calls use an empty final hypothesis for failure-inclusive WER/CER. First/final latency distributions use completed calls only; phase throughput includes time spent on failures. Failure details remain in each per-call record.\n\nEOF delay is worker EOF receipt to final publication, not first-to-final time. Queue, offline decode, IPC publication, startup, effective RTF, and unavailable active-compute/stable-word fields are retained in `comparison.json`. Descriptive p95 from ten recordings is not a production tail guarantee. Accuracy uses corpus edits/reference units separately per language.", "",
        "Warm model contexts are reused across language suites, which run separately. Language-level memory/throughput values are suite measurements; multi-language smoke suites are separately marked. Automatic worker restart is not implemented: failed workers reject new assignments, surviving workers continue; restart the service to restore full capacity. A worker failure can fail every active session on that worker.", "",
        "Artifacts: `plan.json`, `jobs.json`, `comparison.json`, `calls.json`, `checksums.json`, service logs, cold CLI plans, runtime snapshots, and each original C++ suite. All raw timing records remain in the suite directories.", ""]
    failure_count = sum(c["status"] != "COMPLETE" for j in jobs for c in j["calls"])
    result = {"call_failures": failure_count, "plan": plan, "groups": groups, "jobs": jobs,
        "status": ("COMPLETED_WITH_CALL_FAILURES" if failure_count else "COMPLETE") if len(groups) == 12 and all(g["all_checks"] for g in groups.values()) else "INCOMPLETE"}
    write(output / "comparison.json", result)
    write(output / "calls.json", [{"case": j["case"], "kind": j["kind"], **c} for j in jobs for c in j["calls"]])
    (output / "comparison.md").write_text("\n".join(lines))
    return result

def run_suite(port, body, directory, case, language, kind, sources):
    job = request(port, "/v1/suites", body)
    deadline = time.monotonic() + 1800
    snapshots = []
    while time.monotonic() < deadline:
        state = request(port, f"/v1/jobs/{job['job_id']}")
        runtime = request(port, "/v1/runtime")
        snapshots.append(runtime)
        if state["status"] != "RUNNING":
            break
        time.sleep(0.5)
    assert state["status"] in ("COMPLETE", "FAILED") and "result" in state, {"status":state["status"], "error":state.get("error")}
    suite = state["result"]
    write(directory / f"{language}_{kind}_runtime.json", snapshots)
    calls = read_calls(suite, sources)
    workers, slots, concurrency = CASES[case]
    peak_active = [max(s["workers"][i]["active_sessions"] for s in snapshots) for i in range(workers)]
    assert all(n <= slots for n in peak_active), peak_active
    if body["calls"] >= concurrency:
        assert all(n >= 1 for n in peak_active), peak_active
        assert sum(peak_active) >= concurrency, peak_active
    pids = {w["process_id"] for s in snapshots for w in s["workers"]}
    assert len(pids) == workers, pids
    assert all(w["runtime_threads"] == w["blas_threads"] == 4 for s in snapshots for w in s["workers"])
    assert all(all(c["checks"].values()) for c in calls), [{"recording":c["recording_id"], "checks":c["checks"]} for c in calls if not all(c["checks"].values())]
    assert request(port, "/v1/runtime")["workers"] and all(w["active_sessions"] == 0 for w in request(port, "/v1/runtime")["workers"])
    return {"case": case, "language": language, "kind": kind, "request": body, "suite": suite,
        "calls": calls, "peak_active_per_worker": peak_active, "worker_process_ids": sorted(pids)}

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--per-language", type=int, default=10)
    parser.add_argument("--report-only", action="store_true")
    parser.add_argument("--stress", action="store_true", help="resource-guarded capacity curves instead of the four-case comparison")
    parser.add_argument("--repetitions", type=int, default=3, help="measured repetitions per stress point")
    parser.add_argument("--max-workers", type=int, default=8, help="stress worker counts 1..N (1..8)")
    parser.add_argument("--dense", action="store_true", help="stress every integer concurrency rather than scaling points")
    parser.add_argument("--reserve-gib", type=float, default=2, help="host RAM reserve during stress")
    parser.add_argument("--soak-seconds", type=int, default=0, help="additional max-occupancy network load on largest worker layout")
    parser.add_argument("--plan-only", action="store_true", help="write stress plan and input identities without inference")
    args = parser.parse_args()
    if args.stress:
        if not (2 <= args.per_language <= 50 and 1 <= args.max_workers <= 8 and 1 <= args.repetitions <= 20
                and args.reserve_gib >= 2 and args.soak_seconds >= 0):
            parser.error("invalid stress limits")
        from tools.testing.capacity_stress import run, report
        if args.report_only:
            saved = json.loads((args.output / "curve.json").read_text())
            report(args.output, saved["plan"], json.loads((args.output / "jobs.json").read_text()), saved["worker_runs"])
        else:
            run(args)
        return
    if args.plan_only or args.dense or args.soak_seconds:
        parser.error("plan-only, dense and soak options require --stress")
    output = args.output.resolve()
    if args.report_only:
        summarize(output, json.loads((output/"plan.json").read_text()), json.loads((output/"jobs.json").read_text()))
        return
    if output.exists():
        parser.error("output already exists")
    if not 2 <= args.per_language <= 50:
        parser.error("per-language must be 2..50 unique WAVs")
    output.mkdir(parents=True)
    records = [json.loads(line) for line in (ROOT/"datasets/manifests/fleurs_validation.jsonl").read_text().splitlines()]
    selected = {lang: [r for r in records if r["language"] == lang][:args.per_language] for lang in ("en", "id", "zh")}
    sources = {r["id"]: r for rows in selected.values() for r in rows}
    rows = [selected[lang][i] for i in range(args.per_language) for lang in ("en", "id", "zh")]
    manifest = output/"inputs.jsonl"
    manifest.write_text("".join(json.dumps({**r, "file": str(ROOT/r['file'])}, ensure_ascii=False)+"\n" for r in rows))
    for row in rows:
        assert digest(ROOT/row["file"]) == row["sha256"]
    cli = ROOT/"build/release-cpu/asr-cli"
    plan = {"per_language": args.per_language, "recordings": rows, "manifest_sha256": digest(manifest),
        "cli_sha256": digest(cli), "worker_sha256": digest(ROOT/"build/release-cpu/asr-prefix-worker"),
        "decode_guard_source_sha256": digest(ROOT/"scripts/prepare_qwen_guard.py"),
        "model_acquisition": json.loads((ROOT/"models/qwen3-asr-0.6b/acquisition.json").read_text()),
        "git_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "cases": CASES, "mode": "direct", "model_threads_per_worker": 4,
        "source_manifest_sha256": digest(ROOT/"datasets/manifests/fleurs_validation.jsonl")}
    write(output/"plan.json", plan)
    jobs = []
    write(output/"status.json", {"status":"RUNNING"})
    try:
        for case, (workers, slots, concurrency) in CASES.items():
            directory = output/case
            directory.mkdir()
            common = ["--config", "configs/qwen_prefix_shared.yaml", "--manifest", str(manifest),
                "--set", f"workers.processes={workers}", "--set", f"workers.max_sessions_per_process={slots}",
                "--set", "workers.scheduler=least_active", "--set", f"output.directory={directory}"]
            cold = subprocess.run([str(cli), "load-dry-run", *common, "--calls", str(args.per_language),
                "--concurrency", str(concurrency), "--languages", "en"], cwd=ROOT, text=True, capture_output=True, check=True)
            (directory/"cold_cli_plan.json").write_text(cold.stdout)
            command = [str(cli), "serve", *common, "--port", "0"]
            write(directory/"command.json", {"command": command, "cli_sha256": digest(cli), "worker_sha256": digest(ROOT/"build/release-cpu/asr-prefix-worker")})
            with (directory/"service.stdout").open("w") as out, (directory/"service.stderr").open("w") as err:
                process = subprocess.Popen(command, cwd=ROOT, stdout=out, stderr=err, start_new_session=True)
                try:
                    deadline=time.monotonic()+150; port=None
                    while time.monotonic()<deadline:
                        if process.poll() is not None:
                            raise RuntimeError(f"{case} service exited: {(directory/'service.stderr').read_text()[-2000:]}")
                        for line in (directory/"service.stdout").read_text().splitlines():
                            try:
                                value=json.loads(line)
                                if "port" in value: port=value["port"]
                            except json.JSONDecodeError:
                                pass
                        if port is not None:break
                        time.sleep(0.2)
                    assert port is not None, "service startup timeout"
                    caps=request(port,"/v1/capabilities")
                    assert caps["worker_processes"]==workers and caps["max_sessions_per_process"]==slots and caps["is_mock"] is False
                    assert caps["process_isolated"] == (workers > 1)
                    write(directory/"capabilities.json",caps)
                    for language in ("en", "id", "zh"):
                        body={"kind":"load","mode":"direct","calls":args.per_language,"concurrency":concurrency,
                            "languages":[language],"max_failure_rate":0}
                        jobs.append(run_suite(port,body,directory,case,language,"language",sources))
                        write(output/"jobs.json",jobs)
                        print(f"{case} {language}: {jobs[-1]['suite']['completed_calls']}/{len(jobs[-1]['calls'])} unique-WAV C++ calls completed",flush=True)
                    if workers == 2 and slots == 2:
                        body={"kind":"load","mode":"network","calls":4,"concurrency":4,"languages":["en","id","zh"],"max_failure_rate":0}
                        jobs.append(run_suite(port,body,directory,case,"mixed","network",sources))
                        write(output/"jobs.json",jobs)
                        print("shared_2w_2s: four mixed-WAV calls through C++ WebSocket ingress passed",flush=True)
                finally:
                    if process.poll() is None:
                        os.killpg(process.pid,signal.SIGTERM)
                        try:process.wait(timeout=75)
                        except subprocess.TimeoutExpired:
                            os.killpg(process.pid,signal.SIGKILL);process.wait()
            summarize(output,plan,jobs)
        result=summarize(output,plan,jobs)
        write(output/"status.json",{"status":result["status"]})
        hashes={str(p.relative_to(output)):digest(p) for p in sorted(output.rglob('*')) if p.is_file() and p.name!='checksums.json'}
        write(output/"checksums.json",hashes)
    except Exception as error:
        write(output/"status.json",{"status":"FAILED","error":str(error)})
        raise

if __name__ == "__main__":
    main()
