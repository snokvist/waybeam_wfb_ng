# archive/init-old/ — superseded ground-side init scripts

Pre-`gs_supervisor` ground-station bring-up scripts.  Replaced by
running `gs_supervisor` from a single systemd unit / OpenWRT init
script that drives everything from JSON config.

| File | What it did | Replacement |
|---|---|---|
| `wbmode` | Toggle wlan adapter between AP / monitor / down via `nmcli` + `iw` | `gs_supervisor.example.json` `system.up` / `system.down` arrays |
| `wfb-ng.sh` | Manual bring-up shell script for `wfb_rx` + `wfb_tx` on ground | `gs_supervisor` tunnel autostart |
| `wfb-ng.init` | OpenWRT procd-style init wrapper around `wfb-ng.sh` | OpenWRT init wrapping `gs_supervisor` |

Vehicle-side init (`S99wfb`) still lives in `vehicle/init/` because
the vehicle runs `link_controller` directly — no supervisor wrapper
on that side by design.

Kept here for reference if anyone needs to debug a legacy deployment
that didn't migrate to gs_supervisor.
