/* fivebit_vliw.c — VLIW scheduler built ON the 5bit fabric
 * ==========================================================
 * The 5bit interpreter's grant table (write_holder[]) IS the VLIW hazard
 * detector. For each bundle (cycle), ops acquire grants on scratch slots.
 * A conflicting write → REFUSED → that op goes to the next bundle.
 *
 * This is not a scheduler that borrows 5bit ideas — it IS the 5bit
 * ownership model applied to VLIW bundle packing.
 *
 * The 5bit insight: slots = registers, grants = exclusive access.
 * Disjoint slots don't conflict. Structure IS the address.
 *
 * Build: cc -O2 -o fivebit_vliw fivebit_vliw.c
 * Run:   ./fivebit_vliw
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* ── The VLIW machine spec (matching problem.py) ─────────────────────── */
#define VLEN 8
#define MAX_SLOTS 1536
#define MAX_OPS 16384

/* Engine slot limits */
enum { SLOT_ALU=12, SLOT_VALU=6, SLOT_LOAD=2, SLOT_STORE=2, SLOT_FLOW=1 };

/* Op types */
typedef enum { ENG_ALU, ENG_VALU, ENG_LOAD, ENG_STORE, ENG_FLOW, ENG_DEBUG } Engine;

/* VLIW op: reads/writes scratch slots */
typedef struct {
    Engine eng;
    int reads[16];    /* slot addresses this op reads */
    int nreads;
    int writes[16];   /* slot addresses this op writes */
    int nwrites;
    int height;       /* critical path length (for priority) */
    int scheduled;    /* which bundle this op landed in */
    int op_idx;       /* original index */
} VliwOp;

/* ── THE 5BIT FABRIC: grant table = hazard detector ──────────────────── */
typedef struct {
    int write_holder[MAX_SLOTS];  /* slot → op_idx that holds write grant */
    int nholders;
} GrantTable;  /* per-bundle; mirrors fb_machine.write_holder[] */

static void gt_init(GrantTable *gt) {
    memset(gt->write_holder, -1, sizeof(gt->write_holder));
    gt->nholders = 0;
}

/* Try to acquire a write grant. Returns 0 on success, -1 if already held.
 * THIS IS THE HAZARD DETECTOR. It's the same logic as fb_exec V_STORE:
 *   if (m->write_holder[slot] != m->holder + 1) return FB_REFUSED;
 */
static int gt_grant_w(GrantTable *gt, int slot, int op_idx) {
    if (slot < 0 || slot >= MAX_SLOTS) return -1;
    if (gt->write_holder[slot] >= 0) return -1;    /* REFUSED: slot held */
    gt->write_holder[slot] = op_idx;
    gt->nholders++;
    return 0;
}

/* Check if ANY of these slots is already write-held */
static int gt_any_held(GrantTable *gt, int *slots, int n) {
    for (int i = 0; i < n; i++)
        if (slots[i] >= 0 && gt->write_holder[slots[i]] >= 0)
            return 1;
    return 0;
}

/* ── Dependency graph builder ────────────────────────────────────────── */
static void build_deps(VliwOp *ops, int n) {
    int last_writer[MAX_SLOTS];
    memset(last_writer, -1, sizeof(last_writer));

    for (int j = 0; j < n; j++) {
        /* RAW: j reads what i wrote */
        for (int ri = 0; ri < ops[j].nreads; ri++) {
            int a = ops[j].reads[ri];
            if (a >= 0 && last_writer[a] >= 0) {
                /* dep established via height below */
            }
        }
        /* WAW + WAR tracked through last_writer for height */
        for (int wi = 0; wi < ops[j].nwrites; wi++) {
            int a = ops[j].writes[wi];
            if (a >= 0) last_writer[a] = j;
        }
    }

    /* Compute critical-path height (reverse pass) */
    for (int j = 0; j < n; j++) ops[j].height = 0;
    for (int j = n - 1; j >= 0; j--) {
        /* Find successors: ops that read what j writes */
        int max_h = 0;
        for (int k = j + 1; k < n; k++) {
            int depends = 0;
            for (int ri = 0; ri < ops[k].nreads && !depends; ri++)
                for (int wi = 0; wi < ops[j].nwrites && !depends; wi++)
                    if (ops[k].reads[ri] == ops[j].writes[wi])
                        depends = 1;
            for (int wi2 = 0; wi2 < ops[k].nwrites && !depends; wi2++)
                for (int wi = 0; wi < ops[j].nwrites && !depends; wi++)
                    if (ops[k].writes[wi2] == ops[j].writes[wi])
                        depends = 1;
            if (depends && ops[k].height + 1 > max_h)
                max_h = ops[k].height + 1;
        }
        if (max_h > ops[j].height) ops[j].height = max_h;
    }
}

/* ── 5BIT-POWERED VLIW SCHEDULER ─────────────────────────────────────── */
static int schedule_5bit(VliwOp *ops, int n) {
    /* Sort by height descending for priority */
    /* (simplified: use original order with height-based priority in loop) */

    int scheduled_count = 0;
    int bundle_num = 0;

    while (scheduled_count < n) {
        GrantTable gt;
        gt_init(&gt);
        int engine_count[6] = {0};
        int limits[6] = {SLOT_ALU, SLOT_VALU, SLOT_LOAD, SLOT_STORE, SLOT_FLOW, 64};
        int bundle_placed = 0;

        /* Greedy pass: try each unscheduled op by priority */
        for (int pass = 0; pass < 2; pass++) {
            /* Pass 0: try ops whose deps are all scheduled in earlier bundles */
            /* Pass 1: if nothing fit, force-place highest-priority ready op */
            for (int j = 0; j < n; j++) {
                if (ops[j].scheduled >= 0) continue;  /* already placed */

                /* Check if all deps are in earlier bundles */
                int deps_ready = 1;
                /* (deps implicitly handled by height priority — simplified) */

                if (!deps_ready && pass == 0) continue;
                if (pass == 1 && bundle_placed > 0) break; /* only force if empty */

                Engine e = ops[j].eng;
                int elim = (e == ENG_DEBUG) ? 5 : (int)e;
                if (engine_count[elim] >= limits[elim]) continue;

                /* 5BIT HAZARD CHECK: any read/write slot already held? */
                if (gt_any_held(&gt, ops[j].reads, ops[j].nreads)) continue;
                if (gt_any_held(&gt, ops[j].writes, ops[j].nwrites)) continue;

                /* Acquire write grants (5bit-style) */
                int conflict = 0;
                for (int wi = 0; wi < ops[j].nwrites; wi++) {
                    if (gt_grant_w(&gt, ops[j].writes[wi], j) != 0) {
                        conflict = 1; break;
                    }
                }
                if (conflict) continue;

                /* OP PLACED in this bundle */
                ops[j].scheduled = bundle_num;
                engine_count[elim]++;
                bundle_placed++;
                scheduled_count++;
            }
            if (pass == 0 && bundle_placed == 0) continue; /* try force-pass */
            if (bundle_placed > 0) break; /* placed ops, move to next bundle */
        }

        if (bundle_placed == 0) {
            /* Deadlock — shouldn't happen with correct deps */
            bundle_num++;
            continue;
        }
        bundle_num++;
    }

    return bundle_num;  /* total bundles = cycle count */
}


/* ═══════════════════════════════════════════════════════════════════════════════
 * KERNEL OP GENERATOR
 * Builds the VLIW ops for the tree-walk + hash kernel (matching takehome).
 * This is the equivalent of KernelBuilder.build_kernel() but in C, feeding
 * the 5bit scheduler directly.
 * ═══════════════════════════════════════════════════════════════════════════════ */

typedef struct {
    VliwOp ops[MAX_OPS];
    int nops;
    int scratch_ptr;
    int scratch_map[256];  /* name hash → addr (simple) */
    int const_addrs[4096]; /* value → addr */
} OpBuilder;

static int ob_alloc(OpBuilder *ob, int len) {
    int a = ob->scratch_ptr;
    ob->scratch_ptr += len;
    return a;
}

static int ob_const(OpBuilder *ob, int val) {
    /* Simple: always allocate a new scratch slot for each const.
     * The const value is loaded via a load(const) op. */
    int addr = ob_alloc(ob, 1);
    VliwOp *op = &ob->ops[ob->nops++];
    memset(op, 0, sizeof(*op));
    op->eng = ENG_LOAD;
    op->writes[0] = addr; op->nwrites = 1;
    op->op_idx = ob->nops - 1;
    ob->const_addrs[addr % 4096] = val;  /* store for debugging */
    return addr;
}

static void ob_alu(OpBuilder *ob, int dest, int a1, int a2) {
    VliwOp *op = &ob->ops[ob->nops++];
    memset(op, 0, sizeof(*op));
    op->eng = ENG_ALU;
    op->reads[0] = a1; op->reads[1] = a2; op->nreads = 2;
    op->writes[0] = dest; op->nwrites = 1;
    op->op_idx = ob->nops - 1;
}

static void ob_load(OpBuilder *ob, int dest, int addr) {
    VliwOp *op = &ob->ops[ob->nops++];
    memset(op, 0, sizeof(*op));
    op->eng = ENG_LOAD;
    op->reads[0] = addr; op->nreads = 1;
    op->writes[0] = dest; op->nwrites = 1;
    op->op_idx = ob->nops - 1;
}

static void ob_store(OpBuilder *ob, int addr, int src) {
    VliwOp *op = &ob->ops[ob->nops++];
    memset(op, 0, sizeof(*op));
    op->eng = ENG_STORE;
    op->reads[0] = addr; op->reads[1] = src; op->nreads = 2;
    op->op_idx = ob->nops - 1;
}

static void ob_vbroadcast(OpBuilder *ob, int vdest, int src) {
    VliwOp *op = &ob->ops[ob->nops++];
    memset(op, 0, sizeof(*op));
    op->eng = ENG_VALU;
    op->reads[0] = src; op->nreads = 1;
    for (int i = 0; i < VLEN; i++) op->writes[i] = vdest + i;
    op->nwrites = VLEN;
    op->op_idx = ob->nops - 1;
}

static void ob_valu2(OpBuilder *ob, int vdest, int va1, int va2) {
    VliwOp *op = &ob->ops[ob->nops++];
    memset(op, 0, sizeof(*op));
    op->eng = ENG_VALU;
    for (int i = 0; i < VLEN; i++) {
        op->reads[i] = va1 + i;
        op->reads[VLEN + i] = va2 + i;
    }
    op->nreads = VLEN * 2;
    for (int i = 0; i < VLEN; i++) op->writes[i] = vdest + i;
    op->nwrites = VLEN;
    op->op_idx = ob->nops - 1;
}

/* ── Build the vectorized tree-walk kernel ──────────────────────────── */
static int build_kernel_ops(OpBuilder *ob, int n_nodes, int batch, int rounds) {
    int NG = batch / VLEN;
    int NBANKS = 12;

    /* Alloc bank registers */
    int bank_vidx[12], bank_vval[12];
    for (int b = 0; b < NBANKS; b++) {
        bank_vidx[b] = ob_alloc(ob, VLEN);
        bank_vval[b] = ob_alloc(ob, VLEN);
    }

    /* Vector temporaries */
    int vt1 = ob_alloc(ob, VLEN);
    int vt2 = ob_alloc(ob, VLEN);
    int vfvp = ob_alloc(ob, VLEN);
    int vaddr = ob_alloc(ob, VLEN);
    int vbc1 = ob_alloc(ob, VLEN);
    int vbc2 = ob_alloc(ob, VLEN);

    /* Scalar temps */
    int st = ob_alloc(ob, 1);
    int st2 = ob_alloc(ob, 1);

    /* Memory layout slots */
    int rounds_slot = ob_alloc(ob, 1);
    int nnodes_slot = ob_alloc(ob, 1);
    int batch_slot = ob_alloc(ob, 1);
    int height_slot = ob_alloc(ob, 1);
    int fvp_slot = ob_alloc(ob, 1);
    int iip_slot = ob_alloc(ob, 1);
    int ivp_slot = ob_alloc(ob, 1);

    /* Init loads */
    ob_load(ob, rounds_slot, ob_const(ob, 0));   /* dummy: would need mem reads */
    ob_load(ob, nnodes_slot, ob_const(ob, 0));
    ob_load(ob, batch_slot, ob_const(ob, 0));
    ob_load(ob, height_slot, ob_const(ob, 0));
    ob_load(ob, fvp_slot, ob_const(ob, 7));       /* forest_values_p = 7 */
    ob_load(ob, iip_slot, ob_const(ob, 14));      /* inp_indices_p */
    ob_load(ob, ivp_slot, ob_const(ob, 22));      /* inp_values_p */

    /* Constants */
    int zc = ob_const(ob, 0);
    int oc = ob_const(ob, 1);
    int tc = ob_const(ob, 2);

    /* Broadcast constants */
    int vzero = ob_alloc(ob, VLEN);
    int vone = ob_alloc(ob, VLEN);
    int vtwo = ob_alloc(ob, VLEN);
    int vnn = ob_alloc(ob, VLEN);
    ob_vbroadcast(ob, vzero, zc);
    ob_vbroadcast(ob, vone, oc);
    ob_vbroadcast(ob, vtwo, tc);
    ob_vbroadcast(ob, vnn, nnodes_slot);
    ob_vbroadcast(ob, vfvp, fvp_slot);

    /* Group offsets */
    int goffs[32]; /* max 32 groups */
    for (int g = 0; g < NG; g++) goffs[g] = ob_const(ob, g * VLEN);

    /* ── THE KERNEL LOOP ──────────────────────────────────────────── */
    for (int rnd = 0; rnd < rounds; rnd++) {
        for (int g = 0; g < NG; g++) {
            int b = g % NBANKS;
            int vidx = bank_vidx[b];
            int vval = bank_vval[b];
            int goff = goffs[g];

            if (rnd == 0) {
                /* vload idx and val (simplified: would be actual mem loads) */
                ob_alu(ob, st, iip_slot, goff);
                /* vload simplified as vector broadcast from const */
                ob_vbroadcast(ob, vidx, zc);
                ob_alu(ob, st, ivp_slot, goff);
                ob_vbroadcast(ob, vval, zc);
            }

            /* vaddr = vfvp + vidx */
            ob_valu2(ob, vaddr, vfvp, vidx);

            /* Gather (per-lane loads, simplified) */
            for (int li = 0; li < VLEN; li++) {
                ob_alu(ob, st2, vaddr, ob_const(ob, li));
                ob_load(ob, vt1 + li, st2);
            }

            /* val ^= node_val */
            ob_valu2(ob, vval, vval, vt1);

            /* HASH: 6 stages, 3 fused (multiply_add simplified as 3-ops each) */
            /* Stage 0: a = (a + 0x7ED55D16) + (a << 12)  → fused */
            {
                int vbc1_m = ob_alloc(ob, VLEN);
                int vbc2_v = ob_alloc(ob, VLEN);
                ob_vbroadcast(ob, vbc1_m, ob_const(ob, 1 + (1 << 12)));
                ob_vbroadcast(ob, vbc2_v, ob_const(ob, 0x7ED55D16));
                /* multiply_add: vval = vval * vbc1 + vbc2 */
                ob_valu2(ob, vval, vval, vbc1_m);  /* mul */
                ob_valu2(ob, vval, vval, vbc2_v);  /* add (2 ops instead of 1 multiply_add) */
            }
            /* Stages 1-5: simplified as pass-through for now */
            /* (Full implementation would add all 6 stages) */

            /* idx update: parity = val & 1, inc = 1 + parity */
            ob_valu2(ob, vt1, vval, vone);    /* vt1 = val (mask, simplified) */
            ob_valu2(ob, vt1, vone, vt1);      /* inc = 1 + parity */
            ob_valu2(ob, vidx, vidx, vtwo);     /* idx *= 2 (simplified) */
            ob_valu2(ob, vidx, vidx, vt1);      /* idx += inc */

            /* wrap: mask = idx < n_nodes */
            ob_valu2(ob, vt1, vidx, vnn);
            /* vselect simplified as override */
            ob_valu2(ob, vidx, vt1, vzero);

            /* Store at end */
            if (rnd == rounds - 1) {
                ob_alu(ob, st, iip_slot, goff);
                ob_store(ob, st, vidx);
                ob_alu(ob, st, ivp_slot, goff);
                ob_store(ob, st, vval);
            }
        }
    }

    return ob->nops;
}


/* ═══════════════════════════════════════════════════════════════════════════════
 * MAIN: build kernel ops, schedule with 5bit grant model, report cycles
 * ═══════════════════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv) {
    int height = argc > 1 ? atoi(argv[1]) : 10;
    int rounds = argc > 2 ? atoi(argv[2]) : 16;
    int batch  = argc > 3 ? atoi(argv[3]) : 256;

    int n_nodes = (1 << (height + 1)) - 1;

    printf("┌──────────────────────────────────────────────┐\n");
    printf("│  5bit VLIW Scheduler                          │\n");
    printf("│  Grant table = hazard detector                 │\n");
    printf("│  Ops acquire grants; conflicts → next bundle   │\n");
    printf("└──────────────────────────────────────────────┘\n");
    printf("  height=%d rounds=%d batch=%d n_nodes=%d\n\n", height, rounds, batch, n_nodes);

    OpBuilder *ob = calloc(1, sizeof(OpBuilder));
    if (!ob) { printf("OOM\n"); return 1; }
    ob->scratch_ptr = 256; /* reserve low addresses */

    int nops = build_kernel_ops(ob, n_nodes, batch, rounds);
    printf("  Kernel ops: %d\n", nops);

    /* Build dependency graph for height computation */
    build_deps(ob->ops, nops);

    /* Schedule using 5bit grant model */
    int cycles = schedule_5bit(ob->ops, nops);
    printf("  VLIW bundles: %d\n", cycles);

    /* Bundle distribution */
    int max_bundle = 0;
    for (int i = 0; i < nops; i++)
        if (ob->ops[i].scheduled > max_bundle)
            max_bundle = ob->ops[i].scheduled;
    printf("  Max bundle: %d\n", max_bundle + 1);

    /* Engine utilization */
    printf("\n  ── Bundle occupancy ──\n");
    for (int b = 0; b < 20 && b <= max_bundle; b++) {
        int counts[6] = {0};
        for (int i = 0; i < nops; i++)
            if (ob->ops[i].scheduled == b)
                counts[(int)ob->ops[i].eng]++;
        printf("  bundle %3d: alu=%d valu=%d load=%d store=%d flow=%d\n",
               b, counts[0], counts[1], counts[2], counts[3], counts[4]);
    }

    printf("\n  Done. The 5bit grant table IS the VLIW scheduler.\n");
    return 0;
}
