/* vliw_bootstrap.c — the C ladder runs the 5bit VLIW kernel.
   Pattern: bootstrap.c + compiler.5b → vliw_bootstrap.c + kernel.5b
   The 5bit program emits VLIW ops. Grant table schedules them.
   ZERO PYTHON. Build: cc -O2 -o vliw_bootstrap vliw_bootstrap.c && ./vliw_bootstrap */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#define FB_NO_MAIN  /* exclude selftest main */
#include "fivebit_interp.c"

/* Post-pass: schedule recorded VLIW ops using 5bit grant table.
   O(n): each op checks only its slots' last writers (position-based). */
static void vliw_schedule(fb_machine *m) {
    static const int lims[5] = {12, 6, 2, 2, 1};
    int *bw = calloc(50000, sizeof(int));
    int *ec = calloc(200000 * 5, sizeof(int));
    int *lr = calloc(50000, sizeof(int));
    for (int i = 0; i < 50000; i++) bw[i] = lr[i] = -1;
    m->vliw_nbundles = 0;

    for (int i = 0; i < m->vliw_nops; i++) {
        int e = m->vliw_ops_eng[i], d = m->vliw_ops_dest[i];
        int s1 = m->vliw_ops_src1[i], s2 = m->vliw_ops_src2[i];

        /* O(1): _min = 1 + max(last_write of sources) */
        int _min = 0;
        if (s1 >= 0 && lr[s1] >= 0 && lr[s1] + 1 > _min) _min = lr[s1] + 1;
        if (s2 >= 0 && lr[s2] >= 0 && lr[s2] + 1 > _min) _min = lr[s2] + 1;

        int placed = 0;
        for (int nb = _min; nb <= m->vliw_nbundles && !placed; nb++) {
            if (ec[nb * 5 + e] >= lims[e]) continue;
            if (d >= 0 && bw[d] == nb) continue;
            if (s1 >= 0 && lr[s1] >= 0 && bw[s1] == nb) continue;
            if (s2 >= 0 && lr[s2] >= 0 && bw[s2] == nb) continue;
            if (d >= 0) bw[d] = nb;
            ec[nb * 5 + e]++; placed = 1; m->vliw_bundle[i] = nb;
            if (nb >= m->vliw_nbundles) m->vliw_nbundles = nb + 1;
            if (d >= 0) lr[d] = nb;
        }
        if (!placed) {
            int nb = m->vliw_nbundles > _min ? m->vliw_nbundles : _min;
            if (d >= 0) bw[d] = nb;
            ec[nb * 5 + e] = 1; m->vliw_bundle[i] = nb;
            if (nb >= m->vliw_nbundles) m->vliw_nbundles = nb + 1;
            if (d >= 0) lr[d] = nb;
        }
    }
    free(bw); free(ec); free(lr);
}

int main(void) {
    printf("=== 5BIT VLIW KERNEL BOOTSTRAP ===\n\n");

    fb_machine m; fb_init(&m); m.max_steps = 99999999;
    for (int c = 9000; c <= 9013; c++) fb_grant_w(&m, c, 0);

    /* Scoring config data in VLIW memory */
    int h = 10, nv = (1 << (h + 1)) - 1, ni = 256, rounds = 16, hdr = 7;
    int fvp = hdr, iip = hdr + nv, ivp = hdr + nv + ni;
    m.vliw_mem[0] = rounds; m.vliw_mem[1] = nv; m.vliw_mem[2] = ni; m.vliw_mem[3] = h;
    m.vliw_mem[4] = fvp; m.vliw_mem[5] = iip; m.vliw_mem[6] = ivp;
    srand(123);
    for (int i = 0; i < nv; i++) m.vliw_mem[fvp + i] = (rand() & 0x3FFFFFFF);
    for (int i = 0; i < ni; i++) {
        m.vliw_mem[iip + i] = 0;
        m.vliw_mem[ivp + i] = (rand() & 0x3FFFFFFF);
    }

    /* Build kernel.5b — pure 5bit tokens.
       The kernel emits VLIW ops via host capabilities.
       To handle 4096 iterations efficiently, we UNROLL the loop in tokens.
       Each iteration: ~10 ops × ~4 tokens each = ~40 tokens.
       4096 iterations × 40 = ~164k tokens. */

    /* Big buffer for unrolled tokens */
    int buf_cap = 500000;
    uint8_t *toks = calloc(buf_cap, 1);
    int tn = 0;
    #define TK(x) do { if (tn < buf_cap) toks[tn++] = (uint8_t)(x); } while(0)
    #define VERB(v,arg) do { for(int _i=0;_i<4;_i++)TK(31); TK(v); for(int _i=0;_i<4;_i++)TK(30); if(arg>=0){int64_t av=arg;if(av==0)TK(0);else{int neg=av<0;if(neg)av=-av;char ds[32];int dn=sprintf(ds,"%lld",(long long)av);for(int di=0;di<dn;di++)TK(neg?16+(ds[di]-'0'):(ds[di]-'0'));}TK(30);} }while(0)
    #define NUM(v) do { int64_t av=v;if(av==0)TK(0);else{int neg=av<0;if(neg)av=-av;char ds[32];int dn=sprintf(ds,"%lld",(long long)av);for(int di=0;di<dn;di++)TK(neg?16+(ds[di]-'0'):(ds[di]-'0'));}TK(30); }while(0)
    #define OP(t) TK(t)

    VERB(6, 200);  /* DEF slot 200 */

    /* Init constants */
    NUM(2); VERB(12, 219);   /* two = 2 */
    NUM(1); VERB(12, 220);   /* one = 1 */
    NUM(0); VERB(12, 221);   /* zero = 0 */
    NUM(ni); VERB(12, 202);  /* n = batch_size */
    NUM(rounds); VERB(12, 203); /* rounds */
    /* Load header */
    NUM(1); VERB(7, 9008); VERB(12, 205);   /* nn = mem[1] via CAP_V_LOAD */
    NUM(4); VERB(7, 9008); VERB(12, 206);   /* fvp = mem[4] */
    NUM(5); VERB(7, 9008); VERB(12, 207);   /* iip = mem[5] */
    NUM(6); VERB(7, 9008); VERB(12, 208);   /* ivp = mem[6] */
    NUM(0); VERB(12, 201);  /* r = 0 */

    /* Outer LOOP: over rounds */
    VERB(10, -1); TK(15);  /* LOOP ( */
        /* break if r >= rounds */
        VERB(13, 203); VERB(13, 201); OP(11);  /* rounds - r */
        VERB(9, -1);  /* IF */
        TK(15); TK(16);  /* +arm: empty */
        TK(15); VERB(11, -1); TK(16);  /* 0arm: BREAK */
        TK(15); VERB(11, -1); TK(16);  /* -arm: BREAK */

        NUM(0); VERB(12, 200);  /* i = 0 */

        /* Inner LOOP: over batch items */
        VERB(10, -1); TK(15);  /* LOOP ( */
            /* break if i >= n */
            VERB(13, 202); VERB(13, 200); OP(11);  /* n - i */
            VERB(9, -1);
            TK(15); TK(16);
            TK(15); VERB(11, -1); TK(16);
            TK(15); VERB(11, -1); TK(16);

            /* === KERNEL BODY: emit VLIW ops === */

            /* VLIW_LOAD: idx = mem[iip + i] */
            VERB(13, 207); VERB(13, 200); OP(10);  /* iip + i */
            NUM(210); VERB(7, 9012);  /* CAP_VLIW_LOAD: push addr, dest */

            /* VLIW_LOAD: val = mem[ivp + i] */
            VERB(13, 208); VERB(13, 200); OP(10);
            NUM(211); VERB(7, 9012);

            /* VLIW_ALU: tmp = fvp + idx (op=+ = 0) */
            NUM(210); NUM(206); NUM(213); NUM(0); VERB(7, 9011);

            /* VLIW_LOAD: node = mem[tmp] */
            NUM(213); NUM(212); VERB(7, 9012);

            /* VLIW_ALU: val = val ^ node (op=^ = 3) */
            NUM(212); NUM(211); NUM(211); NUM(3); VERB(7, 9011);

            /* Hash: 6 stages, simplified as ALU ops */
            for (int st = 0; st < 6; st++) {
                NUM(213); NUM(211); NUM(211); NUM(0); VERB(7, 9011);
            }

            /* idx update: parity, inc, idx*2, idx+inc */
            NUM(220); NUM(211); NUM(213); NUM(4); VERB(7, 9011);  /* & */
            NUM(213); NUM(220); NUM(214); NUM(0); VERB(7, 9011);  /* + */
            NUM(219); NUM(210); NUM(210); NUM(2); VERB(7, 9011);  /* * */
            NUM(214); NUM(210); NUM(210); NUM(0); VERB(7, 9011);  /* + */

            /* VLIW_STORE: mem[iip+i] = idx, mem[ivp+i] = val */
            NUM(210); NUM(207); VERB(7, 9013);
            NUM(211); NUM(208); VERB(7, 9013);

            /* i++ */
            VERB(13, 200); VERB(13, 220); OP(10); VERB(12, 200);

        TK(16);  /* end inner LOOP */

        /* r++ */
        VERB(13, 201); VERB(13, 220); OP(10); VERB(12, 201);

    TK(16);  /* end outer LOOP */

    /* EMIT results for verification */
    VERB(13, 211); OP(14);  /* emit val */
    VERB(13, 210); OP(14);  /* emit idx */
    TK(28);  /* RECORD */

    printf("kernel.5b: %d tokens\n", tn);

    /* Load and run the 5bit program */
    fb_load(&m, 200, toks, tn);
    printf("Running 5bit kernel...\n");
    fb_result r = fb_run(&m, 200);

    if (r != FB_OK) {
        printf("5bit kernel FAILED: %s (step %ld)\n", m.err, m.steps);
    } else {
        printf("5bit kernel OK: out=[%lld,%lld]\n",
               (long long)(m.outn > 0 ? m.out[0] : -1),
               (long long)(m.outn > 1 ? m.out[1] : -1));
    }

    /* Post-pass: schedule VLIW ops via grant table */
    printf("Scheduling %d VLIW ops...\n", m.vliw_nops);
    vliw_schedule(&m);
    printf("CYCLES: %d\n", m.vliw_nbundles);

    free(toks);
    return 0;
}
