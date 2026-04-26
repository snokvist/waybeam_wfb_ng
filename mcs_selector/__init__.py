"""mcs_selector — RSSI/loss-driven MCS selector for wfb_tx.

Standalone module. Consumes wfb_rx -Y JSON datagrams (rx_ant ver 1) and
emits CMD_SET_RADIO updates to wfb_tx via the binary control protocol
(tx_cmd.h). Independent of fec_controller; the FEC controller reacts to
externally-applied MCS changes on its own.
"""
