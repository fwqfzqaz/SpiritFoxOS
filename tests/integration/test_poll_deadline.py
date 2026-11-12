"""Deterministic host pauses and real VM wait-path negatives, without a VM."""
from pathlib import Path
import queue
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tests/qemu'))
from poll_deadline import PollDeadline
from qemu_persist_test import VM


class Clock:
    now = 0
    def __call__(self):
        return self.now


class PollDeadlineTests(unittest.TestCase):
    def test_invalid_budgets(self):
        for timeout, gap in [(0,2),(-1,2),(float('inf'),2),(float('nan'),2),(45,0),(45,float('inf')),(45,float('nan'))]:
            with self.subTest(timeout=timeout,gap=gap):
                with self.assertRaises(ValueError):
                    PollDeadline(timeout,max_gap=gap)

    def test_regular_polling_keeps_exact_original_budget(self):
        clock=Clock();deadline=PollDeadline(45,clock)
        for second in range(1,46):
            clock.now=second
            self.assertFalse(deadline.expired())
        clock.now=46
        self.assertTrue(deadline.expired())
        self.assertEqual(deadline.excluded,0)

    def test_long_host_gap_preserves_remaining_budget(self):
        clock=Clock();deadline=PollDeadline(45,clock)
        clock.now=600
        self.assertFalse(deadline.expired())
        self.assertEqual(deadline.excluded,598)
        for second in range(601,644):
            clock.now=second
            self.assertFalse(deadline.expired())
        clock.now=644
        self.assertTrue(deadline.expired())

    def test_repeated_gaps_still_exhaust_budget(self):
        clock=Clock();deadline=PollDeadline(5,clock)
        for second,expired in [(100,False),(200,False),(300,True)]:
            clock.now=second
            self.assertEqual(deadline.expired(),expired)

    def test_invalid_clock_fails_closed(self):
        for now in [-1,float('nan'),float('inf')]:
            clock=Clock();deadline=PollDeadline(45,clock);clock.now=now
            with self.assertRaises(RuntimeError):
                deadline.expired()

    def test_zero_elapsed_does_not_invent_time(self):
        clock=Clock();deadline=PollDeadline(45,clock)
        for _ in range(100):
            self.assertFalse(deadline.expired())
        self.assertEqual(deadline.elapsed,0)

    def test_actual_vm_wait_reads_output_after_host_pause(self):
        clock=Clock()
        class Output:
            data=iter(b'READY')
            def get(self,timeout):
                clock.now=600 if clock.now==0 else clock.now+1
                return bytes([next(self.data)])
        vm=VM.__new__(VM);vm.output=bytearray();vm.name='host-pause';vm.queue=Output()
        vm.process=type('Process',(),{'poll':lambda self:None})()
        deadlines=[]
        def timer(timeout):
            value=PollDeadline(timeout,clock);deadlines.append(value);return value
        with patch('qemu_persist_test.PollDeadline',timer):
            vm.expect('READY',timeout=10)
        self.assertEqual(vm.output,b'READY')
        self.assertEqual(deadlines[0].excluded,598)

    def test_actual_vm_wait_still_rejects_silent_guest(self):
        clock=Clock()
        class Silent:
            def get(self,timeout):
                clock.now+=1
                raise queue.Empty
        vm=VM.__new__(VM);vm.output=bytearray();vm.name='silent';vm.queue=Silent()
        vm.process=type('Process',(),{'poll':lambda self:None})()
        with patch('qemu_persist_test.PollDeadline',lambda timeout:PollDeadline(timeout,clock)):
            with self.assertRaisesRegex(RuntimeError,'missing READY'):
                vm.expect('READY',timeout=3)
        self.assertEqual(clock.now,4)


if __name__=='__main__':
    unittest.main(verbosity=2)
