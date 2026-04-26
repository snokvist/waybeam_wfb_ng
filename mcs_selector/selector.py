"""
MCS selector core — asymmetric FSM.

Philosophy mirrors fec_controller (TCP AIMD), but inverted: in FEC, "up"
means more redundancy and is the safe direction; here, "down" means a
slower/more-robust MCS and is the safe direction. Either way, we react
fast in the safe direction and cautiously in the risky one — a
recently-bad link is more likely to stay bad than to recover.

Decision pipeline per tick:

  1. Watchdog: if no datagram for `failsafe_timeout_s`, force range[0]
     immediately and freeze the FSM until recovery.
  2. Scorer: smooth RSSI + loss penalty -> effective_rssi.
  3. Bucket-from-RSSI with deadband (ranges.py).
  4. Hysteresis: require N consecutive samples in the candidate bucket
     (different N for up vs down).
  5. Cooldown: enforce min interval since last change (different per
     direction; up is widened during oscillation).
  6. Commit + record for oscillation detector.
"""

import logging
import time
from collections import deque
from dataclasses import dataclass
from typing import Callable

from mcs_selector.config import MCSSelectorConfig
from mcs_selector.ranges import bucket_from_rssi, mcs_for_bucket
from mcs_selector.scorer import Score, Scorer
from mcs_selector.protocol import RxAntDatagram

log = logging.getLogger("mcs_sel")


@dataclass
class Decision:
    """Outcome of a single tick."""
    bucket: int
    mcs_index: int
    changed: bool
    failsafe: bool
    score: Score | None
    reason: str  # human-readable: "init", "down", "up", "hysteresis", ...


class MCSSelector:

    def __init__(
        self,
        config: MCSSelectorConfig | None = None,
        time_fn: Callable[[], float] | None = None,
    ):
        self.cfg = config or MCSSelectorConfig()
        self._time_fn = time_fn or time.monotonic
        self.scorer = Scorer(self.cfg)

        self.current_bucket: int | None = None
        self.current_mcs: int | None = None
        self.last_change_time: float = -1e9
        self.last_datagram_time: float | None = None

        # Hysteresis counters: streak of samples voting for `_pending_bucket`.
        self._pending_bucket: int | None = None
        self._pending_streak: int = 0

        # Failsafe state.
        self._in_failsafe: bool = False
        self._recovery_streak: int = 0

        # Oscillation detector — timestamps of recent committed changes.
        self._change_times: deque[float] = deque()

    # --------------------------------------------------------------- helpers

    @property
    def is_oscillating(self) -> bool:
        return len(self._change_times) >= self.cfg.oscillation_threshold

    def _record_change(self, now: float) -> None:
        self._change_times.append(now)
        cutoff = now - self.cfg.oscillation_window_s
        while self._change_times and self._change_times[0] < cutoff:
            self._change_times.popleft()

    def _commit(
        self, bucket: int, now: float, score: Score | None, reason: str
    ) -> Decision:
        prev_mcs = self.current_mcs
        self.current_bucket = bucket
        self.current_mcs = mcs_for_bucket(bucket, self.cfg)
        changed = prev_mcs != self.current_mcs
        if changed:
            self.last_change_time = now
            self._record_change(now)
        # Reset pending streak — current bucket is now the bucket we're in.
        self._pending_bucket = None
        self._pending_streak = 0
        return Decision(
            bucket=bucket,
            mcs_index=self.current_mcs,
            changed=changed,
            failsafe=self._in_failsafe,
            score=score,
            reason=reason,
        )

    # --------------------------------------------------------------- ticks

    def tick_no_data(self, now: float | None = None) -> Decision | None:
        """Drive the watchdog without a fresh datagram. Caller should
        invoke this on a timer (e.g. every 50 ms) so the failsafe trips
        even if the listener is starved.
        """
        now = self._time_fn() if now is None else now

        # Never seen a datagram yet — wait for the first one. The service
        # layer initialises with a SET_RADIO at startup (range[0] from
        # boot) so we don't need to emit anything here.
        if self.last_datagram_time is None:
            return None

        gap = now - self.last_datagram_time
        if gap < self.cfg.failsafe_timeout_s:
            return None

        if self._in_failsafe and self.current_bucket == 0:
            # Already failsafed — nothing to do.
            return None

        log.warning(
            "failsafe: no rx_ant for %.2fs (>%.2fs) — forcing range[0]",
            gap, self.cfg.failsafe_timeout_s,
        )
        self._in_failsafe = True
        self._recovery_streak = 0
        self.scorer.reset()
        return self._commit(0, now, None, "failsafe")

    def update(self, d: RxAntDatagram, now: float | None = None) -> Decision | None:
        """Feed one wfb_rx -Y datagram. Returns a Decision if the
        emitted MCS should change (or this is the very first commit);
        None means no action this tick.
        """
        now = self._time_fn() if now is None else now
        self.last_datagram_time = now

        score = self.scorer.update(d)

        # No antennas yet — treat like "no data" for failsafe purposes,
        # but don't reset the watchdog since we did receive a datagram.
        # The watchdog is driven off `last_datagram_time`; instead, hold
        # state and wait for the first scored sample.
        if score is None:
            return None

        candidate = bucket_from_rssi(score.effective_rssi, self.current_bucket, self.cfg)

        # First commit ever — emit immediately so wfb_tx and the selector
        # agree, regardless of cooldown / hysteresis.
        if self.current_bucket is None:
            return self._commit(candidate, now, score, "init")

        # Failsafe recovery gating: even good samples don't unfreeze the
        # FSM until we've seen `failsafe_recovery_consecutive` of them.
        if self._in_failsafe:
            if score.effective_rssi >= self.cfg.rssi_thresh_low + self.cfg.rssi_deadband_db:
                self._recovery_streak += 1
                if self._recovery_streak >= self.cfg.failsafe_recovery_consecutive:
                    log.info("failsafe: recovered after %d good samples",
                             self._recovery_streak)
                    self._in_failsafe = False
                    self._recovery_streak = 0
                    # Fall through to normal selection logic below.
                else:
                    return None
            else:
                self._recovery_streak = 0
                return None

        if candidate == self.current_bucket:
            self._pending_bucket = None
            self._pending_streak = 0
            return None

        # Streak accounting for hysteresis.
        if candidate != self._pending_bucket:
            self._pending_bucket = candidate
            self._pending_streak = 1
        else:
            self._pending_streak += 1

        going_down = candidate < self.current_bucket
        required = (
            self.cfg.down_consecutive if going_down else self.cfg.up_consecutive
        )
        if self._pending_streak < required:
            return None

        # Cooldown.
        elapsed = now - self.last_change_time
        if going_down:
            if elapsed < self.cfg.down_cooldown_s:
                return None
            return self._commit(candidate, now, score, "down")

        # Going up — widen cooldown if the FSM has been thrashing.
        cooldown = self.cfg.up_cooldown_s
        if self.is_oscillating:
            cooldown *= self.cfg.oscillation_backoff
        if elapsed < cooldown:
            return None
        return self._commit(candidate, now, score, "up")
