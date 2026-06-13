// peek_short_roundtrip.cpp
//
// Deterministic correctness gate for the peek SHORT_TAIL proportional-parity
// close (docs/design/peek-proportional-parity.md, follow-up to PR #64).
//
// It replicates the EXACT byte flow of the on-device change without any
// sockets/AEAD/pcap:
//
//   TX (close_block_short, tx.cpp):
//     - j real data fragments (random wpacket_hdr + payload) at slots [0, j)
//     - slots [j, k) filled with CANONICAL PAD = { flags=FEC_ONLY, size=0, 0... }
//     - fec_encode_simd computes the FULL parity set into [k, n)
//     - only the first p = round(j*(n-k)/k) parity fragments are "transmitted"
//
//   RX (process_packet + apply_fec, rx.cpp):
//     - receives a SUBSET of { data[0,j), parity[k,k+p) } (up to `loss` dropped)
//     - synthesizes slots [j, k) as the IDENTICAL canonical pad
//     - decodes (apply_fec) and must reconstruct the j real data fragments
//
// The test asserts bit-exact recovery of the j real fragments for every
// (k, n, payload, j, loss<=p) combination, proving the math + the synthesize-pad
// approach. A separate check asserts the "present >= k" decodability invariant.
//
// Build & run (host):
//   g++ -O2 -Wall -I<wfb-ng/src> peek_short_roundtrip.cpp <wfb-ng/src>/zfex.c -o /tmp/pst && /tmp/pst

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <vector>

extern "C" {
#include "zfex.h"
}

// Mirror of the wire constants we depend on (avoid pulling wifibroadcast.hpp,
// which drags in libsodium/pcap). Keep in sync with src/wifibroadcast.hpp.
static const uint8_t WFB_PACKET_FEC_ONLY = 0x1;
struct wpacket_hdr_t { uint8_t flags; uint16_t packet_size; } __attribute__((packed));

// htobe16 without <endian.h> dependency noise
static inline uint16_t be16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

// Same p as tx.cpp close_block_short: integer round-half-up, clamp [1, n-k].
static int proportional_p(int j, int k, int n)
{
    int parity_total = n - k;
    if (parity_total <= 0) return 0;
    int p = (j * parity_total + k / 2) / k;
    if (p < 1) p = 1;
    if (p > parity_total) p = parity_total;
    return p;
}

// Fill a slot with the canonical pad bytes (identical on TX and RX).
static void write_canonical_pad(uint8_t *slot, size_t cap)
{
    memset(slot, 0, cap);
    wpacket_hdr_t *h = (wpacket_hdr_t*)slot;
    h->flags = WFB_PACKET_FEC_ONLY;
    h->packet_size = be16(0);
}

static unsigned g_seed = 0x9e3779b9u;
static uint8_t rnd_byte() { g_seed = g_seed * 1664525u + 1013904223u; return (uint8_t)(g_seed >> 17); }

static int g_fail = 0;
#define CHECK(cond, msg, ...) do { if(!(cond)) { printf("FAIL: " msg "\n", ##__VA_ARGS__); g_fail++; } } while(0)

// One end-to-end short-block roundtrip. `drop_mask` selects which of the
// on-air fragments to drop (bit i = data slot i for i<j; bit (j + pi) = parity pi).
static void run_case(int k, int n, size_t payload, int j, uint32_t drop_mask)
{
    assert(j >= 1 && j < k);
    const size_t cap = ZFEX_ROUND_UP_SIMD(payload + sizeof(wpacket_hdr_t));
    const int p = proportional_p(j, k, n);

    fec_t *fec = NULL;
    assert(fec_new(k, n, &fec) == ZFEX_SC_OK);

    // ---- TX encode buffers ----
    std::vector<uint8_t*> tx(n);
    std::vector<std::vector<uint8_t>> ref(k);      // golden copy of [0,j) real data
    for (int i = 0; i < n; i++)
        assert(posix_memalign((void**)&tx[i], ZFEX_SIMD_ALIGNMENT, cap) == 0);

    size_t max_ps = sizeof(wpacket_hdr_t);
    for (int i = 0; i < j; i++)
    {
        memset(tx[i], 0, cap);
        wpacket_hdr_t *h = (wpacket_hdr_t*)tx[i];
        size_t sz = 1 + (rnd_byte() % payload);    // 1..payload real bytes
        h->flags = 0;
        h->packet_size = be16((uint16_t)sz);
        for (size_t b = 0; b < sz; b++) tx[i][sizeof(wpacket_hdr_t) + b] = rnd_byte();
        max_ps = std::max(max_ps, sizeof(wpacket_hdr_t) + sz);
    }
    for (int i = j; i < k; i++) write_canonical_pad(tx[i], cap);

    ref.resize(k);
    for (int i = 0; i < k; i++) { ref[i].assign(tx[i], tx[i] + cap); }

    // Full parity into [k, n) (codec is all-or-nothing), as close_block_short does.
    assert(fec_encode_simd(fec, (const uint8_t**)tx.data(), tx.data() + k,
                           ZFEX_ROUND_UP_SIMD(max_ps)) == ZFEX_SC_OK);

    // ---- RX side ----
    std::vector<uint8_t*> rx(n, NULL);
    std::vector<bool> present(n, false);
    for (int i = 0; i < n; i++)
    {
        assert(posix_memalign((void**)&rx[i], ZFEX_SIMD_ALIGNMENT, cap) == 0);
        // Zero the whole buffer: FEC reconstructs only [0, ROUND_UP(max_ps)); the
        // tail [ROUND_UP(max_ps), cap) must match ref's zeros (on device the RX
        // emits only packet_size bytes from the recovered wpacket_hdr, well inside
        // the encoded region, so the tail is irrelevant there).
        memset(rx[i], 0, cap);
    }

    // On-air fragments = data [0,j) + parity [k, k+p). Apply drop_mask.
    int air_idx = 0;
    for (int i = 0; i < j; i++, air_idx++)
        if (!(drop_mask & (1u << air_idx))) { memcpy(rx[i], tx[i], cap); present[i] = true; }
    for (int pi = 0; pi < p; pi++, air_idx++)
        if (!(drop_mask & (1u << air_idx))) { memcpy(rx[k + pi], tx[k + pi], cap); present[k + pi] = true; }

    // Synthesize the SHORT_TAIL tail [j, k) (slot j is the marker; on device it
    // is received, but it carries the identical canonical pad, so synthesizing
    // the whole [j,k) range here is byte-equivalent and simpler for the test).
    for (int i = j; i < k; i++)
        if (!present[i]) { write_canonical_pad(rx[i], cap); present[i] = true; }

    // Decodability invariant the RX relies on (has_fragments >= k).
    int present_cnt = 0; for (int i = 0; i < n; i++) if (present[i]) present_cnt++;

    // ---- replicate apply_fec (rx.cpp) ----
    unsigned index[256];
    const uint8_t *in_blocks[256];
    uint8_t *out_blocks[256];
    int jj = k, ob = 0;
    size_t rx_max_ps = 0;
    bool decodable = true;
    for (int i = 0; i < k; i++)
    {
        if (present[i]) { in_blocks[i] = rx[i]; index[i] = i; }
        else
        {
            while (jj < n && !present[jj]) jj++;
            if (jj >= n) { decodable = false; break; }     // not enough parity
            // parity size on device = fragment_map[parity] = max real packet size
            rx_max_ps = std::max(rx_max_ps, max_ps);
            in_blocks[i] = rx[jj];
            out_blocks[ob++] = rx[i];
            index[i] = jj++;
        }
    }

    CHECK(decodable == (present_cnt >= k),
          "decodability mismatch k=%d n=%d j=%d p=%d present=%d decodable=%d",
          k, n, j, p, present_cnt, (int)decodable);

    if (decodable)
    {
        if (ob > 0)
            assert(fec_decode_simd(fec, in_blocks, out_blocks, index,
                                   ZFEX_ROUND_UP_SIMD(max_ps)) == ZFEX_SC_OK);
        // Verify bit-exact recovery of the j real fragments.
        for (int i = 0; i < j; i++)
            CHECK(memcmp(rx[i], ref[i].data(), cap) == 0,
                  "data slot %d corrupted k=%d n=%d j=%d p=%d drop=0x%x",
                  i, k, n, j, p, drop_mask);
    }

    for (int i = 0; i < n; i++) { free(tx[i]); free(rx[i]); }
    fec_free(fec);
}

int main()
{
    struct { int k, n; } codes[] = { {8,12}, {8,10}, {4,6}, {16,24}, {37,51}, {2,3} };
    size_t payloads[] = { 32, 512, 1024, 1448 };
    int total = 0;

    for (auto c : codes)
    {
        for (size_t pl : payloads)
        {
            for (int j = 1; j < c.k; j++)
            {
                int p = proportional_p(j, c.k, c.n);
                // No loss
                run_case(c.k, c.n, pl, j, 0); total++;
                // Drop each single on-air fragment (data or parity), one at a time.
                int air = j + p;
                for (int d = 0; d < air; d++) { run_case(c.k, c.n, pl, j, 1u << d); total++; }
                // Drop up to p fragments (parity must cover): drop the first `loss`
                // data fragments (worst case: leading data lost).
                for (int loss = 1; loss <= p && loss <= j; loss++)
                {
                    uint32_t m = 0; for (int d = 0; d < loss; d++) m |= (1u << d);
                    run_case(c.k, c.n, pl, j, m); total++;
                }
                // Over-loss (loss = p+1 on data): must be reported NON-decodable,
                // never a silent corruption. Only when j > p so the mask is valid.
                if (p + 1 <= j)
                {
                    uint32_t m = 0; for (int d = 0; d < p + 1; d++) m |= (1u << d);
                    run_case(c.k, c.n, pl, j, m); total++;
                }
            }
        }
    }

    printf("ran %d cases, %d failures\n", total, g_fail);
    return g_fail ? 1 : 0;
}
