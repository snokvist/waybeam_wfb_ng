# HT40 Bandwidth Coordination — link_controller budget + live switch — Requirements / Test Spec

Status: **COMBO-VERIFIED 2026-06-16** — host (phy_mbps scaling + live-switch
plumbing) **and** a live gs+air link (vehicle `link_controller` budget). The HT40
budget accounting is **correct and live-verified (2.10×/MCS)**; the end-to-end
throughput-doubling criterion **did not pass on this rig** — HT40 degraded the
GS-side RX so the MCS controller demoted and net `safe_kbps` *fell*. See §3.1
for the combo run. The controller behaved correctly: loss feedback demoted MCS
rather than over-driving the 2× budget.
Date: 2026-06-16
Branch: `spec/ht40-coordinated-test`
Depends on: `link_controller` budget law (on `main`, `ee230b1`); the mega-binary
            `wfb-gs`/`wfb-air` supervisor + `wfb_tx` CMD_SET_RADIO (PR #78/#79).

---

## 1. The question this answers

> Does the link controller account for HT40's doubled bandwidth, and can HT20/HT40
> be live-switched (wfb_cmd + `iw`)?

**Yes on both, with one structural caveat (a two-layer desync risk, §4).** This doc
records what was proven on the host, the code paths involved, and the coordinated
gs+air procedure to finish the verification on a live link.

---

## 2. What is already confirmed (code + host)

### 2.1 The budget scales with HT40 — automatically, no special-casing

The FEC bitrate budget is derived entirely from `radio->phy_mbps`:

```
safe_kbps = phy_mbps × 1000 × (k/n) × safety_margin
```

`phy_mbps()` (`vehicle/link_controller.c:998`) scales the base HT rate by bandwidth —
the **only** bandwidth-dependent term:

```c
case 40:  bw_scale = 2.1f; break;   // HT40 ≈ 2.077× HT20 (108 vs 52 data subcarriers)
case 80:  bw_scale = 4.5f; break;
case 160: bw_scale = 9.0f; break;
default:  bw_scale = 1.0f; break;   // HT20
```

Both consumers use it: `bitrate_assert()` (`:1227`) and the WebUI/SSE
`computed_safe_kbps` (`:3814`). `k/n` and `safety_margin` (default 0.5) are
bandwidth-independent, so for any fixed FEC ratio the ceiling scales ~2.1× with HT40.

Bandwidth is sourced from the **radiotap radio state**, not the OS channel:
bootstrapped via `CMD_GET_RADIO` at startup (`radio_apply_observation`, call site
`:5758`) and refreshed on every external `SET_RADIO`. The adaptive MCS law **never
changes bandwidth** — HT40 is a coordinated manual/config switch the controller
observes and budgets for.

### 2.2 Live switch is two independent layers (both required)

| Layer | What it does | How to set | Code |
|---|---|---|---|
| **radiotap TX width** | per-frame injected width (BW_40 flag) | wfb_cmd `CMD_SET_RADIO bandwidth=40` | `wfb-ng/.../tx.cpp:1202` (handler) → `:1635` `IEEE80211_RADIOTAP_MCS_BW_40` |
| **`iw` channel width** | actual RF channel + secondary offset | `iw dev <mon> set channel <ch> HT40+` | `gs_supervisor_csa.c:123` `run_iw_set_channel`; `vehicle/csa/csa.c:140` |

`wfb_tx` rebuilds its radiotap header **live** on `CMD_SET_RADIO` (`tx.cpp:1213`),
no restart. Reachable on the GS via the supervisor:

```
GET /api/v1/tunnels/<tunnel>/control?cmd=set_radio&bandwidth=40
```
(read-modify-write; only overrides passed fields — `gs_supervisor_http.c:1122`,
dispatch at `:1176`, `WFB_CMD_SET_RADIO` build at `:1214`). Or `wfb_tx_cmd`
directly against the control port (`-C 8000`).

### 2.3 Host verification — 2026-06-16, dev x86 GS, dual RTL88x2 (live `wfb-gs`)

Cards `wlx40a5ef2f229b` + `wlx40a5ef2f2308`, both ch161, uplink `wfb_tx -i 208 -C 8000`.

| Check | HT20 | → HT40 | Result |
|---|---|---|---|
| `iw … set channel 161 HT40+` | width 20 MHz | **width 40 MHz, center1 5815** | ✓ both cards |
| `SET_RADIO bandwidth=40` | `bw=20` | **`bw=40`**, tx **stayed running** | ✓ no crash |
| controller `phy_mbps` (MCS1) | **13.0** | **27.3** | **2.10×** ✓ |
| revert (`bandwidth=20` + bare `set channel 161`) | — | back to 20 MHz / `bw=20` | ✓ clean |

**Why `computed_safe_kbps` read `None` standalone:** that field only populates after
the FEC subsystem commits a k/n, which needs the **probe (`--probe`) + venc sidecar +
live rx_ant stats**. In FEC-only mode the rx_ant socket isn't bound (`stats_port=0`);
MCS mode hard-requires `--probe`. A GS-only box has none of these — the FEC
controller runs **vehicle-side**. So the kbps doubling is provable by the formula
(only `phy_mbps` moves) but is *observed* live only in the combo test below.

---

## 3. The gs+air combo test — RUN 2026-06-16 (results in §3.1)

**Goal:** prove end-to-end on a live link that flipping to HT40 (both ends) ~doubles
the vehicle `link_controller`'s `computed_safe_kbps` **and** the sustained video
bitrate, with no loss/flap, and that reverting restores the HT20 baseline.

**Rig:** GS (this x86 host or an RK3566) + vehicle (`wfb-air` on Star6E `.13`/`.10`),
both on the same channel (e.g. ch161). Vehicle runs the video downlink `wfb_tx` +
`link_controller`; GS runs `wfb-gs` + `wfb_rx`.

**Ordering rule (critical — avoids the §4 desync):**
- **Switch up:** set `iw … HT40+` on **both ends first** (cards can carry 40 MHz),
  *then* `SET_RADIO bandwidth=40` (radiotap follows the channel).
- **Switch down:** WCMD `wfb_bandwidth=20` **first**, *then* `iw … set channel <ch>`
  (bare = HT20). Never leave radiotap=40 on a 20 MHz channel.

**Status JSON paths** (verified live — note they differ from earlier guesses):
`radio.bw`, `radio.mcs`, `radio.phy_mbps`; `wfb.computed_safe_kbps`,
`wfb.last_bitrate_kbps`, `wfb.last_set_fec_k/n`; `score.smoothed_lost_ratio`,
`score.smoothed_rssi`; `mcs.flap_guard.frozen`. Served at
`http://127.0.0.1:8765/status` (the `wfb-air link` applet, **not** `/api/v1/...`).

**Step 4 — set bandwidth via the controller's WCMD, NOT a raw `SET_RADIO`.**
With adaptive MCS active, a raw `SET_RADIO bandwidth=40` straight to the video tx
is *unsafe*: the controller's next autonomous MCS commit rebuilds the radiotap
from its **cached** `radio_body` (`commit_mcs_change`, `:2690`), which still says
bw=20 → it silently reverts you to 20. The WCMD path is race-free: the controller
read-modify-writes the live tx (`:2286`) **and adopts the result into its cache**
(`:6464–6504`, "so future commit bodies see it instead of reverting from a stale
cache"), so phy_mbps recomputes and subsequent MCS commits keep bw=40.

**Steps:**
1. **Baseline (HT20):** capture vehicle `/status` → `radio.phy_mbps`,
   `wfb.computed_safe_kbps`, `wfb.last_bitrate_kbps`, `score.smoothed_lost_ratio`;
   confirm clean video on `:5600`.
2. **Vehicle iw:** `iw dev wlan0 set channel <ch> HT40+`.
3. **GS iw:** `iw dev <gs-mon...> set channel <ch> HT40+` on **every** GS RX card
   (width must match to decode the downlink).
4. **Radiotap (via WCMD):** from the GS,
   `GET /api/v1/cmd?key=wfb_bandwidth&value=40` (key 8; range `[5,80]`, accepted
   as-is). Emits up the uplink; the controller applies it live + caches it.
5. **Observe (vehicle /status):** `phy_mbps` ~2.1× **at matched MCS**;
   `computed_safe_kbps` ~2.1× **only if MCS holds**; PER stays low; **no MCS flap**.
6. **Soak 60–120 s:** video clean, no `fec_skip` storm, no SHM ring backpressure
   (judge by `fec_skip`/ring-fill, not avg overhead — cf. peek findings).
7. **Revert** (down-order above); confirm HT20 baseline restored, video clean.

**Pass criteria:**
- `computed_safe_kbps(HT40) / computed_safe_kbps(HT20)` ∈ **[1.9, 2.2]**.
- Sustained video bitrate rises toward the new ceiling within `bitrate_grace_s`.
- No link flap, no failsafe trip, PER delta within noise.
- Clean revert to HT20 with video intact throughout.

### 3.1 Combo run — 2026-06-16, dev x86 GS (dual RTL88x2) ↔ vehicle `.13` (imx335, bench)

Live `wfb-air` rig on `.13`: `waybeam` venc + video tx (`-M 2 -B 20 -k 8 -n 12 -C 8000
--peek-profile close -i 207`) + probe tx (`-i 50 -C 8001`) + `wfb-air link`
(`--probe 127.0.0.1:8001 --stats 0.0.0.0:5801`, adaptive MCS + FEC). GS `wfb-gs`
RX both cards. Runner: `test/ht40_combo_test.sh` (ordering rule + trap-revert).

| t | bw | mcs | phy_mbps | safe_kbps | lost | rssi | note |
|---|---|---|---|---|---|---|---|
| baseline | 20 | 5 | 52.0 | **18200** | 0.0000 | −12 | k/n 21/30, sm 0.5 → 52000·0.7·0.5 = 18200 ✓ |
| HT40 t+4s | 40 | 0 | 13.65 | 4343 | **0.273** | −15 | RX collapsed the instant radiotap=40 |
| HT40 t+20s | 40 | 1 | **27.3** | 8190 | 0.014 | −15 | **matched-MCS check: 27.3 / 13.0(@mcs1,HT20) = 2.10×** |
| HT40 t+32s | 40 | 0 | 13.65 | 3102 | 0.137 | −15 | oscillating, MCS pinned low |
| reverted | 20 | 5 | 52.0 | 18121 | 0.0000 | −12 | full recovery in <20 s |

**Verdict — budget law: PASS. End-to-end doubling: FAIL (physical link, not controller).**

- **`phy_mbps` scales exactly 2.10× with HT40 at matched MCS** — confirmed live
  (mcs1 HT20=13.0 → HT40=27.3). The deterministic claim of §2.1 holds end-to-end on
  a real vehicle link, not just dry-run. The original question — *does the
  controller account for HT40's doubled bandwidth?* — is a definitive **yes**.
- **The pass criterion (`safe_kbps` ratio ∈ [1.9, 2.2]) was NOT met**, because the
  GS-side RX degraded badly at HT40: `lost` spiked to **0.27** and RSSI fell ~3 dB
  (−12 → −15). The MCS controller **correctly demoted 5 → 0/1**, so net `safe_kbps`
  *fell* (18200 → ~4–8k) rather than doubling.
- **Root cause is the GS RTL88x2 monitor RX, not the budget.** The vehicle TX'd
  HT40 fine; `lost` is reported back via rx_ant stats, i.e. it is the GS `wfb_rx`
  failing to decode ~27% of 40 MHz frames. −3 dB is the textbook HT40 noise-
  bandwidth penalty, but 27% loss at −15 dBm on a bench is **poor HT40 monitor-mode
  RX on these cards** (a known RTL88x2 class — cf. memory `rtl88x2cu_cooperative_rx`).
- **The §4 desync did NOT bite** — both layers were flipped together per the
  ordering rule. The controller's loss-feedback demotion is what kept it safe; it
  never over-drove the 2× budget into the wall.

**Practical conclusion:** HT40 is *not* a free 2× on this hardware. It pays off only
on a link with enough SNR margin to hold the **same MCS** at HT40's higher noise
floor (rule of thumb: ≥ ~6 dB headroom above the MCS threshold). Below that, MCS
demotion erases the bandwidth gain — and the controller is right to demote. Re-run
on a link with real margin (longer range / better antennas / a cleaner band, or
RTL88x2EU/AX cards with better HT40 RX) before treating HT40 as a throughput lever.

---

## 4. The structural caveat — radiotap↔iw desync

`link_controller` trusts the **radiotap `bandwidth`** as ground truth for available
airtime, but real capacity depends on the **`iw` channel width**, set by a *different*
mechanism. If `SET_RADIO bandwidth=40` is applied while the card stays on a bare
20 MHz channel, the controller computes a **2.1× ceiling the link cannot carry** →
over-drive → loss. The two must flip together, on both ends.

**Open decisions (for a follow-up phase, not this test):**
- **Who owns the `iw` width in production?** Today `system.up` sets it statically
  with a bare `set channel <ch>` (= HT20) — see `ground/config/host_x86.json`.
  `init/wfb-link.example.json` *already* carries `"htmode":"HT20"` but it is not
  wired through. **Recommended:** plumb `radio.htmode` → (a) the `iw set channel …
  <htmode>` call, (b) the `wfb_tx -B <bw>` arg, and (c) the `SET_RADIO` default, so
  one config field moves all three coherently.
- **Desync guard?** Should `link_controller` refuse the 2× budget when iw width and
  radiotap disagree? It can't read `iw` directly; the supervisor could surface the
  actual width in stats (Phase 2 idea).
- **HT40+ vs HT40-:** channel-dependent (ch161 → HT40+, center 5815, pairs with 165;
  ch149 → HT40-). The htmode must be channel-correct. `vehicle/csa/csa_mux.py`
  already enumerates HT40+/HT40-.

---

## 5. Artifacts

- `test/ht40_combo_test.sh` — the **gs+air combo runner used for §3.1**. From the
  GS host: snapshots the vehicle `/status` baseline, sets `iw HT40+` on the vehicle
  wlan0 + both GS cards, emits WCMD `wfb_bandwidth=40` over the uplink, polls the
  vehicle budget for ~32 s, then **auto-reverts on any exit** (trap: WCMD bw=20
  first, then `iw` HT20 both ends). Env-tunable: `GS_CARDS`, `VEH`, `CH`. Needs
  non-interactive `sudo` for `iw` on the GS and key-based root ssh to the vehicle.
- `test/ht40_host_test.sh` — the host-only harness: flips `iw`+`SET_RADIO` to HT40,
  runs a native dry-run `link_controller` to read back `phy_mbps`, and
  **auto-reverts to HT20 on any exit** (trap). The GS-side half / no-vehicle
  fallback. Build the native controller first with `make -C vehicle host` →
  `vehicle/build/link_controller.host`.

---

## 6. Code reference index

| Concern | Location |
|---|---|
| `phy_mbps` bw_scale (2.1× @ 40) | `vehicle/link_controller.c:998–1014` |
| budget formula | `bitrate_assert` `:1218–1228`; `computed_safe_kbps` `:3814–3826` |
| radio bootstrap / refresh | `radio_apply_observation` `:1090`; GET_RADIO seed `:5755` |
| WCMD bandwidth key | `WCMD_KEY_WFB_BANDWIDTH` = 8 (`shared/wcmd_proto.h:68`); clamp `[5,80]` `:1925–1928`, default `:5227`; apply (read-modify-write) `:2286–2306` |
| WCMD result → cache adopt | main-loop adopts `radio_written_body` into `radio_body` `:6464–6504` (keeps autonomous MCS commits coherent) |
| autonomous MCS commit (uses cache) | `commit_mcs_change` `:2676–2718` (`r = *current_radio`, only `mcs_index` set); emit site `:6058–6070` |
| GS WCMD emit route | `ground/gs_supervisor_http.c:450` `/api/v1/cmd?key=wfb_bandwidth&value=`; key string `ground/gs_supervisor.c:2369` |
| `RadioBody` wire (7 bytes) | `:180`; `shared/wfb_control.h` (`WFB_CMD_SET_RADIO 2`, `GET_RADIO 4`) |
| wfb_tx radiotap rebuild | `wfb-ng/.../src/tx.cpp:1202` (CMD_SET_RADIO), `:1628–1638` (BW flags) |
| supervisor set_radio action | `ground/gs_supervisor_http.c:1122` (control), `:1176–1235` |
| iw width in GS config | `ground/config/host_x86.json` `system.up`; `init/wfb-link.example.json` `htmode` |
| CSA `iw set channel` | `ground/gs_supervisor_csa.c:123` `run_iw_set_channel`; `vehicle/csa/csa.c:140` |
