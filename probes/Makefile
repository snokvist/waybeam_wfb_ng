# Build host-native dev probes (RTP timing, sidecar inspection, etc.).
#
# These are NOT shipped to the vehicle or GS; they're development /
# regression aids the operator runs on a laptop while diagnosing the
# pipeline.  Cross-compiles are not maintained here.

BUILD_DIR ?= build
CC        ?= cc
CFLAGS    ?= -std=gnu99 -O2 -Wall -Wextra
LDFLAGS   ?= -lm

PROBES = $(BUILD_DIR)/rtp_timing_probe

.PHONY: all clean

all: $(PROBES)

$(BUILD_DIR):
	@mkdir -p $@

$(BUILD_DIR)/rtp_timing_probe: rtp_timing_probe.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I../shared -o $@ $< $(LDFLAGS)

clean:
	rm -f $(PROBES)
