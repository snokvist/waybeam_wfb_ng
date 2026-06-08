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
const axisColor = "#8b949e";

function makeChart(el, title, series, data, scales, axes) {
  const opts = {
    title, width: el.clientWidth, height: 200,
    cursor: { sync: { key: SYNC_KEY }, drag: { x: true, y: false, setScale: true } },
    legend: { live: true },
    scales: scales || {},
    axes: [{ stroke: axisColor, grid: { stroke: "#21262d" }, ticks: { stroke: "#21262d" } }].concat(axes),
    plugins: [bandsPlugin()],
    hooks: { setSelect: [onSelect] },
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

async function main() {
  const r = await fetch(`/api/session/${SID}/series`);
  if (!r.ok) { document.getElementById("charts").textContent = "failed to load series"; return; }
  const j = await r.json();
  const S = j.series, t = S.t;
  document.getElementById("overlay-status").textContent =
    ` · ${j.n} records · model ${j.model_ver || "(none scored)"}`;
  STATE_BANDS = stateBands(t, S.tier1_state);

  const root = document.getElementById("charts");
  const mk = () => { const d = document.createElement("div"); root.appendChild(d); return d; };

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
    makeChart(mk(), "MCS / Tier-1 state",
      [{}, { label: "mcs", scale: "mcs", stroke: C.mcs, paths: stepped },
           { label: "tier1_state", scale: "state", stroke: C.tier1, paths: stepped }],
      [t, S.mcs, S.tier1_state], { mcs: {}, state: { range: [-0.2, 2.2] } },
      [{ scale: "mcs", stroke: axisColor, grid: { stroke: "#21262d" } },
       { scale: "state", side: 1, stroke: axisColor, grid: { show: false } }]),
  ];

  let raf;
  window.addEventListener("resize", () => {
    cancelAnimationFrame(raf);
    raf = requestAnimationFrame(() => {
      for (const u of CHARTS) u.setSize({ width: u.root.parentNode.clientWidth, height: 200 });
    });
  });

  wireForms();
  reloadLabels();
}

main();
