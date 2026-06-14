/* Session detail: synchronised uPlot time-series + ML/human overlays, plus
   phase-4 labeling — drag-select a span (label mode) to write a labels row,
   manage existing labels, and edit session metadata. */
"use strict";

const SID = window.SESSION_ID;
const SYNC_KEY = "wfb";

const C = {
  rssi: "#2f81f7", snr: "#3fb950", per: "#f85149", lost: "#d29922",
  mcs: "#58a6ff", tier1: "#db61a2",
};
const OVL = {
  tier1_crit: "rgba(248,81,73,0.16)",
  tier1_degr: "rgba(210,153,34,0.13)",
  event: "rgba(88,166,255,0.15)",
  cliff:  "rgba(219,97,162,0.18)",
  action: "rgba(63,185,80,0.15)",
  state:  "rgba(139,148,158,0.16)",
};

let STATE_BANDS = [];   // from Tier-1 predictions (read-only)
let LABELS = [];        // human labels (editable)
let BANDS = [];         // merged, what the plugin draws
let CHARTS = [];
let LABEL_MODE = false;
let PENDING = null;     // {x0, x1} from the active selection
let ZOOMED = false;     // user has zoomed the x-scale in — hold live tailing
// A drag below this many px is treated as a click: uPlot fires setSelect with
// ~0 width on a plain click, so onSelect expands sub-CLICK_PX selections to a
// small default span — single-clicking a point works, not only dragging a span.
const CLICK_PX = 6;

/* ---- overlay bands ---- */
function bandsPlugin() {
  return { hooks: { draw: u => {
    if (!BANDS.length) return;
    const ctx = u.ctx, top = u.bbox.top, h = u.bbox.height;
    const xmin = u.scales.x.min, xmax = u.scales.x.max;
    ctx.save();
    for (const b of BANDS) {
      if (b.x1 < xmin || b.x0 > xmax) continue;
      const x0 = u.valToPos(Math.max(b.x0, xmin), "x", true);
      const x1 = u.valToPos(Math.min(b.x1, xmax), "x", true);
      ctx.fillStyle = b.color;
      ctx.fillRect(x0, top, Math.max(1.5, x1 - x0), h);
    }
    ctx.restore();
  }}};
}

function stateBands(t, states) {
  const out = [];
  let i = 0;
  while (i < states.length) {
    const s = states[i];
    if (s === 1 || s === 2) {
      let j = i;
      while (j + 1 < states.length && states[j + 1] === s) j++;
      out.push({ x0: t[i], x1: t[j], color: s === 2 ? OVL.tier1_crit : OVL.tier1_degr });
      i = j + 1;
    } else i++;
  }
  return out;
}

function rebuildBands() {
  BANDS = STATE_BANDS.concat(LABELS.map(l => ({
    x0: l.t0_s, x1: l.t1_s, color: OVL[l.kind] || OVL.event,
  })));
  for (const u of CHARTS) u.redraw(false);
}

/* ---- charts ---- */
const stepped = uPlot.paths.stepped({ align: 1 });
const bars = uPlot.paths.bars({ size: [1.0, Infinity], align: 1 });
const axisColor = "#8b949e";

// MCS rung colours: warm/low = degraded or peek-protected rungs, cool/high =
// healthy bulk (M5 matches the existing blue). Index = MCS index.
const MCS_COLORS = ["#f85149", "#db6d28", "#d29922", "#bf8700",
                    "#3fb950", "#58a6ff", "#388bfd", "#1f6feb"];

// Build a stacked-bar dataset from per-rung packet counts. uPlot has no native
// stacking, so we emit CUMULATIVE heights (bottom=M0 … top=M7) and draw rungs
// back-to-front (tallest cumulative first) — each opaque bar over-paints the
// one below, leaving each rung's own segment visible. Fixed 8 rungs so the
// series count never changes between live polls.
function mcsStack(t, dist) {
  const N = t.length;
  const seg = [];
  for (let m = 0; m < 8; m++) {
    const a = dist && dist[String(m)];
    seg[m] = a && a.length === N ? a : new Array(N).fill(0);
  }
  const cum = [];
  for (let m = 0; m < 8; m++) {
    cum[m] = new Array(N);
    for (let i = 0; i < N; i++)
      cum[m][i] = (m === 0 ? 0 : cum[m - 1][i]) + (+seg[m][i] || 0);
  }
  const data = [t], series = [{}];
  for (let m = 7; m >= 0; m--) {            // top → bottom = draw back → front
    data.push(cum[m]);
    // Legend/tooltip must show this rung's OWN packet count, not the
    // cumulative stack height the bar geometry is drawn from. Each series'
    // datum is cum[m]; the rung below it is the next series (m-1), so
    // seg[m] = cum[m] - cum[m-1] = data[si] - data[si+1] (0 for bottom rung
    // M0, which has no series below it). This stays correct across live polls
    // because it reads the live chart data, not a captured array.
    series.push({ label: `M${m}`, scale: "pkts", stroke: MCS_COLORS[m],
                  fill: MCS_COLORS[m], paths: bars, points: { show: false },
                  value: (u, v, si, di) => {
                    if (v == null) return "";
                    const below = (u.data[si + 1] && u.data[si + 1][di]) || 0;
                    return Math.round(v - below);
                  } });
  }
  return { data, series };
}

function makeChart(el, title, series, data, scales, axes) {
  const opts = {
    title, width: el.clientWidth, height: 200,
    cursor: { sync: { key: SYNC_KEY }, drag: { x: true, y: false, setScale: true } },
    legend: { live: true },
    scales: scales || {},
    axes: [{ stroke: axisColor, grid: { stroke: "#21262d" }, ticks: { stroke: "#21262d" } }].concat(axes),
    plugins: [bandsPlugin()],
    hooks: { setSelect: [onSelect], setScale: [onSetScale] },
    series,
  };
  const u = new uPlot(opts, data, el);
  // uPlot only fires setSelect for a real drag; a plain click (no movement)
  // may fire it with ~0 width or not at all. Make a click deterministic in
  // label mode: synthesise a zero-width selection at the click so onSelect
  // (which expands sub-CLICK_PX selections to a default span) always runs.
  let downX = null;
  u.over.addEventListener("mousedown", e => { downX = e.clientX; });
  u.over.addEventListener("mouseup", e => {
    if (!LABEL_MODE || downX === null) return;
    const moved = Math.abs(e.clientX - downX);
    downX = null;
    if (moved > CLICK_PX) return;  // real drag — setSelect already handled it
    const lpx = e.clientX - u.over.getBoundingClientRect().left;
    u.setSelect({ left: lpx, width: 0, top: 0, height: u.over.clientHeight }, true);
  });
  return u;
}

// Track whether the user has zoomed the x-axis away from the full data extent.
// While zoomed, the live poll holds (it would otherwise setData(resetScales)
// and snap back to full range every 2 s). Double-click (uPlot's built-in zoom
// reset) restores the full extent → clears the flag → tailing resumes.
function onSetScale(u, key) {
  if (key !== "x") return;
  const d = u.data[0];
  if (!d || d.length < 2) { ZOOMED = false; return; }
  const lo = d[0], hi = d[d.length - 1], eps = (hi - lo) * 1e-4 + 1e-6;
  ZOOMED = (u.scales.x.min > lo + eps) || (u.scales.x.max < hi - eps);
}

function onSelect(u) {
  if (!LABEL_MODE) return;
  const sel = u.select;
  if (!sel) return;
  let a, b;
  if (sel.width <= CLICK_PX) {
    const cx = u.posToVal(sel.left + sel.width / 2, "x");
    const xmin = u.scales.x.min, xmax = u.scales.x.max;
    const half = Math.max(0.25, (xmax - xmin) * 0.0075);
    a = Math.max(xmin, cx - half);
    b = Math.min(xmax, cx + half);
  } else {
    a = u.posToVal(sel.left, "x");
    b = u.posToVal(sel.left + sel.width, "x");
  }
  PENDING = { x0: Math.min(a, b), x1: Math.max(a, b) };
  // reflect the (possibly click-expanded) span as a visible selection box;
  // fire=false so this doesn't re-enter onSelect. Skip when it already matches
  // the drag the user made.
  const lpx = u.valToPos(PENDING.x0, "x");
  const wpx = u.valToPos(PENDING.x1, "x") - lpx;
  if (Math.abs(wpx - sel.width) > 0.5)
    u.setSelect({ left: lpx, width: wpx, top: 0, height: u.over.clientHeight }, false);
  const f = document.getElementById("label-form");
  document.getElementById("lf-range").textContent =
    `${PENDING.x0.toFixed(2)}s – ${PENDING.x1.toFixed(2)}s`;
  f.classList.remove("hidden");
  f.querySelector('input[name="value"]').focus();
}

function clearSelections() {
  for (const u of CHARTS) u.setSelect({ left: 0, width: 0, top: 0, height: 0 }, false);
  PENDING = null;
  document.getElementById("label-form").classList.add("hidden");
}

/* ---- labels panel ---- */
async function reloadLabels() {
  const r = await fetch(`/api/session/${SID}/labels`);
  LABELS = r.ok ? await r.json() : [];
  renderLabels();
  rebuildBands();
}

function renderLabels() {
  const el = document.getElementById("labels-panel");
  if (!LABELS.length) { el.innerHTML = '<p class="muted">No labels yet. Toggle label mode, then click a point or drag a span on a chart.</p>'; return; }
  const rows = LABELS.map(l => `
    <tr>
      <td><span class="chip hl-${l.kind}">${l.kind}</span></td>
      <td>${escapeHtml(l.value || "")}</td>
      <td class="num">${l.t0_s.toFixed(2)}–${l.t1_s.toFixed(2)}s</td>
      <td>${escapeHtml(l.author || "")}</td>
      <td><button data-del="${l.id}" class="del">delete</button></td>
    </tr>`).join("");
  el.innerHTML = `<table class="labels"><thead><tr>
      <th>kind</th><th>value</th><th>range</th><th>author</th><th></th>
    </tr></thead><tbody>${rows}</tbody></table>`;
  el.querySelectorAll("button.del").forEach(b =>
    b.addEventListener("click", async () => {
      await fetch(`/api/label/${b.dataset.del}`, { method: "DELETE" });
      reloadLabels();
    }));
}

function escapeHtml(s) {
  return s.replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}

/* ---- wiring ---- */
function wireForms() {
  document.getElementById("label-mode").addEventListener("change", e => {
    LABEL_MODE = e.target.checked;
    for (const u of CHARTS) u.cursor.drag.setScale = !LABEL_MODE;  // label-mode = select, else zoom
    document.body.classList.toggle("labeling", LABEL_MODE);
    if (!LABEL_MODE) clearSelections();
  });

  const authorInput = document.getElementById("lf-author");
  authorInput.value = localStorage.getItem("wfb-author") || "human:web";

  document.getElementById("label-form").addEventListener("submit", async e => {
    e.preventDefault();
    if (!PENDING) return;
    const fd = new FormData(e.target);
    const author = (fd.get("author") || "human:web").trim();
    localStorage.setItem("wfb-author", author);
    const res = await fetch(`/api/session/${SID}/labels`, {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ t0_s: PENDING.x0, t1_s: PENDING.x1,
        kind: fd.get("kind"), value: fd.get("value"), author }),
    });
    if (res.ok) { e.target.querySelector('input[name="value"]').value = ""; clearSelections(); reloadLabels(); }
  });
  document.getElementById("lf-cancel").addEventListener("click", clearSelections);

  document.getElementById("meta-form").addEventListener("submit", async e => {
    e.preventDefault();
    const body = Object.fromEntries(new FormData(e.target).entries());
    const res = await fetch(`/api/session/${SID}/meta`, {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    });
    document.getElementById("meta-status").textContent =
      res.ok ? "saved — reload to see it in the header" : "save failed";
  });
}

/* ---- live tailing ---- */
let LIVE = true;
let LAST_N = -1;
let IS_VEHICLE = false;   // set in main() from the series payload's source flag
const POLL_MS = 2000;

function setLiveDot(txt, cls) {
  const d = document.getElementById("live-dot");
  if (d) { d.textContent = txt; d.className = cls; }
}

// Push fresh series into the existing charts (used by the poll). setData with
// resetScales=true tails the view to the full range — the expected live
// behaviour; pause via the toggle to inspect a frozen window.
function applySeries(j) {
  const S = j.series, t = S.t;
  document.getElementById("overlay-status").textContent =
    ` · ${j.n} records · model ${j.model_ver || "(none scored)"}`;
  if (IS_VEHICLE) {
    const EX = j.vehicle_extra || {};
    CHARTS[0].setData([t, S.rssi_comb, EX.adapter_count], true);
    CHARTS[1].setData([t, S.per], true);
    CHARTS[2].setData([t, S.mcs, EX.rssi_slope], true);
  } else {
    CHARTS[0].setData([t, S.rssi_comb, S.snr_avg], true);
    CHARTS[1].setData([t, S.per, S.pkt_lost], true);
    CHARTS[2].setData([t, S.fec_rec, S.pkt_lost], true);
    CHARTS[3].setData(mcsStack(t, j.mcs_dist).data, true);
  }
  STATE_BANDS = stateBands(t, S.tier1_state);
  rebuildBands();
}

async function pollOnce() {
  if (!LIVE) { setLiveDot("● paused", "live-paused"); return; }
  // Never disturb an in-progress label selection or a manual zoom.
  if (LABEL_MODE || PENDING || ZOOMED) { setLiveDot("● live (held)", "live-on"); return; }
  let j;
  try {
    const r = await fetch(`/api/session/${SID}/series`);
    if (!r.ok) return;
    j = await r.json();
  } catch (e) { return; }
  if (j.n !== LAST_N) { applySeries(j); LAST_N = j.n; setLiveDot("● live", "live-on"); }
  else setLiveDot("● idle", "live-idle");
}

function startLive() {
  const cb = document.getElementById("live-toggle");
  if (cb) { LIVE = cb.checked; cb.addEventListener("change", () => { LIVE = cb.checked; }); }
  setLiveDot("● live", "live-on");
  setInterval(pollOnce, POLL_MS);
}

async function main() {
  const r = await fetch(`/api/session/${SID}/series`);
  if (!r.ok) { document.getElementById("charts").textContent = "failed to load series"; return; }
  const j = await r.json();
  LAST_N = j.n;
  IS_VEHICLE = !!j.is_vehicle;
  const S = j.series, t = S.t;
  document.getElementById("overlay-status").textContent =
    ` · ${j.n} records · model ${j.model_ver || "(none scored)"}`;
  STATE_BANDS = stateBands(t, S.tier1_state);

  const root = document.getElementById("charts");
  const mk = () => { const d = document.createElement("div"); root.appendChild(d); return d; };

  if (IS_VEHICLE) {
    // Vehicle-uplink is 1 Hz controller STATE, not packet stats: no SNR, no
    // pkt_lost, no per-rung packet mix. Chart what the controller actually
    // tracks — smoothed RSSI + diversity (adapter_count), smoothed loss, and
    // current_mcs as a step line alongside the fade-rate it demotes on.
    const EX = j.vehicle_extra || {};
    CHARTS = [
      // "downlink" = rssi_comb = the GS-relayed downlink score the vehicle
      // adapts on. "uplink" = uplink_rssi = the vehicle's OWN reception of the
      // GS (independent antenna data; null on sessions captured before the
      // uplink_rx feature). When both are present they should diverge.
      makeChart(mk(), "RSSI — downlink (GS-relayed) vs uplink (vehicle rx)",
        [{}, { label: "downlink (dBm)", scale: "rssi", stroke: C.rssi },
             { label: "uplink (dBm)", scale: "rssi", stroke: "#58a6ff",
               spanGaps: true },
             { label: "adapters", scale: "adapters", stroke: C.snr,
               paths: stepped, points: { show: false } }],
        [t, S.rssi_comb, S.uplink_rssi, EX.adapter_count],
        { rssi: {}, adapters: { range: [0, 4] } },
        [{ scale: "rssi", stroke: axisColor, grid: { stroke: "#21262d" } },
         { scale: "adapters", side: 1, stroke: axisColor, grid: { show: false } }]),
      makeChart(mk(), "PER (smoothed loss ratio)",
        [{}, { label: "per", scale: "per", stroke: C.per }],
        [t, S.per], { per: { range: [0, 1] } },
        [{ scale: "per", stroke: axisColor, grid: { stroke: "#21262d" } }]),
      makeChart(mk(), "MCS (current) / RSSI slope",
        [{}, { label: "mcs", scale: "mcs", stroke: C.rssi,
               paths: stepped, points: { show: false } },
             { label: "rssi slope (dB/s)", scale: "slope", stroke: C.lost }],
        [t, S.mcs, EX.rssi_slope], { mcs: { range: [-0.5, 7.5] }, slope: {} },
        [{ scale: "mcs", stroke: axisColor, grid: { stroke: "#21262d" } },
         { scale: "slope", side: 1, stroke: axisColor, grid: { show: false } }]),
    ];
  } else {
    CHARTS = [
      makeChart(mk(), "RSSI / SNR",
        [{}, { label: "rssi_comb (dBm)", scale: "rssi", stroke: C.rssi },
             { label: "snr_avg (dB)", scale: "snr", stroke: C.snr }],
        [t, S.rssi_comb, S.snr_avg], { rssi: {}, snr: {} },
        [{ scale: "rssi", stroke: axisColor, grid: { stroke: "#21262d" } },
         { scale: "snr", side: 1, stroke: axisColor, grid: { show: false } }]),
      makeChart(mk(), "PER / lost",
        [{}, { label: "per", scale: "per", stroke: C.per },
             { label: "pkt_lost", scale: "cnt", stroke: C.lost }],
        [t, S.per, S.pkt_lost], { per: { range: [0, 1] }, cnt: {} },
        [{ scale: "per", stroke: axisColor, grid: { stroke: "#21262d" } },
         { scale: "cnt", side: 1, stroke: axisColor, grid: { show: false } }]),
      // FEC-recovered vs lost: the real "is the video healthy" signal.
      // fec_rec climbing while lost stays ~0 = FEC is absorbing the channel;
      // lost lifting off zero = under-protection / actual video loss.
      makeChart(mk(), "FEC recovered / lost (rx pkts / 100 ms)",
        [{}, { label: "fec_rec", scale: "cnt", stroke: C.snr },
             { label: "pkt_lost", scale: "cnt", stroke: C.lost }],
        [t, S.fec_rec, S.pkt_lost], { cnt: {} },
        [{ scale: "cnt", stroke: axisColor, grid: { stroke: "#21262d" } }]),
      (() => {
        const st = mcsStack(t, j.mcs_dist);
        return makeChart(mk(), "MCS distribution (rx pkts / 100 ms, stacked)",
          st.series, st.data, { pkts: {} },
          [{ scale: "pkts", stroke: axisColor, grid: { stroke: "#21262d" } }]);
      })(),
    ];
  }

  let raf;
  window.addEventListener("resize", () => {
    cancelAnimationFrame(raf);
    raf = requestAnimationFrame(() => {
      for (const u of CHARTS) u.setSize({ width: u.root.parentNode.clientWidth, height: 200 });
    });
  });

  wireForms();
  reloadLabels();
  startLive();
}

main();
