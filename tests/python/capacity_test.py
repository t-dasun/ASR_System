"""Capacity input balance and resource guard protect fair comparisons and RAM."""
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import Mock, patch
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.testing.capacity_stress import ResourceGuard, balanced_calls, concurrency_levels


class CapacityTest(unittest.TestCase):
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
                    return {'timestamp_ns':count[0]*10**9, 'memory':{'MemAvailable':10*2**30},
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
