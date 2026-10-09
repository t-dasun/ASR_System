#!/usr/bin/env python3
"""Five-WAV EN latency/accuracy sweep of main C++ resumable direct/network inference."""
import argparse
import csv
import json
from pathlib import Path
import random
import subprocess
import sys
import time
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.testing.capacity_stress import ResourceGuard, execute, stop, wait_ready, host_sample
from tools.testing.run_shared_pool_matrix import request, write, digest
from tools.testing.metrics import distribution

CASES = [(2000,200),(1000,200),(2000,100),(1000,100),(500,100)]
TIMES = ('first_partial_ns','first_usable_transcript_ns','final_result_ns','finalization_ns',
         'stream_decode_wall_ns','stream_decode_queue_wait_ns')

def summarize(output, plan, jobs):
    groups=[];per_wav=[]
    lines=['# English shared-runtime decode-step / chunk-size experiment','',
           f"Main C/C++ `qwen_stream`, {plan.get('mode', 'network')} mode. One worker, one active call at a time; eight session slots remain available in the shared runtime. No Python inference.",'',
           f"Same five distinct English WAVs, {plan['repetitions']} measured repetitions per setting. Four model threads, 32 new tokens/step, zero initially withheld chunks, no full-audio final refinement. Each round permutes setting order using seed 42. Warmup is separate.",'',
           'First text is first nonempty server result, not proof of semantic correctness. EOF delay is worker EOF receipt to final publication. Browser rendering/network impairment are not measured. WER uses completed calls only with failed calls reported separately.','',
           '| Decode step ms | PCM chunk ms | Complete | First text mean / p95 s | EOF mean / p95 s | Final mean s | Corpus WER % |','|---:|---:|---:|---|---|---:|---:|']
    def n(v):return '—' if v is None else f'{v:.3f}'
    for step,chunk in CASES:
        calls=[c for j in jobs if j['step_ms']==step and j['chunk_ms']==chunk for c in j['calls']]
        passed=[c for c in calls if c['status']=='COMPLETE']
        timings={k:distribution([c['measurements'][k]/1e9 for c in passed if c['measurements'].get(k) is not None], 'seconds') for k in TIMES}
        edits=sum(c['accuracy']['edits'] for c in passed);units=sum(c['accuracy']['reference_units'] for c in passed)
        group={'step_ms':step,'chunk_ms':chunk,'offered':len(calls),'completed':len(passed),'failures':len(calls)-len(passed),
               'timings':timings,'completed_wer':edits/units if units else None,'edits':edits,'reference_words':units,
               'contract_checks':all(all(c['checks'].values()) for c in calls),
               'pre_eof_text':sum(c['before_eof_text'] for c in passed)}
        groups.append(group)
        first,eof=timings['first_usable_transcript_ns'],timings['finalization_ns']
        lines.append(f"| {step} | {chunk} | {len(passed)}/{len(calls)} | {n(first['mean'])} / {n(first['p95'])} | {n(eof['mean'])} / {n(eof['p95'])} | {n(timings['final_result_ns']['mean'])} | {n(group['completed_wer']*100 if units else None)} |")
        for source in plan['recordings']:
            selected=[c for c in calls if c['recording_id']==source['id']];complete=[c for c in selected if c['status']=='COMPLETE']
            row={'recording_id':source['id'],'step_ms':step,'chunk_ms':chunk,'offered':len(selected),'completed':len(complete),
                 'mean_wer_percent':sum(c['accuracy']['rate'] for c in complete)/len(complete)*100 if complete else None}
            for key in TIMES:row[key+'_mean_seconds']=distribution([c['measurements'][key]/1e9 for c in complete if c['measurements'].get(key) is not None],'seconds')['mean']
            per_wav.append(row)
    lines+=['','## Per-WAV means','',
            'Each cell: **WER % / first-text seconds / EOF-to-final seconds**. Mean of completed repetitions; corpus WER above weights by reference words.','',
            '| Recording | '+' | '.join(f'{s} ms / {c} ms' for s,c in CASES)+' |', '|---|'+'---|'*len(CASES)]
    for source in plan['recordings']:
        cells=[]
        for step,chunk in CASES:
            r=next(x for x in per_wav if x['recording_id']==source['id'] and x['step_ms']==step and x['chunk_ms']==chunk)
            cells.append(' / '.join(n(r[k]) for k in ['mean_wer_percent','first_usable_transcript_ns_mean_seconds','finalization_ns_mean_seconds'])+f" ({r['completed']}/{r['offered']})")
        lines.append('| '+source['id']+' | '+' | '.join(cells)+' |')
    measured=[g for g in groups if g['timings']['first_usable_transcript_ns']['count']]
    if measured:
        fastest=min(measured,key=lambda g:g['timings']['first_usable_transcript_ns']['mean'])
        lines+=['','## Observed fastest first text','',f"Lowest measured mean first text: **{fastest['step_ms']} ms decode / {fastest['chunk_ms']} ms chunks**, {n(fastest['timings']['first_usable_transcript_ns']['mean'])} s, completed WER {n(fastest['completed_wer']*100)}%. This is a nonempty-output latency result; inspect final quality and EOF delay before choosing a setting."]
    lines+=['','## Scope and artifacts','',
            'Short read-speech sample: five selected files, not the full ten-file cohort or domain audio. Subsecond audio steps may increase encoder recomputation and provisional mistakes. No stable-word latency, batching gain or continuous-call capacity is claimed. Failed calls remain in calls.json and job artifacts; first/EOF/final distributions describe completions.',
            '', 'Raw C++ suite directories contain per-call configuration, transcript/events, audio/runtime timings and resource samples. Plan records WAV/model/binary/config/driver identities. summary.json retains timing means and p50/p95/p99; per_wav.csv provides individual file means.']
    write(output/'summary.json',{'plan':plan,'groups':groups,'per_wav':per_wav})
    write(output/'calls.json',[{'step_ms':j['step_ms'],'chunk_ms':j['chunk_ms'],'round':j['round'],**c} for j in jobs for c in j['calls']])
    if per_wav:
        with (output/'per_wav.csv').open('w',newline='') as f:
            w=csv.DictWriter(f,fieldnames=list(per_wav[0]),lineterminator='\n');w.writeheader();w.writerows(per_wav)
    (output/'report.md').write_text('\n'.join(lines)+'\n')
    return groups

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--repetitions',type=int,default=3)
    parser.add_argument('--mode',choices=('direct','network'),default='network')
    parser.add_argument('--report-only',action='store_true')
    parser.add_argument('--plan-only',action='store_true')
    args=parser.parse_args();output=args.output.resolve()
    if args.report_only:
        summarize(output,json.loads((output/'plan.json').read_text()),json.loads((output/'jobs.json').read_text()));return
    if not 1<=args.repetitions<=10:parser.error('repetitions must be 1..10')
    if output.exists():parser.error('output exists; choose a new directory')
    rows=[r for r in map(json.loads,(ROOT/'datasets/manifests/fleurs_validation.jsonl').read_text().splitlines()) if r['language']=='en'][:5]
    if len(rows)!=5 or len({r['sha256'] for r in rows})!=5:raise ValueError('need five distinct EN WAVs')
    for r in rows:
        if digest(ROOT/r['file'])!=r['sha256']:raise ValueError('WAV hash mismatch')
    output.mkdir(parents=True)
    manifest=output/'inputs.jsonl';manifest.write_text(''.join(json.dumps({**r,'file':str(ROOT/r['file'])})+'\n' for r in rows))
    cli=ROOT/'build/release-cpu/asr-cli';config=ROOT/'configs/qwen_stream_shared.yaml'
    schedule=[];rng=random.Random(42)
    for repetition in range(args.repetitions):
        order=CASES.copy();rng.shuffle(order);schedule.extend({'round':repetition+1,'step_ms':s,'chunk_ms':c} for s,c in order)
    plan={'runtime':'qwen_stream','mode':args.mode,'workers':1,'active_concurrency':1,'session_slots':8,
          'threads':4,'repetitions':args.repetitions,'cases':CASES,'schedule':schedule,'recordings':rows,
          'manifest_sha256':digest(manifest),'config_sha256':digest(config),'cli_sha256':digest(cli),
          'worker_sha256':digest(ROOT/'build/release-cpu/asr-prefix-worker'),'driver_sha256':digest(Path(__file__)),
          'model':json.loads((ROOT/'models/qwen3-asr-0.6b/acquisition.json').read_text()),'host':host_sample()}
    write(output/'plan.json',plan)
    if args.plan_only:write(output/'status.json',{'status':'PLANNED','measured_calls':25*args.repetitions});return
    command=[str(cli),'serve','--config',str(config),'--manifest',str(manifest),'--set','workers.processes=1',
             '--set','workers.max_sessions_per_process=8','--set',f'output.directory={output}','--port','0']
    write(output/'command.json',command);jobs=[];write(output/'jobs.json',jobs)
    write(output/'status.json',{'status':'RUNNING'});sources={r['id']:r for r in rows}
    with (output/'service.stdout').open('w') as out,(output/'service.stderr').open('w') as err:
        process=subprocess.Popen(command,cwd=ROOT,stdout=out,stderr=err,start_new_session=True)
        guard=ResourceGuard(process,output/'host_resources.jsonl',2*2**30,64*2**20)
        try:
            port=wait_ready(process,output/'service.stdout',guard);caps=request(port,'/v1/capabilities')
            write(output/'capabilities.json',caps)
            assert caps['streaming_kind']=='resumable_windowed_streaming' and caps['worker_processes']==1 and not caps['is_mock']
            warm=execute(port,{'kind':'load','mode':args.mode,'calls':1,'concurrency':1,'languages':['en']},output,'warmup',sources,guard)
            write(output/'warmup.json',warm)
            for item in schedule:
                step,chunk=item['step_ms'],item['chunk_ms'];label=f"r{item['round']}_step{step}_chunk{chunk}"
                body={'kind':'load','mode':args.mode,'calls':5,'concurrency':1,'languages':['en'],
                      'overrides':[f'model.decode_step_ms={step}',f'audio.chunk_ms={chunk}']}
                job=execute(port,body,output,label,sources,guard);job.update(item)
                for call in job['calls']:
                    cfg=json.loads((Path(call['directory'])/'config.json').read_text())
                    assert cfg['model']['runtime']=='qwen_stream' and cfg['model']['decode_step_ms']==step and cfg['audio']['chunk_ms']==chunk
                if job.get('peak_simultaneous_sessions',0)>1:raise RuntimeError('single-active-call contract violated')
                jobs.append(job);write(output/'jobs.json',jobs);summarize(output,plan,jobs)
                print(f"{label}: {sum(c['status']=='COMPLETE' for c in job['calls'])}/{len(job['calls'])} complete",flush=True)
            groups=summarize(output,plan,jobs)
            write(output/'status.json',{'status':'COMPLETE','calls':sum(g['offered'] for g in groups),
                'completed':sum(g['completed'] for g in groups),'failures':sum(g['failures'] for g in groups),
                'contract_checks':all(g['contract_checks'] for g in groups)})
        except BaseException as error:
            write(output/'status.json',{'status':'ABORTED_RESOURCE' if guard.error else 'FAILED','error':str(error)})
            raise
        finally:
            guard.close();stop(process)
            write(output/'checksums.json',{str(f.relative_to(output)):digest(f) for f in output.rglob('*') if f.is_file() and f.name!='checksums.json'})

if __name__=='__main__':main()
