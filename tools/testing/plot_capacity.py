#!/usr/bin/env python3
"""Create standalone PNG/PDF figures from a completed capacity curve bundle."""
import argparse
import json
from pathlib import Path
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt

    directory = args.directory.resolve()
    data = json.loads((directory/'curve.json').read_text())
    resumable = data['plan'].get('runtime') == 'qwen_stream'
    invocation_key = 'stream_invocation_wall_rtf' if resumable else 'offline_decode_wall_rtf'
    images = directory/'figures'; images.mkdir(exist_ok=True)
    plt.rcParams.update({'font.size':10, 'axes.grid':True, 'grid.alpha':.25})
    captions=[]
    for language in ('en','id','zh'):
        points=[p for p in data['points'] if p['language']==language and p['mode']=='direct']
        if not points:
            continue
        panels=[('Completed audio / wall time', lambda p:p['audio_seconds_per_wall_second'], 'Audio seconds / wall second'),
                ('First usable text',lambda p:p['timings']['first_usable_transcript_ns']['mean'], 'Mean seconds'),
                ('EOF to final text',lambda p:p['timings']['finalization_ns']['mean'], 'Mean seconds'),
                ('Call failures',lambda p:p['failure_rate']*100, 'Percent'),
                ('CPU usage',lambda p:p['sampled_mean_cpu_cores'], 'Mean logical CPU equivalents'),
                ('Physical memory',lambda p:p['sampled_peak_pss_bytes']/2**30, 'Sampled peak PSS GiB'),
                ('Final accuracy error',lambda p:None if p['accuracy']['completed_only']['rate'] is None else p['accuracy']['completed_only']['rate']*100, 'WER/CER percent (completed calls)'),
                ('Text before EOF',lambda p:p['pre_eof_text']/p['completed']*100 if p['completed'] else None, 'Percent of completed calls'),
                ('Streaming invocation RTF' if resumable else 'Offline invocation RTF',lambda p:p['timings'].get(invocation_key, {}).get('mean'), 'Invocation wall / audio duration')]
        fig,axes=plt.subplots(3,3,figsize=(14,11),layout='constrained')
        for workers in sorted({p['workers'] for p in points}):
            selected=sorted((p for p in points if p['workers']==workers),key=lambda p:p['concurrency'])
            x=[p['concurrency'] for p in selected]
            for ax,(title, getter, ylabel) in zip(axes.flat,panels):
                values=[getter(p) for p in selected]
                if title in ('First usable text','EOF to final text'):
                    values=[None if v is None else v/1000 for v in values]
                line,=ax.plot(x,values,marker='o',label=f'{workers} worker(s)')
                if title in ('First usable text','EOF to final text'):
                    key='first_usable_transcript_ns' if title=='First usable text' else 'finalization_ns'
                    tails=[p['timings'][key]['p95'] for p in selected]
                    ax.plot(x,[None if v is None else v/1000 for v in tails],linestyle='--',color=line.get_color())
                ax.set(title=title,xlabel='Target concurrent calls',ylabel=ylabel)
        for ax,(title, _, _) in zip(axes.flat,panels):
            ax.relim()
            ax.autoscale_view()
            ax.set_ylim(bottom=0)
            if title in ('Call failures','Text before EOF'): ax.set_ylim(0,100)
        axes.flat[0].legend()
        runtime_label = 'resumable streaming' if resumable else 'shared-prefix'
        fig.suptitle(f'{language.upper()}: Qwen {runtime_label} CPU capacity\n'
                     f"{data['plan']['per_language']} distinct WAVs, {data['plan']['repetitions']} repetitions; timings: solid mean / dashed p95 (completed calls)")
        for extension in ('png','pdf'):
            fig.savefig(images/f'{language}_capacity.{extension}',dpi=160)
        plt.close(fig)
        captions.extend([f'## {language.upper()} capacity curves', '', f'![{language} capacity curves](figures/{language}_capacity.png)', ''])
    import hashlib
    write_meta={'plot_source_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'matplotlib_version':matplotlib.__version__, 'python':sys.version,
                'source':'curve.json','timing_population':'completed calls; null means no successful observation',
                'figure_units':'timing seconds, memory GiB, CPU core equivalents'}
    (images/'metadata.json').write_text(json.dumps(write_meta,indent=2)+'\n')
    report=directory/'report.md'
    marker='\n## Exported figures\n'
    content=report.read_text().split(marker)[0]
    report.write_text(content+marker+'\n'+'\n'.join(captions))
    import hashlib
    if (directory/'checksums.json').exists():
        hashes={str(p.relative_to(directory)):hashlib.sha256(p.read_bytes()).hexdigest()
                for p in sorted(directory.rglob('*')) if p.is_file() and p.name!='checksums.json'}
        (directory/'checksums.json').write_text(json.dumps(hashes,indent=2)+'\n')
    print(f'Figures: {images}')


if __name__=='__main__':
    main()
