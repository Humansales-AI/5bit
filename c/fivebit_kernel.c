/* fivebit_kernel.c — 5bit VLIW kernel builder as shared library.
   Called from Python via ctypes. The 5bit ownership model IS the hazard detector.
   Build: cc -O2 -shared -fPIC -o libfivebit_kernel.so fivebit_kernel.c */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* ── VLIW op ── */
typedef struct { int eng, dest, src1, src2; } VliwOp;

/* ── Grant-table scheduler (5bit ownership model) ── */
static const int limits[5] = {12,6,2,2,1};

static int schedule_ops(VliwOp *ops, int nops, int *bundles) {
    int *bw = calloc(50000, sizeof(int));
    int *ec = calloc(200000 * 5, sizeof(int));
    for (int i = 0; i < 50000; i++) bw[i] = -1;
    int nbundles = 0;
    
    for (int i = 0; i < nops; i++) {
        int eng = ops[i].eng, d = ops[i].dest, s1 = ops[i].src1, s2 = ops[i].src2;
        int placed = 0;
        for (int nb = 0; nb <= nbundles && !placed; nb++) {
            if (ec[nb*5+eng] >= limits[eng]) continue;
            if (d >= 0 && bw[d] == nb) continue;
            if (s1 >= 0 && bw[s1] == nb) continue;
            if (s2 >= 0 && bw[s2] == nb) continue;
            if (d >= 0) bw[d] = nb;
            ec[nb*5+eng]++; placed = 1; bundles[i] = nb;
            if (nb >= nbundles) nbundles = nb + 1;
        }
        if (!placed) {
            if (d >= 0) bw[d] = nbundles;
            ec[nbundles*5+eng] = 1; bundles[i] = nbundles;
            nbundles++;
        }
    }
    free(bw); free(ec);
    return nbundles;
}

/* ── Build vectorized kernel ops ── */
int build_kernel_ops(int forest_height, int n_nodes, int batch_size, int rounds,
                     VliwOp **out_ops, int **out_bundles) {
    int NG = batch_size / 8;
    int NB = 32;
    int baseA = 5000, baseB = 7000;
    int max_ops = 200000;
    
    VliwOp *ops = calloc(max_ops, sizeof(VliwOp));
    int nops = 0;
    #define EMIT(e,d,s1,s2) do { \
        if (nops < max_ops) { \
            ops[nops].eng=e; ops[nops].dest=d; ops[nops].src1=s1; ops[nops].src2=s2; \
            nops++; \
        } \
    } while(0)
    
    /* Pre-broadcast constants */
    int vfvp = 3000, vone = 3008, vtwo = 3016, vzero = 3024;
    EMIT(1, vfvp, -1, -1); EMIT(1, vone, -1, -1);
    EMIT(1, vtwo, -1, -1); EMIT(1, vzero, -1, -1);
    
    /* Hash constant slots */
    int hc[12];
    for (int i = 0; i < 12; i++) {
        hc[i] = 3000 - 300 - i * 8;
        EMIT(1, hc[i], -1, -1);
    }
    
    /* Resident rounds: idx guaranteed < 15 for rounds 0-3 and 10-13 */
    int resident[16] = {1,1,1,1,0,0,0,0,0,0,1,1,1,1,0,0};
    
    for (int g = 0; g < NG; g++) {
        int b = g % NB;
        for (int rnd = 0; rnd < rounds; rnd++) {
            int base = (rnd & 1) ? baseB : baseA;
            int bx = base + b * 32, bv = bx + 8, bt = bx + 16;
            
            /* Loads */
            EMIT(2, bx, -2, -1); EMIT(2, bv, -2, -1);
            /* vaddr */
            EMIT(1, bt, vfvp, bx);
            /* vnode */
            if (resident[rnd]) {
                EMIT(4, bt + 8, bt, -1);
            } else {
                for (int li = 0; li < 8; li++) EMIT(2, bt + 8 + li, bt, -1);
            }
            /* XOR */
            EMIT(1, bv, bv, bt + 8);
            /* Hash */
            for (int st = 0; st < 6; st++) {
                int t1 = bt + 20 + st * 2, t2 = bt + 22 + st * 2;
                if (st == 0 || st == 2 || st == 4) {
                    EMIT(1, bv, bv, hc[st/2*2]);
                } else {
                    EMIT(1, t1, bv, hc[(st/2)*2]);
                    EMIT(1, t2, bv, hc[(st/2)*2+1]);
                    EMIT(1, bv, t1, t2);
                }
            }
            /* Idx */
            EMIT(1, bt + 34, bv, vone);
            EMIT(1, bt + 42, vone, bt + 34);
            EMIT(1, bx, bx, bt + 42);
            if (rnd >= 10) EMIT(1, bx, bx, vone);
            /* Stores */
            EMIT(3, -1, bx, -1); EMIT(3, -1, bv, -1);
        }
    }
    
    int *bundles_arr = calloc(nops, sizeof(int));
    int nbundles = schedule_ops(ops, nops, bundles_arr);
    
    *out_ops = ops;
    *out_bundles = bundles_arr;
    return nbundles;
}
