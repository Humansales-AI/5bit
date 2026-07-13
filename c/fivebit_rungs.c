/* fivebit_rungs.c — the 5 x intN ladder (width-agnostic transport)
 *
 * Primitive identity: 5 units of intN carry exactly N tokens.
 * Composition: any count n packs exactly as binary-decomposed rung
 * segments (largest first), zero format slack: 13 = 8+4+1.
 *
 * Invariants (mirrors griddb_rungs.py; conformance is token-level):
 *   I1 round-trip per rung        I2 width equivalence
 *   I3 bit-identity with the reference byte packer
 *
 * Build:  cc -O2 -o fivebit_rungs fivebit_rungs.c
 * Test:   ./fivebit_rungs selftest        (prints vectors; exit 0 on pass)
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

typedef unsigned __int128 acc_t;   /* N up to 64 needs > 64-bit headroom */

/* ---- single rung: N tokens -> 5 units of intN (MSB-first) ---- */
static int pack_units(const uint8_t *tk, int n_tok, int N, uint64_t *units) {
    if (n_tok % N) return -1;
    acc_t acc = 0; int nbits = 0, out = 0;
    uint64_t mask = (N == 64) ? ~0ULL : ((1ULL << N) - 1);
    for (int i = 0; i < n_tok; i++) {
        acc = (acc << 5) | (tk[i] & 0x1F); nbits += 5;
        while (nbits >= N) { nbits -= N; units[out++] = (uint64_t)(acc >> nbits) & mask; }
    }
    return out;                                   /* nbits == 0 by construction */
}

static int unpack_units(const uint64_t *units, int n_units, int N, uint8_t *tk) {
    if (n_units % 5) return -1;
    acc_t acc = 0; int nbits = 0, out = 0;
    for (int i = 0; i < n_units; i++) {
        acc = (acc << N) | units[i]; nbits += N;
        while (nbits >= 5) { nbits -= 5; tk[out++] = (uint8_t)(acc >> nbits) & 0x1F; }
    }
    return out;
}

/* ---- composition: binary decomposition of the count, largest first ----
 * counts > 127 repeat the int64 rung: 200 -> 64,64,64,8                  */
static int compose(int n_tok, int *rungs) {
    int c = 0;
    for (int i = 0; i < n_tok / 64; i++) rungs[c++] = 64;
    int rem = n_tok % 64;
    for (int N = 32; N >= 1; N >>= 1) if (rem & N) rungs[c++] = N;
    return c;
}

/* ---- byte serialization of a composed stream ----
 * Bit-identical to the reference pack(): 5n bits MSB-first,
 * pad = (8 - 5n%8)%8 zero bits in the final byte only.               */
static int composed_to_bytes(const uint8_t *tk, int n_tok, uint8_t *out, int *pad_out) {
    acc_t acc = 0; int nbits = 0, len = 0;
    for (int i = 0; i < n_tok; i++) {
        acc = (acc << 5) | (tk[i] & 0x1F); nbits += 5;
        while (nbits >= 8) { nbits -= 8; out[len++] = (uint8_t)(acc >> nbits) & 0xFF; }
    }
    int pad = 0;
    if (nbits) { pad = 8 - nbits; out[len++] = (uint8_t)(acc << pad) & 0xFF; }
    if (pad_out) *pad_out = pad;
    return len;
}

/* ------------------------------- self test ------------------------------- */
static uint32_t rng = 5;
static uint32_t rnd(void){ rng = rng*1664525u + 1013904223u; return rng >> 8; }
static void hex(const uint8_t *b, int n, char *h){ for(int i=0;i<n;i++) sprintf(h+2*i,"%02x",b[i]); }

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    int fails = 0;
    uint8_t tk[2048], rt[2048]; uint64_t un[4096];

    /* I1 + I2: round-trip per rung, width equivalence */
    const int rungset[] = {1,2,4,8,16,32,64};
    uint8_t base[64]; for (int i = 0; i < 64; i++) base[i] = rnd() & 0x1F;
    for (int r = 0; r < 7; r++) {
        int N = rungset[r];
        int nu = pack_units(base, 64, N, un);
        int nt = unpack_units(un, nu, N, rt);
        if (nt != 64 || memcmp(base, rt, 64)) { fails++; printf("I1/I2 FAIL rung %d\n", N); }
    }
    printf("I1 round-trip + I2 width equivalence: %s\n", fails ? "FAIL" : "OK");

    /* composition: arbitrary counts, zero slack, lossless */
    const int counts[] = {1,3,5,13,21,63,64,65,127,128,200,1000};
    for (size_t c = 0; c < sizeof(counts)/sizeof(*counts); c++) {
        int n = counts[c];
        for (int i = 0; i < n; i++) tk[i] = rnd() & 0x1F;
        int rungs[64]; int nr = compose(n, rungs);
        int idx = 0, bits = 0, rtn = 0;
        for (int s = 0; s < nr; s++) {
            int N = rungs[s];
            int nu = pack_units(tk + idx, N, N, un);
            bits += nu * N;
            rtn += unpack_units(un, nu, N, rt + rtn);
            idx += N;
        }
        if (idx != n || bits != 5*n || rtn != n || memcmp(tk, rt, n)) { fails++; printf("COMPOSE FAIL n=%d\n", n); }
    }
    printf("composition zero-slack + lossless: %s\n", fails ? "FAIL" : "OK");

    /* I3: reference vectors (LEXICON2 §6) through the byte path.
     * Token streams for the vectors, from the spec:
     *   0 -> D0 END        = 00 30      1 -> D1 END = 01 30
     *  -1 -> N1 END        = 17 30     42 -> D4 D2 END = 04 02 30
     * -42 -> N4 N2 END     = 20 18 30                                */
    struct { const char *hexs; int pad; uint8_t t[3]; int n; } vec[] = {
        {"0780",6,{0,30},2}, {"0f80",6,{1,30},2}, {"8f80",6,{17,30},2},
        {"20bc",1,{4,2,30},3}, {"a4bc",1,{20,18,30},3},
    };
    for (size_t v = 0; v < sizeof(vec)/sizeof(*vec); v++) {
        uint8_t b[16]; char h[40]; int pad;
        int len = composed_to_bytes(vec[v].t, vec[v].n, b, &pad);
        hex(b, len, h);
        if (strcmp(h, vec[v].hexs) || pad != vec[v].pad) { fails++; printf("I3 FAIL %s got %s/%d\n", vec[v].hexs, h, pad); }
        printf("VEC\t%s\tpad=%d\n", h, pad);
    }
    printf("I3 LEXICON2 reference vectors: %s\n", fails ? "FAIL" : "OK");

    printf(fails ? "\n%d FAILURES\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
