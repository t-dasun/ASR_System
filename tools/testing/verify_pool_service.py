#!/usr/bin/env python3
"""Verify final C++ manifest service without a fallback WAV, plus dashboard controls."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import time
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.testing.run_shared_pool_matrix import ROOT, digest, request, run_suite, write

parser=argparse.ArgumentParser()
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
output=args.output.resolve()
if output.exists():parser.error('output already exists')
output.mkdir(parents=True)
manifest=ROOT/'datasets/manifests/demo_calls.jsonl'
sources={r['id']:r for line in manifest.read_text().splitlines() for r in [json.loads(line)]}
cli=ROOT/'build/release-cpu/asr-cli'
command=[str(cli),'serve','--config','configs/qwen_prefix_shared.yaml','--manifest',str(manifest),
    '--set','workers.processes=2','--set','workers.max_sessions_per_process=2',
    '--set','audio.path=missing_manifest_base.wav','--set',f'output.directory={output}','--port','0']
write(output/'command.json',{'command':command,'cli_sha256':digest(cli),
    'worker_sha256':digest(ROOT/'build/release-cpu/asr-prefix-worker'),'manifest_sha256':digest(manifest)})
with (output/'service.stdout').open('w') as stdout, (output/'service.stderr').open('w') as stderr:
    process=subprocess.Popen(command,cwd=ROOT,stdout=stdout,stderr=stderr,start_new_session=True)
    try:
        port=None;deadline=time.monotonic()+150
        while time.monotonic()<deadline:
            if process.poll() is not None:raise RuntimeError((output/'service.stderr').read_text())
            for line in (output/'service.stdout').read_text().splitlines():
                try:port=json.loads(line).get('port',port)
                except json.JSONDecodeError:pass
            if port is not None:break
            time.sleep(.2)
        assert port is not None
        caps=request(port,'/v1/capabilities')
        assert caps['engine']=='qwen_prefix_process_pool' and caps['cooperative_cancellation']
        browser=subprocess.run(['node','frontend/tests/dashboard.pool.e2e.mjs',f'http://127.0.0.1:{port}',
            str(output/'dashboard.json')],cwd=ROOT,text=True,capture_output=True,timeout=60)
        if browser.returncode:raise RuntimeError(browser.stderr)
        body={'kind':'load','mode':'network','calls':4,'concurrency':4,'languages':['en','id','zh'],'max_failure_rate':0}
        job=run_suite(port,body,output,'shared_2w_2s','mixed','network',sources)
        assert job['suite']['status']=='COMPLETE' and job['suite']['completed_calls']==4
        assert all(c['before_eof_text'] for c in job['calls'])
        write(output/'verification.json',{'status':'PASS','capabilities':caps,'dashboard':json.loads(browser.stdout),
            'missing_fallback_wav_ignored':True,'job':job,'idle_runtime':request(port,'/v1/runtime')})
        print('Final C++ service: missing fallback WAV, two workers/two calls each, four distinct-WAV network calls, pre-EOF text and browser controls passed',flush=True)
    finally:
        if process.poll() is None:
            os.killpg(process.pid,signal.SIGTERM)
            try:process.wait(timeout=60)
            except subprocess.TimeoutExpired:os.killpg(process.pid,signal.SIGKILL);process.wait()
write(output/'checksums.json',{str(p.relative_to(output)):digest(p) for p in sorted(output.rglob('*')) if p.is_file()})
