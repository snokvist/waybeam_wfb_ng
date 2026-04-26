"""
Async glue: rx_ant listener -> selector -> wfb_tx control.

Boot sequence:
  1. CMD_GET_RADIO sync to populate the WfbTxRadioControl cache. Failure
     here is fatal (we don't know the radiotap fields to preserve).
  2. Send range[0] mcs as the initial MCS — guarantees the link starts
     from the safe end while the FSM warms up. Skipped in dry-run mode.
  3. Bind the rx_ant listener.
  4. Run a watchdog ticker (every failsafe_timeout_s/4) so the selector
     can trip into failsafe even if no datagram ever arrives.
"""

import asyncio
import logging

from mcs_selector.config import MCSSelectorConfig
from mcs_selector.protocol import RxAntDatagram
from mcs_selector.ranges import mcs_for_bucket
from mcs_selector.selector import Decision, MCSSelector
from mcs_selector.stats_input import serve_rx_ant
from mcs_selector.wfb_control import WfbTxRadioControl

log = logging.getLogger("mcs_sel.service")


class MCSSelectorService:

    def __init__(self, cfg: MCSSelectorConfig | None = None):
        self.cfg = cfg or MCSSelectorConfig()
        self.selector = MCSSelector(self.cfg)
        self.control = WfbTxRadioControl(
            host=self.cfg.tx_host,
            port=self.cfg.tx_port,
            response_timeout_s=self.cfg.control_response_timeout_s,
        )
        self._transport = None
        self._watchdog_task: asyncio.Task | None = None
        self._stop = asyncio.Event()

    # ------------------------------------------------------------- callbacks

    async def _on_datagram(self, d: RxAntDatagram) -> None:
        decision = self.selector.update(d)
        if decision is None:
            return
        await self._apply(decision)

    async def _apply(self, decision: Decision) -> None:
        if not decision.changed and decision.reason != "init":
            return
        log.info(
            "decision: mcs=%d bucket=%d reason=%s failsafe=%s%s",
            decision.mcs_index, decision.bucket, decision.reason,
            decision.failsafe,
            ""
            if decision.score is None
            else f" eff_rssi={decision.score.effective_rssi:.1f} "
                 f"loss={decision.score.smoothed_loss_ratio*100:.1f}%",
        )
        if self.cfg.dry_run:
            return
        ok = self.control.set_mcs(decision.mcs_index)
        if not ok:
            log.warning("set_mcs(%d) failed — link may diverge from selector state",
                        decision.mcs_index)

    # ------------------------------------------------------------- watchdog

    async def _watchdog(self) -> None:
        # Tick at 1/4 the failsafe period so a gap is detected promptly.
        period = max(0.05, self.cfg.failsafe_timeout_s / 4.0)
        while not self._stop.is_set():
            try:
                await asyncio.wait_for(self._stop.wait(), timeout=period)
            except asyncio.TimeoutError:
                pass
            decision = self.selector.tick_no_data()
            if decision is not None:
                await self._apply(decision)

    # --------------------------------------------------------------- run

    async def start(self) -> None:
        params = self.control.get_radio()
        if params is None:
            raise RuntimeError(
                "CMD_GET_RADIO sync failed — cannot construct CMD_SET_RADIO "
                "without the current bandwidth/stbc/ldpc/sgi/vht fields. "
                "Is wfb_tx running on %s:%d?" % (self.cfg.tx_host, self.cfg.tx_port)
            )
        log.info(
            "wfb_tx radio sync: bw=%d sgi=%d stbc=%d ldpc=%d mcs=%d vht=%d nss=%d",
            params.bandwidth, params.short_gi, params.stbc, params.ldpc,
            params.mcs_index, params.vht_mode, params.vht_nss,
        )

        # Stamp range[0] as the safe starting MCS unless dry-run.
        initial_mcs = mcs_for_bucket(0, self.cfg)
        if not self.cfg.dry_run and params.mcs_index != initial_mcs:
            log.info("startup: forcing mcs=%d (range[0])", initial_mcs)
            self.control.set_mcs(initial_mcs)

        self._transport, _ = await serve_rx_ant(
            self.cfg.stats_host, self.cfg.stats_port, self._on_datagram
        )
        self._watchdog_task = asyncio.create_task(self._watchdog())

    async def stop(self) -> None:
        self._stop.set()
        if self._watchdog_task:
            try:
                await self._watchdog_task
            except asyncio.CancelledError:
                pass
        if self._transport is not None:
            self._transport.close()
        self.control.close()

    async def run_forever(self) -> None:
        await self.start()
        try:
            await self._stop.wait()
        finally:
            await self.stop()
