"""Conservative analysis of M6 native load suites; no automatic node sizing."""
import json
import hashlib
from pathlib import Path


def analyze_screens(directories):
    """Validate comparable 1/2-worker screens and return observations, not capacity."""
    rows = []
    fingerprints = set()
    specifications = set()
    fixture_hashes = set()
    for directory in directories:
        directory = Path(directory)
        plan = json.loads((directory / "plan.json").read_text())
        status = json.loads((directory / "status.json").read_text())
        summary = json.loads((directory / "summary.json").read_text())
        spec = plan["spec"]
        comparable_spec = dict(spec)
        comparable_spec.pop("concurrency")
        specifications.add(json.dumps(comparable_spec, sort_keys=True))
        metrics = summary["metrics"]
        if (status.get("status") != "COMPLETE" or summary.get("status") != "COMPLETE"
                or summary.get("is_mock") or summary.get("measurement_failures") != 0
                or metrics["offered_calls"] != spec["calls"] * spec["repetitions"]
                or len(summary["phases"]) != spec["warmups"] + spec["repetitions"]):
            raise ValueError(f"invalid capacity screen: {directory}")
        calls = [call for phase in summary["phases"] if not phase["warmup"] for call in phase["calls"]]
        if len(calls) != metrics["offered_calls"] or any(call["status"] != "COMPLETE" for call in calls):
            raise ValueError(f"incomplete call artifacts: {directory}")
        configs = sorted(directory.glob("*/config.json"))
        if len(configs) != metrics["offered_calls"]:
            raise ValueError(f"missing call configurations: {directory}")
        for path in configs:
            config = json.loads(path.read_text())
            fixture_path = Path(config["audio"]["path"])
            if not fixture_path.is_file():
                raise ValueError(f"fixture missing: {fixture_path}")
            fixture_hashes.add(hashlib.sha256(fixture_path.read_bytes()).hexdigest())
            fingerprints.add(json.dumps({"model": config["model"],
                                         "audio": {key: config["audio"][key] for key in
                                                   ("path", "chunk_ms", "effective_samples", "realtime_pacing")}},
                                        sort_keys=True))
        if spec["mode"] != "direct" or spec["languages"] != ["en"]:
            raise ValueError("screen comparison expects direct English fixture")
        rows.append({"run_id": directory.name, "path": str(directory),
                     "concurrency": spec["concurrency"], "calls": metrics["offered_calls"],
                     "completed_calls": metrics["completed_calls"],
                     "diagnostic_slo_qualified": summary["slo"]["qualified"],
                     "diagnostic_slo": {"max_failure_rate": spec["max_failure_rate"],
                                        "max_p95_send_lag_ms": spec["max_p95_send_lag_ms"]},
                     "wall_seconds": metrics["measurement_wall_seconds"],
                     "audio_seconds_per_wall_second": metrics["audio_seconds_per_wall_second"],
                     "first_usable_p95_ms": metrics["first_usable_ms"]["p95"],
                     "final_p95_ms": metrics["final_result_ms"]["p95"],
                     "effective_rtf_p95": metrics["effective_rtf"]["p95"],
                     "send_lag_p95_ms": metrics["call_max_send_lag_ms"]["p95"],
                     "sampled_peak_tree_rss_bytes": metrics["sampled_peak_tree_rss_bytes"]})
    rows.sort(key=lambda row: row["concurrency"])
    if ([row["concurrency"] for row in rows] != [1, 2] or len(fingerprints) != 1
            or len(specifications) != 1 or len(fixture_hashes) != 1):
        raise ValueError("expected one comparable 1/2-worker curve")
    return {"status": "DIAGNOSTIC_SCREEN_ONLY", "rows": rows,
            "fixture_sha256": fixture_hashes.pop(),
            "throughput_ratio_2_vs_1": rows[1]["audio_seconds_per_wall_second"] /
                                        rows[0]["audio_seconds_per_wall_second"],
            "first_p95_delta_ms_2_vs_1": rows[1]["first_usable_p95_ms"] - rows[0]["first_usable_p95_ms"],
            "safe_legs_per_node": None,
            "reason": "One short English fixture, 20 calls/point, <1 minute/point, no failing knee, "
                      "no sustained concurrent multi-language calls, no production SLO or headroom. "
                      "M6 diagnostic SLO qualification does not qualify production capacity."}
