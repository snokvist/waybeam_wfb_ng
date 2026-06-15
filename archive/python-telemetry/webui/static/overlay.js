/* Uplink (vehicle, 1 Hz) ↔ Downlink (GS, 10 Hz) overlay on one GS wall-clock
   axis. The two streams sample at different rates, and uPlot uses a single
   shared x per chart, so we merge onto the union of timestamps and let each
   series spanGaps over the other stream's points (→ two continuous lines). */
(function () {
  const SID = window.SESSION_ID;
  const axisColor = "#8b949e";
  const UP = "#58a6ff", DN = "#f0883e";

  function mergeXY(t1, y1, t2, y2) {
    const m1 = new Map(), m2 = new Map();
    for (let i = 0; i < t1.length; i++) m1.set(t1[i], y1[i]);
    for (let i = 0; i < t2.length; i++) m2.set(t2[i], y2[i]);
    const xs = Array.from(new Set([...t1, ...t2])).sort((a, b) => a - b);
    return [xs,
            xs.map(x => (m1.has(x) ? m1.get(x) : null)),
            xs.map(x => (m2.has(x) ? m2.get(x) : null))];
  }

  // The uplink (1 Hz) and downlink (10 Hz) streams sample at different rates
  // and never share an x, so on the union x-axis each series is null at the
  // other's points. uPlot's legend reads the value AT the cursor index, so it
  // would read null ~91% of the time for the sparse uplink (and on downlink at
  // uplink-x). Snap each series independently to its nearest non-null sample so
  // both legend values are always populated wherever you hover.
  function nearestNonNull(self, seriesIdx, hoveredIdx) {
    const ys = self.data[seriesIdx];
    if (ys[hoveredIdx] != null) return hoveredIdx;
    const n = ys.length;
    for (let lo = hoveredIdx - 1, hi = hoveredIdx + 1; lo >= 0 || hi < n; lo--, hi++) {
      if (hi < n && ys[hi] != null) return hi;
      if (lo >= 0 && ys[lo] != null) return lo;
    }
    return hoveredIdx;
  }

  function chart(el, title, scaleKey, unit, range, upT, upY, dnT, dnY) {
    const [xs, a, b] = mergeXY(upT, upY, dnT, dnY);
    return new uPlot({
      title, width: el.clientWidth, height: 220,
      cursor: { sync: { key: "overlay" }, dataIdx: nearestNonNull },
      scales: { x: { time: true }, [scaleKey]: range ? { range } : {} },
      axes: [
        { stroke: axisColor, grid: { stroke: "#21262d" }, ticks: { stroke: "#21262d" } },
        { scale: scaleKey, stroke: axisColor, grid: { stroke: "#21262d" } },
      ],
      series: [
        {},
        { label: "uplink " + unit, scale: scaleKey, stroke: UP, spanGaps: true, width: 1.5,
          // sparse 1 Hz stream — show each real sample so the line isn't mistaken
          // for missing data, and the cursor lands on visible points.
          points: { show: true, size: 4 } },
        { label: "downlink " + unit, scale: scaleKey, stroke: DN, spanGaps: true, width: 1.5,
          points: { show: false } },
      ],
    }, [xs, a, b], el);
  }

  async function main() {
    const root = document.getElementById("charts");
    let j;
    try {
      const r = await fetch(`/api/session/${SID}/overlay`);
      if (!r.ok) { root.textContent = "failed to load overlay"; return; }
      j = await r.json();
    } catch (e) { root.textContent = "failed to load overlay"; return; }

    const meta = document.getElementById("overlay-meta");
    if (!j.aligned) {
      meta.textContent = j.gs_session
        ? " — vehicle records carry no gs_unix_ms (pre-LOG_SYNC walk, not aligned)."
        : " — no paired GS session found (no LOG_SYNC markers, or GS capture missing).";
      if (!j.uplink.t.length) return;
    } else {
      meta.textContent = ` — paired GS session ${j.gs_session} · ` +
        `uplink ${j.uplink.t.length} pts (1 Hz) · downlink ${j.downlink.t.length} pts (10 Hz).`;
    }

    const mk = () => { const d = document.createElement("div"); root.appendChild(d); return d; };
    const charts = [
      chart(mk(), "RSSI — uplink vs downlink (dBm)", "rssi", "(dBm)", null,
            j.uplink.t, j.uplink.rssi, j.downlink.t, j.downlink.rssi),
      chart(mk(), "PER — uplink vs downlink", "per", "PER", [0, 1],
            j.uplink.t, j.uplink.per, j.downlink.t, j.downlink.per),
    ];
    let raf;
    window.addEventListener("resize", () => {
      cancelAnimationFrame(raf);
      raf = requestAnimationFrame(() => {
        for (const u of charts)
          u.setSize({ width: u.root.parentNode.clientWidth, height: 220 });
      });
    });
  }
  main();
})();
