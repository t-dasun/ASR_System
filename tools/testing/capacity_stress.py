"""Resource-guarded capacity curves using the main C++ service and WAV runner."""
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import signal
import statistics
import subprocess
import sys
import threading
import time

from tools.testing.metrics import distribution, TIMINGS
from tools.testing.scoring import aggregate
from tools.testing.run_shared_pool_matrix import ROOT, digest, read_calls, request, write

GIB = 2 ** 30
LANGUAGES = ("en", "id", "zh")


def concurrency_levels(workers, slots, dense=False):
    maximum = workers * slots
    if dense:
        return list(range(1, maximum + 1))
    return sorted({1, *[min(maximum, workers * scale) for scale in (1, 2, 4, 8)], maximum})


def balanced_calls(unique, concurrency):
    return unique * math.ceil(concurrency / unique)


def matrix_layouts(args):
    """(workers, configured slots per worker, measured total concurrency levels)."""
    if getattr(args, 'resumable_matrix', False):
        return [(1, slots, [slots]) for slots in range(1, 9)] + [
            (2, slots, [2 * slots]) for slots in (1, 2, 4, 8)]
    return [(workers, 8, concurrency_levels(workers, 8, args.dense))
            for workers in range(1, args.max_workers + 1)]


def resume_state(output, args, cli, config):
    """Validate experiment identity before permitting writes or new inference."""
    saved = json.loads((output/'curve.json').read_text())
    plan = saved['plan']
    if json.loads((output/'status.json').read_text()).get('status') == 'RUNNING':
        raise ValueError('run is marked RUNNING; do not resume while its driver may be active')
    if plan.get('matrix_mode') not in ('resumable_16_layouts', 'resumable_12_layouts'):
        raise ValueError('cannot resume: saved experiment is not a resumable matrix')
    expected = {'per_language':args.per_language,
                'repetitions':args.repetitions, 'reserve_bytes':int(args.reserve_gib*GIB),
                'config_sha256':digest(ROOT/config), 'cli_sha256':digest(cli),
                'worker_sha256':digest(ROOT/'build/release-cpu/asr-prefix-worker'),
                'manifest_sha256':digest(output/'inputs.jsonl')}
    for key, value in expected.items():
        if plan.get(key) != value:
            raise ValueError(f'cannot resume: {key} differs from saved experiment')
    jobs = json.loads((output/'jobs.json').read_text())
    layouts = matrix_layouts(args)
    selected = {(w,s) for w,s,_ in layouts}
    measured = [r for r in saved['worker_runs'] if r['status']=='MEASURED'
                and (r['workers'],r['slots_per_worker']) in selected]
    complete = {(r['workers'],r['slots_per_worker']) for r in measured}
    # Retain interrupted attempts in history and on disk, outside final curves.
    # Re-run their complete layout, avoiding partial-cohort double weighting.
    retained = [j for j in jobs if (j['workers'],j.get('slots_per_worker',8)) in complete]
    plan = dict(plan)
    plan.setdefault('original_layouts', plan['layouts'])
    plan['layouts'] = [{'workers':w,'slots_per_worker':s,'concurrency_levels':levels} for w,s,levels in layouts]
    plan['matrix_mode'] = 'resumable_12_layouts'
    plan['planned_curve_calls'] = sum(balanced_calls(args.per_language,c)*args.repetitions*3
                                     for _,_,levels in layouts for c in levels)
    return plan, retained, measured, saved['worker_runs'], jobs


def attempt_directory(output, workers, slots, resumable):
    base = output/(f'{workers}w_{slots}s' if resumable else f'{workers}w')
    directory = base
    attempt = 1
    while directory.exists():
        attempt += 1
        directory = base.with_name(f'{base.name}_attempt{attempt}')
    directory.mkdir()
    return directory


def host_sample(pid=None):
    memory = {}
    for line in Path('/proc/meminfo').read_text().splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == 'kB':
            memory[parts[0].rstrip(':')] = int(parts[1]) * 1024
    swap = {key: int(value) for key, value in
            (line.split() for line in Path('/proc/vmstat').read_text().splitlines())
            if key in ('pswpin', 'pswpout')}
    cpu = [int(v) for v in Path('/proc/stat').read_text().splitlines()[0].split()[1:9]]
    processes, pending, seen = [], [pid] if pid else [], set()
    while pending:
        item = pending.pop()
        if item in seen:
            continue
        seen.add(item)
        base = Path(f'/proc/{item}')
        try:
            stat = (base/'stat').read_text().split(') ')[-1].split()
            smaps = {}
            for line in (base/'smaps_rollup').read_text().splitlines():
                parts = line.split()
                if len(parts) == 3 and parts[2] == 'kB':
                    smaps[parts[0].rstrip(':')] = int(parts[1]) * 1024
            processes.append({'pid': item, 'start_ticks': int(stat[19]),
                              'cpu_ticks': int(stat[11]) + int(stat[12]),
                              'rss_bytes': int(stat[21]) * os.sysconf('SC_PAGE_SIZE'),
                              'pss_bytes': smaps.get('Pss'), 'swap_bytes':smaps.get('Swap',0),
                              'major_faults':int(stat[9]), 'threads': int(stat[17])})
            for task in (base/'task').iterdir():
                try:
                    pending.extend(int(v) for v in (task/'children').read_text().split())
                except OSError:
                    pass
        except (OSError, ValueError, IndexError):
            continue
    frequencies=[]
    for path in Path('/sys/devices/system/cpu').glob('cpu[0-9]*/cpufreq/scaling_cur_freq'):
        try: frequencies.append(int(path.read_text()))
        except (OSError,ValueError): pass
    temperatures={}
    for path in Path('/sys/class/hwmon').glob('hwmon*/temp*_input'):
        try:
            label=path.with_name(path.name.replace('_input','_label'))
            name=(path.parent/'name').read_text().strip()
            key=name+'/'+(label.read_text().strip() if label.exists() else path.stem)
            temperatures[key]=int(path.read_text())/1000
        except (OSError,ValueError): pass
    pressure={}
    try:
        for line in Path('/proc/pressure/memory').read_text().splitlines():
            parts=line.split()
            pressure[parts[0]]={key:float(value) for key,value in (part.split('=') for part in parts[1:])}
    except OSError:
        pass
    return {'timestamp_ns': time.monotonic_ns(), 'memory': memory,
            'memory_pressure':pressure,
            'cpu_mean_frequency_khz': statistics.mean(frequencies) if frequencies else None,
            'temperatures_c':temperatures, 'swap_pages': swap,
            'cpu_total_ticks': sum(cpu), 'cpu_idle_ticks': cpu[3] + cpu[4], 'processes': processes}


def stop(process):
    if process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()


class ResourceGuard:
    """Monitor the host and process tree, including model loading and idle periods."""
    def __init__(self, process, path, reserve, swap_limit):
        self.process, self.path = process, path
        self.reserve, self.swap_limit = reserve, swap_limit
        self.initial = host_sample()
        self.label, self.error = 'startup', None
        self.pressure_since=None
        self.closed = threading.Event()
        self.samples = []
        self.thread = threading.Thread(target=self.monitor, daemon=True)
        self.thread.start()

    def monitor(self):
        try:
            with self.path.open('w') as stream:
                while not self.closed.is_set():
                    sample = host_sample(self.process.pid)
                    sample['stage'] = self.label
                    self.samples.append(sample)
                    stream.write(json.dumps(sample) + '\n')
                    stream.flush()
                    available = sample['memory']['MemAvailable']
                    swap_delta = sum(max(0, sample['swap_pages'][key] - self.initial['swap_pages'][key])
                                     for key in ('pswpin', 'pswpout')) * os.sysconf('SC_PAGE_SIZE')
                    earlier=next((value for value in self.samples if value['timestamp_ns'] >= sample['timestamp_ns']-60*10**9),self.samples[0])
                    recent_swap=sum(max(0,sample['swap_pages'][key]-earlier['swap_pages'][key]) for key in ('pswpin','pswpout'))*os.sysconf('SC_PAGE_SIZE')
                    stalled=sample.get('memory_pressure',{}).get('full',{}).get('avg10',0) >= 10.0
                    if stalled:
                        self.pressure_since=self.pressure_since or time.monotonic()
                    else:
                        self.pressure_since=None
                    prolonged=bool(self.pressure_since and time.monotonic()-self.pressure_since >= 15)
                    swap_free=sample['memory'].get('SwapFree',self.reserve*2)
                    model_swap=sum(p.get('swap_bytes',0) for p in sample.get('processes',[]))
                    sample['swap_io_last_60s_bytes']=recent_swap
                    sample['swap_io_total_bytes']=swap_delta
                    sample['benchmark_swap_bytes']=model_swap
                    # Low host swap alone can come from unrelated applications.
                    # Require sustained pressure and benchmark swapping before
                    # treating swap headroom as a benchmark resource limit.
                    swap_exhaustion = swap_free < self.reserve and model_swap > 0 and prolonged
                    if available < self.reserve or swap_exhaustion or (recent_swap > self.swap_limit and prolonged):
                        self.error = f'resource guard: available={available/GIB:.2f} GiB, swap free={swap_free/GIB:.2f} GiB, benchmark swap={model_swap/2**20:.1f} MiB, prolonged stall={prolonged}'
                        stop(self.process)
                        break
                    self.closed.wait(1)
        except Exception as error:
            self.error = f'resource telemetry failed: {error}'
            stop(self.process)

    def check(self):
        if self.error:
            raise RuntimeError(self.error)
        if self.process.poll() is not None:
            raise RuntimeError(f'service exited with {self.process.returncode}')

    def close(self):
        self.closed.set()
        self.thread.join(timeout=20)


def wait_ready(process, stdout, guard):
    deadline = time.monotonic() + 240
    while time.monotonic() < deadline:
        guard.check()
        for line in stdout.read_text().splitlines():
            try:
                value = json.loads(line)
                if 'port' in value:
                    return value['port']
            except json.JSONDecodeError:
                pass
        time.sleep(.2)
    raise RuntimeError('model/service startup exceeded 240 seconds')


def execute(port, body, directory, label, sources, guard, slots=8):
    guard.label = label
    write(directory/f'{label}_request.json', body)
    planned = request(port, '/v1/suites/dry-run', body)
    write(directory/f'{label}_plan.json', planned)
    if not planned['allowed']:
        return {'label': label, 'request': body, 'status': 'SKIPPED_PREFLIGHT',
                'preflight': planned['preflight'], 'calls': []}
    job = request(port, '/v1/suites', body)
    snapshots = []
    deadline = time.monotonic() + 7200
    while time.monotonic() < deadline:
        guard.check()
        state = request(port, f"/v1/jobs/{job['job_id']}")
        runtime = request(port, '/v1/runtime')
        snapshots.append(runtime)
        if state['status'] != 'RUNNING':
            break
        time.sleep(1)
    else:
        raise RuntimeError(f'{label} exceeded two hours')
    write(directory/f'{label}_runtime.json', snapshots)
    write(directory/f'{label}_job.json', state)
    if 'result' not in state:
        raise RuntimeError(f'{label}: job failed without artifacts: {state}')
    if snapshots:
        assert all(all(w['active_sessions'] <= slots and w['runtime_threads'] == w['blas_threads'] == 4
                       for w in sample['workers']) for sample in snapshots), 'worker/thread/slot contract changed'
    suite = state['result']
    calls = read_calls(suite, sources)
    # Preserve failed calls as measurements; data-contract failures are independent.
    return {'label': label, 'request': body, 'status': state['status'], 'suite': suite, 'calls': calls,
            'peak_simultaneous_sessions': max(sum(w['active_sessions'] for w in s['workers']) for s in snapshots),
            'peak_sessions_per_worker': [max(s['workers'][i]['active_sessions'] for s in snapshots)
                                         for i in range(len(snapshots[0]['workers']))]}


def analyze_job(job, workers):
    calls, suite = job['calls'], job['suite']
    completed = [c for c in calls if c['status'] == 'COMPLETE']
    wall = suite['metrics']['measurement_wall_seconds']
    timings = {key: distribution([c['measurements'][key] / 1e6 for c in completed
                                  if c['measurements'].get(key) is not None])
               for key in TIMINGS if key.endswith('_ns')}
    timings.update({key: distribution([c['measurements'][key] for c in completed
                                       if c['measurements'].get(key) is not None], 'ratio')
                    for key in ('effective_rtf', 'offline_decode_wall_rtf', 'stream_invocation_wall_rtf')})
    for key in ('stream_decode_steps', 'stream_reused_prefill_tokens'):
        timings[key] = distribution([c['measurements'][key] for c in completed
                                    if c['measurements'].get(key) is not None], 'count')
    samples = []
    for phase in suite['phases']:
        samples.extend(json.loads(line) for line in
                       (Path(suite['directory'])/phase['resource_samples_path']).read_text().splitlines())
    cpu_seconds, sampled_wall = 0., 0.
    for before, after in zip(samples, samples[1:]):
        seconds = (after['timestamp_ns'] - before['timestamp_ns']) / 1e9
        # Separate repetitions can have unsampled gaps: use only adjacent samples within a second.
        if 0 < seconds < 1:
            sampled_wall += seconds
            cpu_seconds += seconds * sum(p['cpu_core_equivalents'] or 0 for p in after['processes'])
    host_cpu = [s['cpu']['cpu']['utilization_percent'] for s in samples if s['cpu']['cpu']['utilization_percent'] is not None]
    scores = aggregate(calls)
    language_groups = {}
    for language in LANGUAGES:
        group = [c for c in calls if c['language'] == language]
        successful = [c for c in group if c['status'] == 'COMPLETE']
        edits = sum(c['accuracy']['edits'] for c in group)
        units = sum(c['accuracy']['reference_units'] for c in group)
        language_groups[language] = {'calls':len(group), 'completed':len(successful),
            'metric':'CER' if language=='zh' else 'WER', 'accuracy_rate':edits/units if units else None,
            'completed_only':scores[language]['completed_only'],
            'offered_failure_inclusive':scores[language]['offered_failure_inclusive'],
            'first_text_ms':distribution([c['measurements']['first_usable_transcript_ns']/1e6
                for c in successful if c['measurements'].get('first_usable_transcript_ns') is not None]),
            'eof_ms':distribution([c['measurements']['finalization_ns']/1e6
                for c in successful if c['measurements'].get('finalization_ns') is not None])}
    edits = sum(c['accuracy']['edits'] for c in calls)
    units = sum(c['accuracy']['reference_units'] for c in calls)
    return {'workers': workers, 'slots_per_worker': job.get('slots_per_worker', 8),
            'target_calls_per_worker': job['request']['concurrency'] / workers,
            'language': job['request']['languages'][0] if len(job['request']['languages']) == 1 else 'mixed',
            'concurrency': job['request']['concurrency'], 'mode': job['request']['mode'],
            'calls': len(calls), 'unique_wavs': len({c['recording_id'] for c in calls}),
            'completed': len(completed), 'failures': len(calls)-len(completed),
            'failure_rate': 1-len(completed)/len(calls),
            'pre_eof_text': sum(c['before_eof_text'] for c in completed),
            'all_contract_checks': all(all(c['checks'].values()) for c in calls),
            'measurement_failures': suite.get('measurement_failures', 0), 'timings': timings,
            'language_groups':language_groups, 'accuracy': {'metric': ('CER' if job['request']['languages']==['zh'] else 'WER') if len(job['request']['languages'])==1 else 'mixed (not aggregated)',
                         'edits': edits, 'units': units, 'rate': edits/units if units and len(job['request']['languages'])==1 else None,
                         'completed_only': scores[job['request']['languages'][0]]['completed_only'] if len(job['request']['languages'])==1 else None,
                         'offered_failure_inclusive': scores[job['request']['languages'][0]]['offered_failure_inclusive'] if len(job['request']['languages'])==1 else None},
            'audio_seconds_per_wall_second': suite['metrics']['completed_audio_seconds']/wall,
            'calls_per_second': len(completed)/wall, 'wall_seconds': wall,
            'sampled_mean_cpu_cores': cpu_seconds/sampled_wall if sampled_wall else None,
            'sampled_peak_cpu_cores': max(p['resources']['max_tree_cpu_core_equivalents'] or 0 for p in suite['phases']),
            'sampled_peak_rss_bytes': max(p['resources']['sampled_peak_tree_rss_bytes'] for p in suite['phases']),
            'sampled_peak_pss_bytes': max(p['resources']['sampled_peak_tree_pss_bytes'] or 0 for p in suite['phases']),
            'host_mean_cpu_percent': statistics.mean(host_cpu) if host_cpu else None,
            'host_min_available_memory_bytes': min(s['host_memory_bytes']['MemAvailable'] for s in samples),
            'peak_simultaneous_sessions': job['peak_simultaneous_sessions'],
            'peak_sessions_per_worker': job['peak_sessions_per_worker']}


def load_job(job):
    return json.loads(Path(job['record_path']).read_text()) if 'record_path' in job else job


def save_job(job, directory):
    path=directory/f"{job['label']}_record.json"
    write(path,job)
    return {'workers':job['workers'],'slots_per_worker':job.get('slots_per_worker',8),'kind':job['kind'],'label':job['label'],
            'status':job['status'],'record_path':str(path),
            'suite_directory':job.get('suite',{}).get('directory'),
            'calls':len(job['calls']),'completed':sum(c['status']=='COMPLETE' for c in job['calls'])}


def report(output, plan, jobs, worker_runs):
    # Keep only per-point summaries in RAM; raw call records stay beside each job.
    # Otherwise long sweeps would consume extra host RAM as artifacts accumulate.
    baseline={}
    for entry in jobs:
        if entry.get('kind')=='curve' and entry['workers']==1 and entry['label'].startswith('c1_'):
            job=load_job(entry)
            for call in job['calls']:
                if call['status']=='COMPLETE':
                    baseline.setdefault((call['language'],call['recording_id']),set()).add(call['transcript'])
    points=[]
    for entry in jobs:
        if entry.get('kind') not in ('curve','network','soak'):
            continue
        job=load_job(entry)
        if not job.get('suite'):
            continue
        point=analyze_job(job,job['workers'])
        eligible=[c for c in job['calls'] if c['status']=='COMPLETE' and (c['language'],c['recording_id']) in baseline]
        point['kind']=job['kind']
        point['baseline_comparable_calls']=len(eligible)
        point['exact_final_matches_1w_c1']=sum(c['transcript'] in baseline[(c['language'],c['recording_id'])] for c in eligible)
        errors={}
        for call in job['calls']:
            if call['status']!='COMPLETE':
                key=str(call.get('failure') or 'failure detail in call artifacts')
                errors[key]=errors.get(key,0)+1
        point['failure_reasons']=errors
        points.append(point)
    write(output/'curve.json',{'plan':plan,'worker_runs':worker_runs,'points':points})
    flat = []
    for point in points:
        row = {key: value for key, value in point.items() if not isinstance(value, (dict, list))}
        for key, value in point['timings'].items():
            for statistic in ('mean', 'p50', 'p95', 'max'):
                row[f'{key}_{statistic}'] = value[statistic]
        row['accuracy_rate'] = point['accuracy']['rate']  # legacy failure-inclusive field
        row['accuracy_completed_only_rate'] = (point['accuracy']['completed_only'] or {}).get('rate')
        row['accuracy_failure_inclusive_rate'] = point['accuracy']['rate']
        flat.append(row)
    if flat:
        with (output/'curve.csv').open('w') as stream:
            writer = csv.DictWriter(stream, fieldnames=flat[0].keys())
            writer.writeheader(); writer.writerows(flat)
    def number(value, scale=1):
        return '—' if value is None else f'{value/scale:.3f}'
    lines = ['# CPU capacity stress report', '',
             'Main C++ service and WAV simulator; Python orchestrates and scores. No latency pass/fail target.', '',
             f"Same {plan['per_language']} unique WAVs per language, {plan['repetitions']} measured repetitions per curve point. Larger targets replay full balanced cohorts to fill concurrency. Languages are separate; failures are retained.", '',
             ('Resumable per-call encoder/decoder state shares model weights. Each worker runs one ready decode step at a time and requeues the call. No multi-call tensor batching. Decode step 2 seconds, zero initially withheld chunks, final whole-audio refinement disabled.'
              if plan.get('runtime') == 'qwen_stream' else
              'Worker sessions share a serial prefix/EOF context; this is not independent incremental streaming or batching.') + ' Four compute threads per worker. Model weights are unchanged.', '',
             f"Runtime: `{plan.get('runtime', 'qwen_prefix')}`. Config: `{plan.get('config', 'configs/qwen_prefix_shared.yaml')}`.", '',
             '## Worker admission and idle cost', '',
             '| Workers | Slots/worker | Status | Idle RSS GiB | Idle PSS GiB | Reason |', '|---:|---:|---|---:|---:|---|']
    for run in worker_runs:
        idle = run.get('idle', {})
        lines.append(f"| {run['workers']} | {run.get('slots_per_worker',8)} | {run['status']} | {number(idle.get('rss_bytes'), GIB)} | {number(idle.get('pss_bytes'), GIB)} | {run.get('error', '')} |")
    for language in LANGUAGES:
        lines += ['', f'## {language}', '',
                  '| Workers | Slots/worker | Concurrency | Complete | Failure % | First text mean/p95 s | EOF mean/p95 s | Audio s/wall s | CPU mean/peak cores | Peak PSS GiB | Completed WER/CER % | Failure-inclusive WER/CER % |',
                  '|---:|---:|---:|---:|---:|---|---|---:|---|---:|---:|---:|']
        for p in points:
            if p['language'] != language or p['mode'] != 'direct':
                continue
            first, eof = p['timings']['first_usable_transcript_ns'], p['timings']['finalization_ns']
            lines.append(f"| {p['workers']} | {p['slots_per_worker']} | {p['concurrency']} | {p['completed']}/{p['calls']} | {p['failure_rate']*100:.1f} | {number(first['mean'],1000)} / {number(first['p95'],1000)} | {number(eof['mean'],1000)} / {number(eof['p95'],1000)} | {number(p['audio_seconds_per_wall_second'])} | {number(p['sampled_mean_cpu_cores'])} / {number(p['sampled_peak_cpu_cores'])} | {number(p['sampled_peak_pss_bytes'],GIB)} | {number(p['accuracy']['completed_only']['rate'],.01)} | {number(p['accuracy']['rate'],.01)} |")
        if plan.get('runtime') == 'qwen_stream':
            lines += ['', '### Resumable decoding (completed-call means)', '',
                      '| Workers | Slots/worker | Stream wall ms | Stream queue ms | Steps | Reused prefill tokens | Stream invocation RTF |',
                      '|---:|---:|---:|---:|---:|---:|---:|']
            for p in points:
                if p['language'] == language and p['kind'] == 'curve':
                    keys = ('stream_decode_wall_ns', 'stream_decode_queue_wait_ns', 'stream_decode_steps',
                            'stream_reused_prefill_tokens', 'stream_invocation_wall_rtf')
                    values = ' | '.join(number(p['timings'][key]['mean']) for key in keys)
                    lines.append(f"| {p['workers']} | {p['slots_per_worker']} | {values} |")
    lines += ['', '## Observed throughput maxima (no latency acceptance target)', '',
              '| Language | Workers | Concurrency | Audio s/wall s | Failure % |', '|---|---:|---:|---:|---:|']
    for language in LANGUAGES:
        choices = [p for p in points if p['language']==language and p['mode']=='direct']
        if choices:
            best = max(choices, key=lambda p: p['audio_seconds_per_wall_second'])
            lines.append(f"| {language} | {best['workers']} | {best['concurrency']} | {number(best['audio_seconds_per_wall_second'])} | {best['failure_rate']*100:.1f} |")
    lines += ['', '## Mixed-language network and sustained tests', '',
              '| Workers | Concurrency | Mode | Language | Complete | First text mean/p95 s | EOF mean/p95 s | Completed WER/CER % | Failure-inclusive WER/CER % |',
              '|---:|---:|---|---|---:|---|---|---:|---:|']
    for point in points:
        if point['kind'] == 'curve':
            continue
        for language, group in point['language_groups'].items():
            first,eof=group['first_text_ms'],group['eof_ms']
            lines.append(f"| {point['workers']} | {point['concurrency']} | {point['kind']} | {language} | {group['completed']}/{group['calls']} | {number(first['mean'],1000)} / {number(first['p95'],1000)} | {number(eof['mean'],1000)} / {number(eof['p95'],1000)} | {number(group['completed_only']['rate'],.01)} | {number(group['accuracy_rate'],.01)} |")
    lines += ['', 'These are maximum observed throughput points, not maximum usable capacity. No production latency/quality SLO was selected.', '',
              '## Scope and interpretation', '',
              '- First text can be partial or final; EOF delay is worker EOF receipt to final publication. Completed-call timing distributions exclude failed calls. Primary WER/CER uses completed calls only: total substitutions + deletions + insertions divided by their total reference words (WER) or characters (CER). Failures are reported separately; supplementary failure-inclusive accuracy uses empty hypotheses for failures. Completion is a protocol outcome, not an accuracy threshold. Excluding failures can make accuracy look better when difficult files fail.',
              '- Effective RTF includes audio pacing. Stream invocation wall RTF sums resumable step wall times and optional refinement, divided by input audio duration; it excludes scheduler queue waiting. Legacy offline wall RTF is unavailable for resumable mode. Neither is vendor active compute. Ten distinct files give descriptive tails, not a production p95 guarantee.',
              '- PSS apportions shared physical pages; summed RSS double-counts some mapped weights. Host swap is system-wide and can include unrelated programs. Resource guards reserve RAM and bound swap I/O; an aborted run is not a model accuracy failure.',
              '- Cold CLI conservative plans remain recorded. Worker services load progressively under live resource monitoring; warm suite preflight remains enforced. A failed cold estimate is not silently labelled successful cold admission.',
              '- Maximum configured support is bounded to eight workers and eight sessions/worker. The load runner supports 64 calls so all configured slots can be exercised. Untested higher limits are not hardware capacity claims.',
              '- Finite paced WAV calls use completion-driven replenishment. Target concurrency differs from achieved occupancy; peaks and worker occupancy are recorded. This is not a continuous telephony arrival model.',
              '- Raw jobs, call transcripts, resource samples, runtime snapshots, guard telemetry, plans and hashes are retained beside curve.csv/curve.json.']
    (output/'report.md').write_text('\n'.join(lines)+'\n')
    return points


def run(args):
    output = args.output.resolve()
    resume = getattr(args, 'resume', False)
    resumable = getattr(args, 'resumable_matrix', False)
    cli = ROOT/'build/release-cpu/asr-cli'
    config = 'configs/qwen_stream_shared.yaml' if resumable else 'configs/qwen_prefix_shared.yaml'
    layouts = matrix_layouts(args)
    jobs, worker_runs = [], []
    manifest = output/'inputs.jsonl'
    if resume:
        plan, jobs, worker_runs, previous_runs, previous_jobs = resume_state(output, args, cli, config)
        rows = [json.loads(line) for line in manifest.read_text().splitlines()]
    else:
        if output.exists():
            raise ValueError('output exists; use --resume for an interrupted resumable matrix or a new directory')
        output.mkdir(parents=True)
        rows = [json.loads(line) for line in (ROOT/'datasets/manifests/fleurs_validation.jsonl').read_text().splitlines()]
        selected = {lang: [r for r in rows if r['language']==lang][:args.per_language] for lang in LANGUAGES}
        if any(len(v)!=args.per_language or len({r['id'] for r in v})!=args.per_language for v in selected.values()):
            raise ValueError('insufficient unique WAV inputs')
        rows = [selected[lang][i] for i in range(args.per_language) for lang in LANGUAGES]
    sources = {r['id']: r for r in rows}
    for r in rows:
        if digest(ROOT/r['file']) != r['sha256']:
            raise ValueError(f"WAV hash mismatch: {r['id']}")
    if not resume:
        manifest.write_text(''.join(json.dumps({**r,'file':str(ROOT/r['file'])},ensure_ascii=False)+'\n' for r in rows))
    if not resume:
        plan = {'per_language':args.per_language, 'repetitions':args.repetitions,
                'layouts':[{'workers':w,'slots_per_worker':s,'concurrency_levels':levels} for w,s,levels in layouts],
                'slots_per_worker':list(range(1,9)) if resumable else 8, 'threads_per_worker':4,
                'runtime':'qwen_stream' if resumable else 'qwen_prefix', 'config':config,
                'config_sha256':digest(ROOT/config), 'matrix_mode':'resumable_12_layouts' if resumable else 'capacity_stress',
                'reserve_bytes':int(args.reserve_gib*GIB), 'swap_io_limit_bytes':64*2**20,
                'soak_seconds':args.soak_seconds, 'soak_worker_layout':'largest measured layout', 'dense':args.dense,
                'planned_curve_calls':sum(balanced_calls(args.per_language,c)*args.repetitions*3 for _,_,values in layouts for c in values),
                'host':host_sample(), 'lscpu':subprocess.check_output(['lscpu','--json'],text=True),
                'git_head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
                'cli_sha256':digest(cli),'worker_sha256':digest(ROOT/'build/release-cpu/asr-prefix-worker'),
                'driver_sha256':digest(Path(__file__)),
                'matrix_driver_sha256':digest(ROOT/'tools/testing/run_shared_pool_matrix.py'),
                'metrics_sha256':digest(ROOT/'tools/testing/metrics.py'),
                'git_diff_sha256':hashlib.sha256(subprocess.check_output(['git','diff','HEAD'],cwd=ROOT)).hexdigest(), 'manifest_sha256':digest(manifest),
                'model':json.loads((ROOT/'models/qwen3-asr-0.6b/acquisition.json').read_text())}
        write(output/'plan.json',plan)
    print(f"Capacity plan: {sum(len(levels) for _,_,levels in layouts)} curve points, {plan['planned_curve_calls']} calls, plus warmups/network/soak",flush=True)
    if args.plan_only:
        write(output/'status.json',{'status':'PLANNED'}); return
    if resume:
        history = output/'resume_history'
        history.mkdir(exist_ok=True)
        index = len(list(history.glob('attempt_*.json'))) + 1
        write(history/f'attempt_{index}.json', {'previous_status':json.loads((output/'status.json').read_text()),
              'previous_plan':json.loads((output/'curve.json').read_text())['plan'],
              'effective_layouts':plan['layouts'],
              'previous_worker_runs':previous_runs, 'previous_jobs':previous_jobs,
              'driver_sha256':digest(Path(__file__)), 'host':host_sample(),
              'guard_policy':'RAM reserve; sustained swap pressure, not low host swap alone'})
    write(output/'jobs.json',jobs)
    write(output/'status.json',{'status':'RUNNING'})
    try:
        for layout_index, (workers, slots, levels) in enumerate(layouts):
            if any(r['workers']==workers and r.get('slots_per_worker',8)==slots and r['status']=='MEASURED' for r in worker_runs):
                print(f'Skipping completed {workers}w {slots}s', flush=True)
                continue
            directory=attempt_directory(output, workers, slots, resumable)
            run_info={'workers':workers,'slots_per_worker':slots,'directory':str(directory),'status':'STARTING'}; worker_runs.append(run_info)
            common=['--config',config,'--manifest',str(manifest),
                    '--set',f'workers.processes={workers}','--set',f'workers.max_sessions_per_process={slots}',
                    '--set',f'output.directory={directory}']
            cold=subprocess.run([str(cli),'load-dry-run',*common,'--calls','40','--concurrency',str(workers*slots),'--languages','en'],cwd=ROOT,capture_output=True,text=True,check=True)
            (directory/'cold_plan.json').write_text(cold.stdout)
            if host_sample()['memory']['MemAvailable'] < plan['reserve_bytes']+GIB:
                run_info.update(status='SKIPPED_RESOURCE',error='insufficient RAM headroom before startup')
                worker_runs.extend({'workers':w,'slots_per_worker':s,'status':'NOT_RUN_RESOURCE','error':'earlier startup headroom check stopped escalation'} for w,s,_ in layouts[layout_index+1:])
                report(output,plan,jobs,worker_runs); break
            command=[str(cli),'serve',*common,'--port','0']
            write(directory/'command.json',command)
            with (directory/'service.stdout').open('w') as stdout, (directory/'service.stderr').open('w') as stderr:
                process=subprocess.Popen(command,cwd=ROOT,stdout=stdout,stderr=stderr,start_new_session=True)
                guard=ResourceGuard(process,directory/'host_resources.jsonl',plan['reserve_bytes'],plan['swap_io_limit_bytes'])
                try:
                    port=wait_ready(process,directory/'service.stdout',guard)
                    capabilities=request(port,'/v1/capabilities')
                    assert capabilities['worker_processes']==workers and capabilities['max_sessions_per_process']==slots
                    assert not capabilities['is_mock']
                    if resumable:
                        assert capabilities['streaming_kind']=='resumable_windowed_streaming', capabilities
                        assert capabilities['refine_final'] is False and capabilities['stream_unfixed_chunks']==0, capabilities
                    write(directory/'capabilities.json',capabilities)
                    guard.label='idle'; time.sleep(5); guard.check()
                    idle=host_sample(process.pid)
                    run_info['idle']={'rss_bytes':sum(p['rss_bytes'] for p in idle['processes']),
                                      'pss_bytes':sum(p['pss_bytes'] or 0 for p in idle['processes']),
                                      'sample':idle}
                    warm=execute(port,{'kind':'load','mode':'direct','calls':workers,'concurrency':workers,
                                       'languages':list(LANGUAGES)},directory,'warmup',sources,guard,slots=slots)
                    warm.update(workers=workers,slots_per_worker=slots,kind='warmup'); jobs.append(save_job(warm,directory)); del warm
                    for concurrency in levels:
                        for language in LANGUAGES:
                            body={'kind':'load','mode':'direct','calls':balanced_calls(args.per_language,concurrency),
                                  'concurrency':concurrency,'repetitions':args.repetitions,'languages':[language]}
                            job=execute(port,body,directory,f'c{concurrency}_{language}',sources,guard,slots=slots)
                            job.update(workers=workers,slots_per_worker=slots,kind='curve'); jobs.append(save_job(job,directory))
                            write(output/'jobs.json',jobs); report(output,plan,jobs,worker_runs)
                            print(f"{workers}w c{concurrency} {language}: {sum(c['status']=='COMPLETE' for c in job['calls'])}/{len(job['calls'])} completed",flush=True)
                    # Keep previous mixed WebSocket coverage only at each worker count's maximum.
                    if slots == 8:
                        body={'kind':'load','mode':'network','calls':balanced_calls(len(rows),workers*slots),
                              'concurrency':workers*slots,'languages':list(LANGUAGES)}
                        job=execute(port,body,directory,'network_max',sources,guard,slots=slots)
                        job.update(workers=workers,slots_per_worker=slots,kind='network'); jobs.append(save_job(job,directory)); del job
                    run_info['status']='INCOMPLETE_PREFLIGHT' if any(
                        j['status']=='SKIPPED_PREFLIGHT' and Path(j['record_path']).parent==directory for j in jobs
                    ) else 'MEASURED'
                except Exception as error:
                    run_info['interrupted_stage']=guard.label
                    run_info.update(status='ABORTED_RESOURCE' if guard.error else 'ERROR',error=str(error))
                    if not guard.error:
                        raise
                finally:
                    guard.close(); stop(process)
                    known={Path(j['suite_directory']) for j in jobs if j.get('suite_directory')}
                    incomplete=[]
                    for suite_dir in directory.glob('load_*'):
                        if suite_dir in known:
                            continue
                        counts={}
                        for status in suite_dir.glob('*/status.json'):
                            try:
                                state=json.loads(status.read_text()).get('status','UNKNOWN')
                                counts[state]=counts.get(state,0)+1
                            except (OSError,json.JSONDecodeError): pass
                        if counts: incomplete.append({'directory':str(suite_dir),'call_states':counts})
                    if incomplete: run_info['incomplete_runs']=incomplete
            write(output/'jobs.json',jobs); report(output,plan,jobs,worker_runs)
            if run_info['status']=='ABORTED_RESOURCE':
                worker_runs.extend({'workers':w,'slots_per_worker':s,'status':'NOT_RUN_RESOURCE','error':'earlier resource guard stopped escalation'} for w,s,_ in layouts[layout_index+1:])
                break
        successful=[r['workers'] for r in worker_runs if r['status']=='MEASURED']
        if args.soak_seconds and successful:
            workers=max(successful)
            directory=output/f'soak_{workers}w'; directory.mkdir()
            command=[str(cli),'serve','--config','configs/qwen_prefix_shared.yaml','--manifest',str(manifest),
                     '--set',f'workers.processes={workers}','--set','workers.max_sessions_per_process=8',
                     '--set',f'output.directory={directory}','--port','0']
            write(directory/'command.json',command)
            with (directory/'service.stdout').open('w') as stdout, (directory/'service.stderr').open('w') as stderr:
                process=subprocess.Popen(command,cwd=ROOT,stdout=stdout,stderr=stderr,start_new_session=True)
                guard=ResourceGuard(process,directory/'host_resources.jsonl',plan['reserve_bytes'],plan['swap_io_limit_bytes'])
                soak_info={'workers':workers,'status':'STARTING'}
                try:
                    port=wait_ready(process,directory/'service.stdout',guard)
                    body={'kind':'load','mode':'network','calls':balanced_calls(len(rows),workers*8),
                          'concurrency':workers*8,'languages':list(LANGUAGES)}
                    deadline=time.monotonic()+args.soak_seconds; index=0
                    while time.monotonic()<deadline:
                        job=execute(port,body,directory,f'soak_{index}',sources,guard)
                        job.update(workers=workers,kind='soak'); jobs.append(save_job(job,directory))
                        index+=1; write(output/'jobs.json',jobs); report(output,plan,jobs,worker_runs)
                        print(f"Sustained {workers}w c{workers*8}: batch {index}, {sum(c['status']=='COMPLETE' for c in job['calls'])}/{len(job['calls'])} completed",flush=True)
                    soak_info.update(status='MEASURED',batches=index)
                except Exception as error:
                    soak_info.update(status='ABORTED_RESOURCE' if guard.error else 'ERROR',error=str(error))
                    if not guard.error: raise
                finally:
                    guard.close(); stop(process)
                write(output/'soak_status.json',soak_info)
        points=report(output,plan,jobs,worker_runs)
        failures=sum(j['calls']-j['completed'] for j in jobs)
        write(output/'status.json',{'status':'COMPLETE_WITH_RESOURCE_LIMIT' if any(r['status']!='MEASURED' for r in worker_runs) else 'COMPLETE',
                                   'call_failures':failures,'curve_points':len(points)})
        write(output/'checksums.json',{str(p.relative_to(output)):digest(p) for p in sorted(output.rglob('*')) if p.is_file() and p.name!='checksums.json'})
    except BaseException as error:
        write(output/'status.json',{'status':'FAILED','error':str(error)}); raise
