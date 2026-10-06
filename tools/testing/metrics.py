"""Timing field definitions and descriptive type-7 distributions for C++ artifacts."""
import math
import statistics

TIMINGS = ("startup_ns", "first_partial_ns", "first_usable_transcript_ns", "final_result_ns",
           "finalization_ns", "scheduled_final_lag_ns", "model_load_ns", "shared_model_load_ns",
           "live_invocation_wall_ns", "prefix_decode_wall_ns", "eof_refinement_wall_ns",
           "prefix_decode_queue_wait_ns", "eof_decode_queue_wait_ns", "runtime_queue_wait_ns",
           "offline_decode_wall_ns", "worker_cpu_ns", "first_stable_transcript_ns",
           "first_inference_compute_ns", "partial_service_lag_ns", "inference_compute_rtf")

def quantiles(values, unit="ns"):
    values = sorted(values)
    result = {"count": len(values), "population": "completed calls", "unit": unit,
              "estimator": "linear_(n-1)*p_type7"}
    for name, p in (("p50", .5), ("p90", .9), ("p95", .95), ("p99", .99)):
        pos = (len(values) - 1) * p
        result[name] = (values[math.floor(pos)] + (values[math.ceil(pos)] - values[math.floor(pos)]) *
                        (pos - math.floor(pos))) if values else None
    result["max"] = max(values) if values else None
    return result

def distribution(values, unit="ms", population="completed calls"):
    result = quantiles(values, unit)
    result["mean"] = statistics.mean(values) if values else None
    result["population"] = population
    return result
