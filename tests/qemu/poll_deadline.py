"""Budgets for short host polling loops, not guest protocol deadlines.

A poll normally runs every 0.1 s. Charge at most two seconds for a single
scheduling gap so suspending/descheduling the host cannot consume a whole
guest-response budget before queued output is read. An unresponsive guest
still exhausts the original budget while the host keeps polling. CI retains
its independent absolute job timeout.
"""
import math
import time


class PollDeadline:
    def __init__(self, timeout, clock=time.monotonic, max_gap=2.0):
        if not math.isfinite(timeout) or timeout <= 0 or not math.isfinite(max_gap) or max_gap <= 0:
            raise ValueError('positive finite polling budget required')
        self.timeout = timeout
        self.clock = clock
        self.max_gap = max_gap
        self.last = clock()
        self.elapsed = 0.0
        self.excluded = 0.0

    def expired(self):
        now = self.clock()
        gap = now-self.last
        if not math.isfinite(gap) or gap < 0:
            raise RuntimeError('host monotonic clock moved backwards or became nonfinite')
        self.last = now
        charged = min(gap, self.max_gap)
        self.elapsed += charged
        self.excluded += gap-charged
        return self.elapsed > self.timeout
