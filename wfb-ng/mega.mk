# wfb-ng/mega.mk — shared mega-binary (multi-call) build rules.
#
# Included by ground/Makefile and vehicle/Makefile. The wfb-ng tool objects
# and the multi-call dispatcher compile identically on both sides; only the
# compiler differs (host vs. cross), so each side sets MEGA_CC / MEGA_CXX
# before the include. See docs/design/mega-binary.md.
#
# The including Makefile MUST define, before `include`:
#   MEGA_CC, MEGA_CXX   C / C++ compilers (host cc/g++, or the cross pair)
#   MEGA_OBJ            object output directory
#   WFBNG_SRC           patched wfb-ng src/ tree (build-*.sh output)
#   MULTICALL           path to the multicall/ dir
#   MEGA_DEFS           -DWFB_MULTICALL -DWFB_WITH_WFBNG
#   MEGA_CFLAGS         extra include / sysroot flags (sodium, pcap)
#   MEGA_WFB_DEFS       -DWFB_VERSION=...
# and provide its own per-side applet table (gs_applets.o / air_applets.o),
# daemon objects, and the final link rule (this fragment builds neither a
# daemon object nor the executable — only the shared wfb-ng + dispatcher .o).

# wfb-ng tool objects: the four with a main() are renamed to *_main via
# -Dmain=; the rest carry no main and compile plain.
MEGA_WFBNG_OBJ = $(MEGA_OBJ)/rx.o $(MEGA_OBJ)/tx.o $(MEGA_OBJ)/tx_cmd.o \
                 $(MEGA_OBJ)/keygen.o $(MEGA_OBJ)/peek.o $(MEGA_OBJ)/zfex.o \
                 $(MEGA_OBJ)/wifibroadcast.o $(MEGA_OBJ)/radiotap.o \
                 $(MEGA_OBJ)/venc_ring.o $(MEGA_OBJ)/wfb_keyseed.o

# Auto-generate header dependencies (-MMD -MP) so editing a vendored wfb-ng
# header rebuilds the objects that include it; the rules below list only the
# source as an explicit prerequisite. -include the .d files emitted alongside
# each .o (absent on the first build, which compiles everything anyway).
MEGA_DEP = -MMD -MP

$(MEGA_OBJ):
	@mkdir -p $@

# --- dispatcher (C++, identical on both sides) ---
$(MEGA_OBJ)/wfb_multicall.o: $(MULTICALL)/wfb_multicall.cpp $(MULTICALL)/wfb_multicall.h | $(MEGA_OBJ)
	$(MEGA_CXX) -std=gnu++11 -O2 -Wall $(MEGA_DEP) $(MEGA_DEFS) $(MEGA_CFLAGS) -I$(MULTICALL) -c -o $@ $<

# --- deterministic bring-up key derivation (C, links libsodium) ---
$(MEGA_OBJ)/wfb_keyseed.o: $(MULTICALL)/wfb_keyseed.c $(MULTICALL)/wfb_keyseed.h | $(MEGA_OBJ)
	$(MEGA_CC) -std=gnu99 -O2 -Wall $(MEGA_DEP) $(MEGA_DEFS) $(MEGA_CFLAGS) -I$(MULTICALL) -c -o $@ $<

# --- wfb-ng tools (main() renamed to applet entry via -Dmain=) ---
$(MEGA_OBJ)/rx.o:     $(WFBNG_SRC)/rx.cpp           | $(MEGA_OBJ)
	$(MEGA_CXX) -std=gnu++11 -O2 -Wall -fno-strict-aliasing $(MEGA_DEP) $(MEGA_WFB_DEFS) -Dmain=wfb_rx_main     $(MEGA_CFLAGS) -c -o $@ $<
$(MEGA_OBJ)/tx.o:     $(WFBNG_SRC)/tx.cpp           | $(MEGA_OBJ)
	$(MEGA_CXX) -std=gnu++11 -O2 -Wall -fno-strict-aliasing $(MEGA_DEP) $(MEGA_WFB_DEFS) -Dmain=wfb_tx_main     $(MEGA_CFLAGS) -c -o $@ $<
$(MEGA_OBJ)/tx_cmd.o: $(WFBNG_SRC)/tx_cmd.c         | $(MEGA_OBJ)
	$(MEGA_CC)  -std=gnu99   -O2 -Wall -fno-strict-aliasing $(MEGA_DEP) $(MEGA_WFB_DEFS) -Dmain=wfb_tx_cmd_main $(MEGA_CFLAGS) -c -o $@ $<
$(MEGA_OBJ)/keygen.o: $(WFBNG_SRC)/keygen.c         | $(MEGA_OBJ)
	$(MEGA_CC)  -std=gnu99   -O2 -Wall -fno-strict-aliasing $(MEGA_DEP) $(MEGA_WFB_DEFS) -Dmain=wfb_keygen_main $(MEGA_CFLAGS) -c -o $@ $<
$(MEGA_OBJ)/peek.o:           $(WFBNG_SRC)/peek.cpp          | $(MEGA_OBJ)
	$(MEGA_CXX) -std=gnu++11 -O2 -Wall -fno-strict-aliasing $(MEGA_DEP) $(MEGA_WFB_DEFS) $(MEGA_CFLAGS) -c -o $@ $<
$(MEGA_OBJ)/wifibroadcast.o:  $(WFBNG_SRC)/wifibroadcast.cpp | $(MEGA_OBJ)
	$(MEGA_CXX) -std=gnu++11 -O2 -Wall -fno-strict-aliasing $(MEGA_DEP) $(MEGA_WFB_DEFS) $(MEGA_CFLAGS) -c -o $@ $<
$(MEGA_OBJ)/zfex.o:           $(WFBNG_SRC)/zfex.c            | $(MEGA_OBJ)
	$(MEGA_CC)  -std=gnu99   -O2 -Wall -fno-strict-aliasing $(MEGA_DEP) $(MEGA_WFB_DEFS) $(MEGA_CFLAGS) -c -o $@ $<
$(MEGA_OBJ)/radiotap.o:       $(WFBNG_SRC)/radiotap.c        | $(MEGA_OBJ)
	$(MEGA_CC)  -std=gnu99   -O2 -Wall -fno-strict-aliasing $(MEGA_DEP) $(MEGA_WFB_DEFS) $(MEGA_CFLAGS) -c -o $@ $<
$(MEGA_OBJ)/venc_ring.o:      $(WFBNG_SRC)/venc_ring.c       | $(MEGA_OBJ)
	$(MEGA_CC)  -std=gnu99   -O2 -Wall -fno-strict-aliasing $(MEGA_DEP) $(MEGA_WFB_DEFS) $(MEGA_CFLAGS) -c -o $@ $<

# Pull in the auto-generated header deps (no-op until the .d files exist).
-include $(MEGA_WFBNG_OBJ:.o=.d) $(MEGA_OBJ)/wfb_multicall.d
