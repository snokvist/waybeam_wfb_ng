# HT40 Bandwidth Coordination — link_controller budget + live switch — Requirements / Test Spec

Status: **HOST-VERIFIED 2026-06-16** (phy_mbps scaling + live-switch plumbing on a
GS-only box); **gs+air combo test PENDING** (needs a live vehicle downlink so the
FEC controller commits k/n and reports `computed_safe_kbps`).
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

## 3. The gs+air combo test — TO RUN LATER

**Goal:** prove end-to-end on a live link that flipping to HT40 (both ends) ~doubles
the vehicle `link_controller`'s `computed_safe_kbps` **and** the sustained video
bitrate, with no loss/flap, and that reverting restores the HT20 baseline.

**Rig:** GS (this x86 host or an RK3566) + vehicle (`wfb-air` on Star6E `.13`/`.10`),
both on the same channel (e.g. ch161). Vehicle runs the video downlink `wfb_tx` +
`link_controller`; GS runs `wfb-gs` + `wfb_rx`.

**Ordering rule (critical — avoids the §4 desync):**
- **Switch up:** set `iw … HT40+` on **both ends first** (cards can carry 40 MHz),
  *then* `SET_RADIO bandwidth=40` (radiotap follows the channel).
- **Switch down:** `SET_RADIO bandwidth=20` **first**, *then* `iw … set channel <ch>`
  (HT20). Never leave radiotap=40 on a 20 MHz channel.

**Steps:**
1. **Baseline (HT20):** capture vehicle `link_controller /status` →
   `radio.phy_mbps`, `fec.computed_safe_kbps`, `fec.last_bitrate_kbps`, PER /
   `fec_recovered`; confirm clean video on `:5600`.
2. **Vehicle iw:** `iw dev <veh-mon> set channel <ch> HT40+`.
3. **GS iw:** `iw dev <gs-mon...> set channel <ch> HT40+` on **every** GS RX card
   (width must match to decode).
4. **Radiotap:** `SET_RADIO bandwidth=40` on the vehicle **video** tx (over its
   `wfb_air` control port; or via a GS-side proxy if exposed).
5. **Observe (vehicle /status):** `phy_mbps` ~2.1×; `computed_safe_kbps` ~2.1×;
   `last_bitrate_kbps` climbs toward the new ceiling within `bitrate_grace_s`;
   PER stays low; **no MCS flap**, no failsafe.
6. **Soak 60–120 s:** video clean, no `fec_skip` storm, no SHM ring backpressure
   (judge by `fec_skip`/ring-fill, not avg overhead — cf. peek findings).
7. **Revert** (down-order above); confirm HT20 baseline restored, video clean.

**Pass criteria:**
- `computed_safe_kbps(HT40) / computed_safe_kbps(HT20)` ∈ **[1.9, 2.2]**.
- Sustained video bitrate rises toward the new ceiling within `bitrate_grace_s`.
- No link flap, no failsafe trip, PER delta within noise.
- Clean revert to HT20 with video intact throughout.

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

- `test/ht40_host_test.sh` — the host-only harness from this session: flips
  `iw`+`SET_RADIO` to HT40, runs a native dry-run `link_controller` to read back
  `phy_mbps`, and **auto-reverts to HT20 on any exit** (trap). Reusable as the
  **GS-side half** of the combo test. Build the native controller first with
  `make -C vehicle host` → `vehicle/build/link_controller.host`.

---

## 6. Code reference index

| Concern | Location |
|---|---|
| `phy_mbps` bw_scale (2.1× @ 40) | `vehicle/link_controller.c:998–1014` |
| budget formula | `bitrate_assert` `:1218–1228`; `computed_safe_kbps` `:3814–3826` |
| radio bootstrap / refresh | `radio_apply_observation` `:1090`; GET_RADIO seed `:5755` |
| WCMD bandwidth key | `WCMD_KEY_WFB_BANDWIDTH` set `:2296`; allow-mask bit 7 (128) `:3463` |
| `RadioBody` wire (7 bytes) | `:180`; `shared/wfb_control.h` (`WFB_CMD_SET_RADIO 2`, `GET_RADIO 4`) |
| wfb_tx radiotap rebuild | `wfb-ng/.../src/tx.cpp:1202` (CMD_SET_RADIO), `:1628–1638` (BW flags) |
| supervisor set_radio action | `ground/gs_supervisor_http.c:1122` (control), `:1176–1235` |
| iw width in GS config | `ground/config/host_x86.json` `system.up`; `init/wfb-link.example.json` `htmode` |
| CSA `iw set channel` | `ground/gs_supervisor_csa.c:123` `run_iw_set_channel`; `vehicle/csa/csa.c:140` |
