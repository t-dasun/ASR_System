"""Capacity input balance and resource guard protect fair comparisons and RAM."""
from pathlib import Path
import sys
import tempfile
import time
import unittest
import json
from unittest.mock import Mock, patch
from types import SimpleNamespace
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.testing.capacity_stress import ResourceGuard, balanced_calls, concurrency_levels, matrix_layouts, report, resume_state, attempt_directory


class CapacityTest(unittest.TestCase):
    def test_resumable_grid_has_exact_requested_layouts(self):
        layouts = matrix_layouts(SimpleNamespace(resumable_matrix=True))
        self.assertEqual(len(layouts), 12)
        self.assertEqual([(w,s) for w,s,_ in layouts], [(1,s) for s in range(1,9)] + [(2,s) for s in (1,2,4,8)])
        self.assertTrue(all(levels == [w*s] for w,s,levels in layouts))
        self.assertEqual(layouts[-1], (2,8,[16]))
        self.assertEqual(sum(balanced_calls(10,c)*3*3 for _,_,levels in layouts for c in levels), 1170)

    def test_legacy_stress_grid_is_preserved(self):
        layouts = matrix_layouts(SimpleNamespace(resumable_matrix=False,max_workers=2,dense=False))
        self.assertEqual(layouts, [(1,8,[1,2,4,8]),(2,8,[1,2,4,8,16])])

    def test_resume_keeps_completed_layouts_and_preserves_old_attempts(self):
        with tempfile.TemporaryDirectory() as temporary:
            output=Path(temporary)
            plan={'matrix_mode':'resumable_16_layouts','per_language':10,'repetitions':3,
                  'reserve_bytes':2*2**30,'config_sha256':'hash','cli_sha256':'hash',
                  'worker_sha256':'hash','manifest_sha256':'hash','layouts':[]}
            runs=[{'workers':1,'slots_per_worker':1,'status':'MEASURED'},
                  {'workers':1,'slots_per_worker':2,'status':'ABORTED_RESOURCE'},
                  {'workers':2,'slots_per_worker':3,'status':'MEASURED'},
                  {'workers':2,'slots_per_worker':1,'status':'NOT_RUN_RESOURCE'}]
            jobs=[{'workers':1,'slots_per_worker':1,'label':'keep'},
                  {'workers':1,'slots_per_worker':2,'label':'old_attempt'},
                  {'workers':2,'slots_per_worker':3,'label':'omitted_layout'}]
            (output/'curve.json').write_text(json.dumps({'plan':plan,'worker_runs':runs}))
            (output/'jobs.json').write_text(json.dumps(jobs))
            (output/'status.json').write_text(json.dumps({'status':'COMPLETE_WITH_RESOURCE_LIMIT'}))
            args=SimpleNamespace(per_language=10,repetitions=3,reserve_gib=2,resumable_matrix=True)
            with patch('tools.testing.capacity_stress.digest',return_value='hash'):
                revised, retained, measured, old_runs, old_jobs=resume_state(output,args,output/'cli','config')
                self.assertEqual(len(revised['layouts']),12)
                self.assertEqual(revised['planned_curve_calls'],1170)
                self.assertEqual(retained,[jobs[0]])
                self.assertEqual(measured,[runs[0]])
                self.assertEqual(old_runs,runs)
                self.assertEqual(old_jobs,jobs)
                args.repetitions=1
                with self.assertRaisesRegex(ValueError,'repetitions'):
                    resume_state(output,args,output/'cli','config')
                args.repetitions=3
                (output/'status.json').write_text(json.dumps({'status':'RUNNING'}))
                with self.assertRaisesRegex(ValueError,'RUNNING'):
                    resume_state(output,args,output/'cli','config')
            first=attempt_directory(output,1,2,True)
            (first/'evidence').write_text('original')
            second=attempt_directory(output,1,2,True)
            self.assertEqual(second.name,'1w_2s_attempt2')
            self.assertEqual((first/'evidence').read_text(),'original')

    def test_resumable_report_preserves_metrics_and_excludes_failed_accuracy(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            sample = {'timestamp_ns':1, 'processes':[], 'cpu':{'cpu':{'utilization_percent':0}},
                      'host_memory_bytes':{'MemAvailable':10*2**30}}
            (output/'resources.jsonl').write_text(json.dumps(sample)+'\n')
            metrics = {'stream_decode_wall_ns':5_000_000, 'stream_decode_queue_wait_ns':2_000_000,
                       'stream_invocation_wall_ns':5_000_000, 'stream_invocation_wall_rtf':0.25,
                       'stream_decode_steps':3, 'stream_reused_prefill_tokens':17}
            def call(status, edits):
                return {'status':status, 'language':'en', 'recording_id':status, 'transcript':'text',
                        'accuracy':{'edits':edits,'reference_units':10}, 'checks':{'real_model':True},
                        'before_eof_text':True, 'measurements':metrics}
            suite = {'directory':str(output), 'metrics':{'measurement_wall_seconds':1,'completed_audio_seconds':1},
                     'phases':[{'resource_samples_path':'resources.jsonl',
                                'resources':{'max_tree_cpu_core_equivalents':1,
                                             'sampled_peak_tree_rss_bytes':100,'sampled_peak_tree_pss_bytes':50}}]}
            job = {'workers':2,'slots_per_worker':8,'kind':'curve','label':'c16_en',
                   'request':{'languages':['en'],'mode':'direct','concurrency':16},'suite':suite,
                   'calls':[call('COMPLETE',1),call('FAILED',10)],
                   'peak_simultaneous_sessions':16,'peak_sessions_per_worker':[8,8]}
            points = report(output, {'runtime':'qwen_stream','per_language':10,'repetitions':3}, [job], [])
            point = points[0]
            self.assertEqual(point['timings']['stream_decode_wall_ns']['mean'],5)
            self.assertEqual(point['timings']['stream_decode_steps']['count'],1)
            self.assertEqual(point['timings']['stream_invocation_wall_rtf']['mean'],0.25)
            self.assertIsNone(point['timings']['offline_decode_wall_rtf']['mean'])
            self.assertEqual(point['accuracy']['completed_only']['rate'],0.1)
            self.assertEqual(point['accuracy']['rate'],0.55)
            self.assertEqual(point['slots_per_worker'],8)
            self.assertIn('Resumable decoding', (output/'report.md').read_text())
            self.assertIn('stream_decode_steps_mean',(output/'curve.csv').read_text())

    def test_larger_load_replays_whole_cohorts(self):
        for concurrent in range(1,33):
            calls=balanced_calls(10,concurrent)
            self.assertGreaterEqual(calls,concurrent)
            counts=[sum(i%10==record for i in range(calls)) for record in range(10)]
            self.assertEqual(len(set(counts)),1)
        self.assertEqual(balanced_calls(10,32),40)
        self.assertEqual(concurrency_levels(4,8,True),list(range(1,33)))

    def test_guard_stops_on_memory_reserve(self):
        with tempfile.TemporaryDirectory() as temporary:
            sample={'memory':{'MemAvailable':1},'swap_pages':{'pswpin':0,'pswpout':0}}
            process=Mock(pid=123)
            with patch('tools.testing.capacity_stress.host_sample',return_value=sample):
                with patch('tools.testing.capacity_stress.stop') as stop:
                    guard=ResourceGuard(process,Path(temporary)/'resources.jsonl',2**31,64*2**20)
                    guard.thread.join(timeout=3)
                    self.assertIsNotNone(guard.error)
                    stop.assert_called_once_with(process)
                    guard.close()


    def test_host_swap_alone_is_not_a_capacity_limit(self):
        for stalled in [0.0,2.0]:
            with tempfile.TemporaryDirectory() as temporary:
                count=[0]
                def sample(*args):
                    count[0]+=1
                    return {'timestamp_ns':count[0]*10**9, 'memory':{'MemAvailable':10*2**30,'SwapFree':1},
                            'swap_pages':{'pswpin':0,'pswpout':0 if count[0]<3 else 100000},
                            'memory_pressure':{'full':{'avg10':stalled}}}
                with patch('tools.testing.capacity_stress.host_sample',side_effect=sample):
                    with patch('tools.testing.capacity_stress.stop') as stop:
                        guard=ResourceGuard(Mock(pid=123),Path(temporary)/'resources.jsonl',2**31,64*2**20)
                        deadline=time.monotonic()+3
                        while len(guard.samples)<2 and time.monotonic()<deadline: time.sleep(.01)
                        guard.close()
                        self.assertIsNone(guard.error)
                        stop.assert_not_called()


if __name__=='__main__':
    unittest.main()
