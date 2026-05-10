# probes/

Host-native development probes.  Run on a laptop next to a live setup
to characterise the pipeline; **not** shipped to vehicle or GS.

## rtp_timing_probe

Binds to the venc RTP sidecar (`MSG_SUBSCRIBE` per `shared/rtp_sidecar.h`),
captures one MSG_FRAME per encoded frame, and prints inter-frame timing.

```bash
cd probes/
make
./build/rtp_timing_probe <vehicle-ip> <sidecar-port>
```

Wire format lives in `shared/rtp_sidecar.h` (vendored from waybeam_venc).
