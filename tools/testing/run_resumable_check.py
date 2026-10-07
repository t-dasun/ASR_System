#!/usr/bin/env python3
"""Verify an existing C++ resumable service; Python submits/scores, never infers."""
import argparse
import json
from pathlib import Path
import sys
import time
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.testing.run_shared_pool_matrix import request, read_calls, write, digest
from tools.testing.scoring import aggregate

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=8081)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--calls', type=int, default=4)
    parser.add_argument('--concurrency', type=int, default=4)
    parser.add_argument('--manifest', type=Path, default=ROOT/'datasets/manifests/demo_calls.jsonl')
    args = parser.parse_args()
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=False)
    sources = {r['id']:r for r in map(json.loads,args.manifest.read_text().splitlines())}
    capabilities = request(args.port, '/v1/capabilities')
    assert capabilities['streaming_kind']=='resumable_windowed_streaming', capabilities
    body = {'kind':'load','mode':'network','calls':args.calls,'concurrency':args.concurrency,
            'languages':['en','id','zh'],'overrides':['audio.chunk_ms=200','model.decode_step_ms=2000']}
    write(output/'request.json',body); write(output/'capabilities.json',capabilities)
    write(output/'plan.json',request(args.port,'/v1/suites/dry-run',body))
    job = request(args.port,'/v1/suites',body); runtime=[]
    for _ in range(1200):
        state=request(args.port,f"/v1/jobs/{job['job_id']}");runtime.append(request(args.port,'/v1/runtime'))
        if state['status']!='RUNNING':break
        time.sleep(.5)
    write(output/'runtime.json',runtime);write(output/'job.json',state)
    assert 'result' in state,state
    calls=read_calls(state['result'],sources);write(output/'calls.json',calls)
    write(output/'accuracy.json',aggregate(calls))
    failures=[]
    for call in calls:
        if call['status']!='COMPLETE' or not all(call['checks'].values()): failures.append(call['run_id'])
        events=[json.loads(x) for x in (Path(call['directory'])/'events.jsonl').read_text().splitlines()]
        partials=[e for e in events if e['kind']=='partial' and e['text'].strip()]
        if len(partials)<2 or not any(e['before_eof'] for e in partials):failures.append(call['run_id']+':progressive output')
        m=call['measurements']
        if m.get('stream_decode_steps',0)<2 or m.get('stream_reused_prefill_tokens',0)<=0:
            failures.append(call['run_id']+':cache reuse')
    for sample in runtime:
        for worker in sample['workers']:
            if worker['active_sessions']>capabilities['max_sessions_per_process'] or worker['runtime_threads']!=4:
                failures.append('worker occupancy/thread contract')
    write(output/'status.json',{'status':'PASS' if not failures else 'FAIL','failures':failures,
        'calls':len(calls),'completed':sum(c['status']=='COMPLETE' for c in calls),
        'note':'Focused short-call integration check, not capacity/accuracy validation.'})
    write(output/'checksums.json',{str(p.relative_to(output)):digest(p) for p in output.rglob('*') if p.is_file() and p.name!='checksums.json'})
    print(json.dumps({'output':str(output),'failures':failures,'status':'PASS' if not failures else 'FAIL'}))
    return 1 if failures else 0
if __name__=='__main__':sys.exit(main())
