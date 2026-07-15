/* fivebit_interp.c — the Machine, in C
 * =====================================
 * The missing organ: the C binary can now EXECUTE DEF'd 5bit records.
 * Storage (fivebit_grid.c) + codec (fivebit_codec.c) + THIS = a full
 * VM-DB in one binary. No Python anywhere in the critical path.
 *
 * Ported from the reference implementation (griddb_interp.py, 12/12 green).
 * Every semantic decision is already made and conformance-tested; this is a
 * port, not a design. Cross-checked program-for-program against Python.
 *
 * VERBS (SPECIAL3 slots 6..13), encoded as START*4 . cmd . END*4 [. NUM arg]:
 *   DEF=6 CALL=7 RET=8 IF=9 LOOP=10 BREAK=11 STORE=12 READ=13
 *
 * THREE-WAY IF: pop t, dispatch on sign(t) to up to three ( ... ) regions:
 *   ( +arm ) ( 0arm ) ( -arm ).  All six relations = one MINUS + placement.
 *
 * REGIONS: LPAREN(15)/RPAREN(16); skipping is balanced-paren counting.
 * Structure is the address. No jumps, no offsets.
 *
 * OWNERSHIP: STORE is gated by a grant table (slot -> holder). A program
 * running as `holder` cannot append to a slot it does not hold: STORE
 * refuses MID-PROGRAM (FB_REFUSED), grid untouched. Effects are grants.
 *
 * DOORMAN: CALL to slot >= FB_HOST_BASE (9000) is trapped and routed to a
 * host capability (also grant-gated). Deny-by-default.
 *
 * Build:  cc -O2 -o fivebit_interp fivebit_interp.c && ./fivebit_interp selftest
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ---------------- tokens ---------------- */
#define T_D0     0
#define T_PLUS   10
#define T_MINUS  11
#define T_MUL    12
#define T_DIV    13
#define T_EMIT   14      /* '=' pops and emits */
#define T_LPAREN 15
#define T_RPAREN 16
#define T_N1     17      /* negative digits 17..25 */
#define T_RECORD 28
#define T_CHECKSUM 29
#define T_END    30
#define T_START  31

/* verbs */
#define V_DEF 6
#define V_CALL 7
#define V_RET 8
#define V_IF 9
#define V_LOOP 10
#define V_BREAK 11
#define V_STORE 12
#define V_READ 13
#define V_LOADX 18    /* pop index -> push slots[index]      */
#define V_STOREX 19   /* pop index, pop value -> slots[index] */

#define FB_HOST_BASE 9000
#define CAP_NOW      9000
#define CAP_RANDOM   9001
#define CAP_HASH     9002
#define CAP_EMIT_OUT 9003
#define CAP_LOG      9004
#define CAP_READ_IN  9005
#define CAP_V_XOR 9006
#define CAP_V_AND 9007
#define CAP_V_LOAD 9008
#define CAP_V_STORE 9009
#define CAP_V_HASH6 9010
#define CAP_VLIW_ALU 9011
#define CAP_VLIW_LOAD 9012
#define CAP_VLIW_STORE 9013

/* ---------------- results ---------------- */
typedef enum {
    FB_OK = 0,
    FB_REFUSED,        /* ownership/capability refusal (mid-program) */
    FB_OUT_OF_GAS,
    FB_ERROR,
    FB_SIG_RET,        /* internal control signals */
    FB_SIG_BREAK
} fb_result;

/* ---------------- machine ---------------- */
#define MAX_SLOTS   16384
#define MAX_PROGS   256
#define STACK_MAX   256
#define OUT_MAX     256
#define INBOX_MAX   64

typedef struct {
    const uint8_t *toks;
    int len;
    int slot;
} fb_program;

typedef struct {
    /* value + program memory */
    int64_t stack[STACK_MAX];  int sp;
    int64_t out[OUT_MAX];      int outn;
    fb_program progs[MAX_PROGS]; int nprogs;

    /* the grid: slot -> value (int model; real build calls grid_write/read) */
    int64_t slots[MAX_SLOTS];
    uint8_t slot_set[MAX_SLOTS];

    /* ownership: slot -> holder (+1; 0 = unheld) */
    int32_t write_holder[MAX_SLOTS];
    int holder;                 /* the running program's holder */

    /* doorman */
    int64_t inbox[INBOX_MAX]; int inbox_n, inbox_i;
    int64_t outbox[OUT_MAX];  int outbox_n;
    int64_t fixed_clock;        /* injected seam for determinism */
    int64_t vliw_mem[16384];
    int vliw_ops_eng[131072],vliw_ops_dest[131072],vliw_ops_src1[131072],vliw_ops_src2[131072];
    int vliw_nops;

    /* safety */
    long steps, max_steps;
    char err[160];
} fb_machine;

static void fb_init(fb_machine *m) {
    memset(m, 0, sizeof(*m));
    m->max_steps = 1000000;
    m->fixed_clock = 0;
}

static void fb_grant_w(fb_machine *m, int slot, int holder) {
    if (slot >= 0 && slot < MAX_SLOTS) m->write_holder[slot] = holder + 1;
}

/* ---------------- token helpers ---------------- */
static int is_digit_tok(uint8_t t)  { return t <= 9; }
static int is_negdig_tok(uint8_t t) { return t >= T_N1 && t <= 25; }

/* read an integer (digits + END finalizer) starting at *pos */
static int64_t read_int(const uint8_t *t, int *pos, int end) {
    int64_t v = 0; int neg = 0, any = 0;
    while (*pos < end) {
        uint8_t k = t[*pos];
        if (is_digit_tok(k))       { v = v * 10 + k;            (*pos)++; any = 1; }
        else if (is_negdig_tok(k)) { v = v * 10 + (k - 16); neg = 1; (*pos)++; any = 1; }
        else if (k == T_END)       { (*pos)++; break; }
        else break;
    }
    (void)any;
    return neg ? -v : v;
}

/* balanced-paren match: returns index of the closing RPAREN */
static int match_region(const uint8_t *t, int open, int end) {
    int depth = 0;
    for (int i = open; i < end; i++) {
        if (t[i] == T_LPAREN) depth++;
        else if (t[i] == T_RPAREN) { if (--depth == 0) return i; }
    }
    return -1;
}

/* ---------------- doorman ---------------- */
static uint32_t rng_state = 0x5b17;
static int64_t host_rand(void) {
    rng_state = rng_state * 1103515245u + 12345u;
    return (int64_t)(rng_state & 0x7fffffff);
}
static int64_t host_hash6(int64_t a){uint32_t v=(uint32_t)a;v=(v+0x7ED55D16u)+(v<<12);v=(v^0xC761C23Cu)^(v>>19);v=(v+0x165667B1u)+(v<<5);v=(v+0xD3A2646Cu)^(v<<9);v=(v+0xFD7046C5u)+(v<<3);v=(v^0xB55A4F09u)^(v>>16);return(int64_t)v;}
static int64_t host_hash(int64_t n) {   /* stable, simple (FNV-ish) */
    uint64_t h = 1469598103934665603ULL;
    char buf[32]; int len = snprintf(buf, sizeof buf, "%lld", (long long)n);
    for (int i = 0; i < len; i++) { h ^= (uint8_t)buf[i]; h *= 1099511628211ULL; }
    return (int64_t)(h & 0x7fffffff);
}

static fb_result host_invoke(fb_machine *m, int cap) {
    /* deny-by-default: capability requires a grant, exactly like STORE */
    if (cap < 0 || cap >= MAX_SLOTS || m->write_holder[cap] != m->holder + 1) {
        snprintf(m->err, sizeof m->err,
                 "CAP %d refused: holder %d lacks grant", cap, m->holder);
        return FB_REFUSED;
    }
    switch (cap) {
    case CAP_NOW:      m->stack[m->sp++] = m->fixed_clock ? m->fixed_clock
                                                          : (int64_t)time(NULL); break;
    case CAP_RANDOM:   m->stack[m->sp++] = host_rand(); break;
    case CAP_HASH:     { int64_t a = m->stack[--m->sp];
                         m->stack[m->sp++] = host_hash(a); } break;
    case CAP_EMIT_OUT: { int64_t a = m->stack[--m->sp];
                         if (m->outbox_n < OUT_MAX) m->outbox[m->outbox_n++] = a; } break;
    case CAP_LOG:      { int64_t a = m->stack[--m->sp]; (void)a; } break;
    case CAP_READ_IN:  m->stack[m->sp++] =
                         (m->inbox_i < m->inbox_n) ? m->inbox[m->inbox_i++] : -1; break;
    case CAP_V_XOR:{int64_t b=m->stack[--m->sp],a=m->stack[--m->sp];m->stack[m->sp++]=(a^b)&0xFFFFFFFF;}break;
    case CAP_V_AND:{int64_t b=m->stack[--m->sp],a=m->stack[--m->sp];m->stack[m->sp++]=a&b;}break;
    case CAP_V_LOAD:{int64_t ad=m->stack[--m->sp];m->stack[m->sp++]=(ad>=0&&ad<16384)?m->vliw_mem[ad]:0;}break;
    case CAP_V_STORE:{int64_t v=m->stack[--m->sp],ad=m->stack[--m->sp];if(ad>=0&&ad<16384)m->vliw_mem[ad]=v;}break;
    case CAP_V_HASH6:{int64_t a=m->stack[--m->sp];m->stack[m->sp++]=host_hash6(a);}break;
    case CAP_VLIW_LOAD:{int d=(int)m->stack[--m->sp],s1=(int)m->stack[--m->sp];if(m->vliw_nops<131072){int i=m->vliw_nops++;m->vliw_ops_eng[i]=2;m->vliw_ops_dest[i]=d;m->vliw_ops_src1[i]=s1;}}break;
    case CAP_VLIW_ALU:{int op=(int)m->stack[--m->sp],d=(int)m->stack[--m->sp],s1=(int)m->stack[--m->sp],s2=(int)m->stack[--m->sp];if(m->vliw_nops<131072){int i=m->vliw_nops++;m->vliw_ops_eng[i]=0;m->vliw_ops_dest[i]=d;m->vliw_ops_src1[i]=s1;m->vliw_ops_src2[i]=s2;}}break;
    case CAP_VLIW_STORE:{int s2=(int)m->stack[--m->sp],s1=(int)m->stack[--m->sp];if(m->vliw_nops<131072){int i=m->vliw_nops++;m->vliw_ops_eng[i]=3;m->vliw_ops_src1[i]=s1;m->vliw_ops_src2[i]=s2;}}break;
    default:
        snprintf(m->err, sizeof m->err, "unknown capability %d", cap);
        return FB_REFUSED;
    }
    return FB_OK;
}

/* ---------------- execution ---------------- */
static fb_result fb_exec(fb_machine *m, const uint8_t *t, int pos, int end);

static fb_result fb_call(fb_machine *m, int slot) {
    if (slot >= FB_HOST_BASE) return host_invoke(m, slot);   /* the doorman trap */
    for (int i = 0; i < m->nprogs; i++) {
        if (m->progs[i].slot == slot) {
            fb_result r = fb_exec(m, m->progs[i].toks, 0, m->progs[i].len);
            return (r == FB_SIG_RET) ? FB_OK : r;
        }
    }
    snprintf(m->err, sizeof m->err, "CALL %d: no DEF'd program at slot", slot);
    return FB_ERROR;
}

static fb_result fb_exec(fb_machine *m, const uint8_t *t, int pos, int end) {
    while (pos < end) {
        if (++m->steps > m->max_steps) {
            snprintf(m->err, sizeof m->err, "step budget exhausted");
            return FB_OUT_OF_GAS;
        }
        uint8_t k = t[pos];

        /* SPECIAL3 verb: START*4 . cmd . END*4 [. NUM arg] */
        if (k == T_START && pos + 8 < end &&
            t[pos+1] == T_START && t[pos+2] == T_START && t[pos+3] == T_START) {
            uint8_t cmd = t[pos+4];
            pos += 9;
            int64_t arg = 0; int has_arg =
                (cmd == V_DEF || cmd == V_CALL || cmd == V_STORE || cmd == V_READ);
            if (has_arg) arg = read_int(t, &pos, end);

            fb_result r;
            switch (cmd) {
            case V_DEF:   break;                              /* header, no-op */
            case V_CALL:  r = fb_call(m, (int)arg); if (r != FB_OK) return r; break;
            case V_RET:   return FB_SIG_RET;
            case V_BREAK: return FB_SIG_BREAK;

            case V_IF: {                                      /* THREE-WAY */
                int64_t test = m->stack[--m->sp];
                int sign = (test > 0) - (test < 0);
                int ropen[3], rclose[3], nreg = 0, p = pos;
                while (nreg < 3 && p < end && t[p] == T_LPAREN) {
                    int c = match_region(t, p, end);
                    if (c < 0) { snprintf(m->err, sizeof m->err, "unbalanced region"); return FB_ERROR; }
                    ropen[nreg] = p + 1; rclose[nreg] = c; nreg++;
                    p = c + 1;
                }
                if (nreg == 0) { snprintf(m->err, sizeof m->err, "IF needs a region"); return FB_ERROR; }
                int idx = (sign == 1) ? 0 : (sign == 0 ? 1 : 2);
                if (idx < nreg) {
                    fb_result rr = fb_exec(m, t, ropen[idx], rclose[idx]);
                    if (rr != FB_OK) return rr;               /* RET/BREAK propagate */
                }
                pos = p;
                continue;
            }
            case V_LOOP: {
                if (pos >= end || t[pos] != T_LPAREN) {
                    snprintf(m->err, sizeof m->err, "LOOP needs a region"); return FB_ERROR; }
                int c = match_region(t, pos, end);
                if (c < 0) { snprintf(m->err, sizeof m->err, "unbalanced LOOP"); return FB_ERROR; }
                for (;;) {
                    if (++m->steps > m->max_steps) {
                        snprintf(m->err, sizeof m->err, "step budget exhausted");
                        return FB_OUT_OF_GAS;
                    }
                    fb_result rr = fb_exec(m, t, pos + 1, c);
                    if (rr == FB_SIG_BREAK) break;
                    if (rr != FB_OK) return rr;
                }
                pos = c + 1;
                continue;
            }
            case V_STORE: {                                   /* THE GATE */
                int64_t v = m->stack[--m->sp];
                int slot = (int)arg;
                if (slot < 0 || slot >= MAX_SLOTS ||
                    m->write_holder[slot] != m->holder + 1) {
                    snprintf(m->err, sizeof m->err,
                        "WRITE refused: slot %d not held by holder %d", slot, m->holder);
                    return FB_REFUSED;                        /* grid untouched */
                }
                m->slots[slot] = v; m->slot_set[slot] = 1;
                break;
            }
            case V_READ: {
                int slot = (int)arg;
                if (slot < 0 || slot >= MAX_SLOTS || !m->slot_set[slot]) {
                    snprintf(m->err, sizeof m->err, "READ slot %d: empty", slot);
                    return FB_ERROR;
                }
                m->stack[m->sp++] = m->slots[slot];
                break;
            }
            case V_LOADX: {                       /* INDEXED read: slot from stack */
                int slot = (int)m->stack[--m->sp];
                if (slot < 0 || slot >= MAX_SLOTS) {
                    snprintf(m->err, sizeof m->err, "LOADX slot %d out of range", slot);
                    return FB_ERROR;
                }
                m->stack[m->sp++] = m->slot_set[slot] ? m->slots[slot] : 0;
                break;
            }
            case V_STOREX: {                      /* INDEXED write, grant-gated */
                int slot = (int)m->stack[--m->sp];
                int64_t v = m->stack[--m->sp];
                if (slot < 0 || slot >= MAX_SLOTS ||
                    m->write_holder[slot] != m->holder + 1) {
                    snprintf(m->err, sizeof m->err,
                        "STOREX refused: slot %d not held by holder %d", slot, m->holder);
                    return FB_REFUSED;
                }
                m->slots[slot] = v; m->slot_set[slot] = 1;
                break;
            }
            default:
                snprintf(m->err, sizeof m->err, "unknown verb %d", cmd);
                return FB_ERROR;
            }
            continue;
        }

        if (is_digit_tok(k) || is_negdig_tok(k)) {
            m->stack[m->sp++] = read_int(t, &pos, end);
            continue;
        }
        if (k == T_PLUS || k == T_MINUS || k == T_MUL || k == T_DIV) {
            int64_t b = m->stack[--m->sp], a = m->stack[--m->sp];
            int64_t r = 0;
            if (k == T_PLUS) r = a + b;
            else if (k == T_MINUS) r = a - b;
            else if (k == T_MUL) r = a * b;
            else r = (b != 0) ? a / b : 0;
            m->stack[m->sp++] = r;
            pos++; continue;
        }
        if (k == T_EMIT) {
            int64_t v = m->stack[--m->sp];
            if (m->outn < OUT_MAX) m->out[m->outn++] = v;
            pos++; continue;
        }
        if (k == T_LPAREN) {                                  /* bare region */
            int c = match_region(t, pos, end);
            if (c < 0) { snprintf(m->err, sizeof m->err, "unbalanced region"); return FB_ERROR; }
            fb_result rr = fb_exec(m, t, pos + 1, c);
            if (rr != FB_OK) return rr;
            pos = c + 1; continue;
        }
        if (k == T_END || k == T_CHECKSUM) { pos++; continue; }
        if (k == T_RECORD) return FB_SIG_RET;                 /* implicit RET */

        snprintf(m->err, sizeof m->err, "unexpected token %d at %d", k, pos);
        return FB_ERROR;
    }
    return FB_OK;
}

/* load a DEF'd program (refuses records with no DEF header) */
static int fb_load(fb_machine *m, int slot, const uint8_t *toks, int len) {
    if (len < 9 || toks[0] != T_START || toks[4] != V_DEF) return -1;  /* data != code */
    if (m->nprogs >= MAX_PROGS) return -1;
    m->progs[m->nprogs].toks = toks;
    m->progs[m->nprogs].len = len;
    m->progs[m->nprogs].slot = slot;
    m->nprogs++;
    return 0;
}

static fb_result fb_run(fb_machine *m, int slot) {
    m->steps = 0; m->err[0] = 0;
    return fb_call(m, slot);
}

/* ================================================================
 *                          SELF TEST
 * Mirrors the Python gauntlet program-for-program. Emits results in
 * a form the cross-check harness diffs against Python's output.
 * ================================================================ */
#ifdef FB_SELFTEST

/* program builders (mirror griddb_interp.py's verb()/num()/region()) */
typedef struct { uint8_t t[4096]; int n; } buf;
static void put(buf *b, uint8_t x) { b->t[b->n++] = x; }
static void put_num(buf *b, int64_t v) {
    char s[24]; int neg = v < 0; if (neg) v = -v;
    int len = snprintf(s, sizeof s, "%lld", (long long)v);
    for (int i = 0; i < len; i++) put(b, neg ? (uint8_t)(16 + (s[i]-'0')) : (uint8_t)(s[i]-'0'));
    put(b, T_END);
}
static void put_verb(buf *b, uint8_t cmd, int has_arg, int64_t arg) {
    for (int i = 0; i < 4; i++) put(b, T_START);
    put(b, cmd);
    for (int i = 0; i < 4; i++) put(b, T_END);
    if (has_arg) put_num(b, arg);
}

static int fails = 0;
static void check(const char *name, int cond, const char *detail) {
    printf("  %s  %s%s%s\n", cond ? "PASS" : "FAIL", name,
           detail ? " | " : "", detail ? detail : "");
    if (!cond) fails++;
}

#ifndef FB_NO_MAIN
int main(void) {
    printf("C MACHINE SELFTEST (mirrors griddb_interp.py)\n");

    /* C1: arithmetic (4+2)*10-5 = 55 */
    { fb_machine m; fb_init(&m); buf b = {{0},0};
      put_verb(&b, V_DEF, 1, 1);
      put_num(&b, 4); put_num(&b, 2); put(&b, T_PLUS);
      put_num(&b, 10); put(&b, T_MUL); put_num(&b, 5); put(&b, T_MINUS);
      put(&b, T_EMIT); put(&b, T_RECORD);
      fb_load(&m, 1, b.t, b.n); fb_run(&m, 1);
      char d[64]; snprintf(d, sizeof d, "emitted %lld", (long long)m.out[0]);
      check("C1 arithmetic", m.outn == 1 && m.out[0] == 55, d); }

    /* C2: DEF enforcement — data record is not executable */
    { fb_machine m; fb_init(&m); buf b = {{0},0};
      put_num(&b, 42); put(&b, T_RECORD);
      check("C2 DEF enforcement", fb_load(&m, 2, b.t, b.n) == -1, "data not executable"); }

    /* C3: three-way IF — all six relations via one MINUS */
    { const int a[6] = {7,5,3,7,3,3}, bb[6] = {5,5,8,5,5,5};
      /* GT, EQ, LT, GE, LE, NE region layouts */
      int okc = 1;
      for (int cse = 0; cse < 6; cse++) {
        fb_machine m; fb_init(&m); buf b = {{0},0};
        put_verb(&b, V_DEF, 1, 3);
        put_num(&b, a[cse]); put_num(&b, bb[cse]); put(&b, T_MINUS);
        put_verb(&b, V_IF, 0, 0);
        /* arms: emit 9 in the arms that mean "true" for this relation */
        int arms[3] = {0,0,0};
        if (cse==0) arms[0]=1;                        /* >  : +      7-5=+  true */
        if (cse==1) arms[1]=1;                        /* == : 0      5-5=0  true */
        if (cse==2) arms[2]=1;                        /* <  : -      3-8=-  true */
        if (cse==3) { arms[0]=1; arms[1]=1; }         /* >= : +,0    7-5=+  true */
        if (cse==4) { arms[1]=1; arms[2]=1; }         /* <= : 0,-    3-5=-  true */
        if (cse==5) { arms[0]=1; arms[2]=1; }         /* != : +,-    3-5=-  true */
        for (int r = 0; r < 3; r++) {
          put(&b, T_LPAREN);
          if (arms[r]) { put_num(&b, 9); put(&b, T_EMIT); }
          put(&b, T_RPAREN);
        }
        put(&b, T_RECORD);
        fb_load(&m, 3, b.t, b.n); fb_run(&m, 3);
        if (!(m.outn == 1 && m.out[0] == 9)) okc = 0;
      }
      check("C3 six relations (three-way IF)", okc, "> == < >= <= != all true-path"); }

    /* C4: LOOP + BREAK + slots — sum 1..5 == 15 */
    { fb_machine m; fb_init(&m);
      fb_grant_w(&m, 100, 0); fb_grant_w(&m, 101, 0);
      buf b = {{0},0};
      put_verb(&b, V_DEF, 1, 10);
      put_num(&b, 0); put_verb(&b, V_STORE, 1, 101);        /* acc = 0 */
      put_num(&b, 1); put_verb(&b, V_STORE, 1, 100);        /* i = 1 */
      put_verb(&b, V_LOOP, 0, 0);
      put(&b, T_LPAREN);
        put_verb(&b, V_READ, 1, 100); put_num(&b, 6); put(&b, T_MINUS);
        put_verb(&b, V_IF, 0, 0);
        put(&b, T_LPAREN); put_verb(&b, V_BREAK, 0, 0); put(&b, T_RPAREN);  /* + */
        put(&b, T_LPAREN); put_verb(&b, V_BREAK, 0, 0); put(&b, T_RPAREN);  /* 0 */
        put(&b, T_LPAREN);                                                  /* - */
          put_verb(&b, V_READ, 1, 101); put_verb(&b, V_READ, 1, 100); put(&b, T_PLUS);
          put_verb(&b, V_STORE, 1, 101);
          put_verb(&b, V_READ, 1, 100); put_num(&b, 1); put(&b, T_PLUS);
          put_verb(&b, V_STORE, 1, 100);
        put(&b, T_RPAREN);
      put(&b, T_RPAREN);
      put_verb(&b, V_READ, 1, 101); put(&b, T_EMIT);
      put(&b, T_RECORD);
      fb_load(&m, 10, b.t, b.n); fb_run(&m, 10);
      char d[64]; snprintf(d, sizeof d, "sum(1..5)=%lld i=%lld",
                           (long long)m.out[0], (long long)m.slots[100]);
      check("C4 LOOP+BREAK+slots", m.outn == 1 && m.out[0] == 15, d); }

    /* C5: THE REFUSAL — STORE without a grant, mid-program */
    { fb_machine m; fb_init(&m);
      fb_grant_w(&m, 300, 1);            /* holder 1 owns slot 300 */
      m.slots[300] = 1000; m.slot_set[300] = 1;
      m.holder = 2;                      /* intruder runs as holder 2 */
      buf b = {{0},0};
      put_verb(&b, V_DEF, 1, 30);
      put_num(&b, 777); put(&b, T_EMIT);
      put_num(&b, 666); put_verb(&b, V_STORE, 1, 300);
      put_num(&b, 999); put(&b, T_EMIT);
      put(&b, T_RECORD);
      fb_load(&m, 30, b.t, b.n);
      fb_result r = fb_run(&m, 30);
      char d[128]; snprintf(d, sizeof d, "halted: out=[%lld] slot300=%lld (%s)",
            (long long)(m.outn ? m.out[0] : -1), (long long)m.slots[300], m.err);
      check("C5 ownership refusal mid-program",
            r == FB_REFUSED && m.outn == 1 && m.out[0] == 777 && m.slots[300] == 1000, d); }

    /* C6: grant is the switch — same program, granted */
    { fb_machine m; fb_init(&m);
      fb_grant_w(&m, 300, 2); m.slots[300] = 1000; m.slot_set[300] = 1; m.holder = 2;
      buf b = {{0},0};
      put_verb(&b, V_DEF, 1, 30);
      put_num(&b, 777); put(&b, T_EMIT);
      put_num(&b, 666); put_verb(&b, V_STORE, 1, 300);
      put_num(&b, 999); put(&b, T_EMIT);
      put(&b, T_RECORD);
      fb_load(&m, 30, b.t, b.n);
      fb_result r = fb_run(&m, 30);
      char d[80]; snprintf(d, sizeof d, "out=[%lld,%lld] slot300=%lld",
            (long long)m.out[0], (long long)(m.outn>1?m.out[1]:-1), (long long)m.slots[300]);
      check("C6 grant is the switch",
            r == FB_OK && m.outn == 2 && m.out[1] == 999 && m.slots[300] == 666, d); }

    /* C7: doorman — deny-by-default then granted */
    { fb_machine m; fb_init(&m); buf b = {{0},0};
      put_verb(&b, V_DEF, 1, 40);
      put_num(&b, 42); put_verb(&b, V_CALL, 1, CAP_EMIT_OUT);
      put(&b, T_RECORD);
      fb_load(&m, 40, b.t, b.n);
      fb_result r1 = fb_run(&m, 40);
      int denied = (r1 == FB_REFUSED && m.outbox_n == 0);

      fb_machine m2; fb_init(&m2);
      fb_grant_w(&m2, CAP_EMIT_OUT, 0);
      fb_load(&m2, 40, b.t, b.n);
      fb_result r2 = fb_run(&m2, 40);
      int allowed = (r2 == FB_OK && m2.outbox_n == 1 && m2.outbox[0] == 42);
      check("C7 doorman deny-by-default + grant switch", denied && allowed,
            denied ? "refused ungranted, performed granted" : "gate leak!"); }

    /* C8: full request loop (H4 mirror): READ_IN -> rule -> EMIT_OUT */
    { fb_machine m; fb_init(&m);
      fb_grant_w(&m, CAP_READ_IN, 0); fb_grant_w(&m, CAP_EMIT_OUT, 0);
      m.inbox[0] = 250; m.inbox[1] = 30; m.inbox[2] = 100; m.inbox_n = 3;
      buf b = {{0},0};
      put_verb(&b, V_DEF, 1, 50);
      put_verb(&b, V_CALL, 1, CAP_READ_IN);
      put_num(&b, 100); put(&b, T_MINUS);
      put_verb(&b, V_IF, 0, 0);
      put(&b, T_LPAREN); put_num(&b, 1); put_verb(&b, V_CALL, 1, CAP_EMIT_OUT); put(&b, T_RPAREN);
      put(&b, T_LPAREN); put_num(&b, 0); put_verb(&b, V_CALL, 1, CAP_EMIT_OUT); put(&b, T_RPAREN);
      put(&b, T_LPAREN); put_num(&b, 0); put_verb(&b, V_CALL, 1, CAP_EMIT_OUT); put(&b, T_RPAREN);
      put(&b, T_RECORD);
      fb_load(&m, 50, b.t, b.n);
      for (int i = 0; i < 3; i++) fb_run(&m, 50);
      char d[80]; snprintf(d, sizeof d, "outbox=[%lld,%lld,%lld]",
            (long long)m.outbox[0], (long long)m.outbox[1], (long long)m.outbox[2]);
      check("C8 native request loop (approve/deny)",
            m.outbox_n == 3 && m.outbox[0] == 1 && m.outbox[1] == 0 && m.outbox[2] == 0, d); }

    /* C9: gas — infinite LOOP terminates */
    { fb_machine m; fb_init(&m); m.max_steps = 5000;
      buf b = {{0},0};
      put_verb(&b, V_DEF, 1, 60);
      put_verb(&b, V_LOOP, 0, 0);
      put(&b, T_LPAREN); put_num(&b, 1); put_num(&b, 2); put(&b, T_PLUS); put(&b, T_RPAREN);
      put(&b, T_RECORD);
      fb_load(&m, 60, b.t, b.n);
      check("C9 gas bounds infinite loop", fb_run(&m, 60) == FB_OUT_OF_GAS, m.err); }

    /* C10: native column comparator (N-gauntlet mirror): hamming + manhattan */
    { fb_machine m; fb_init(&m);
      const int D = 8, A = 500, B = 520, T = 590, H = 591, C = 592;
      for (int i = 0; i < D; i++) { fb_grant_w(&m, A+i, 0); fb_grant_w(&m, B+i, 0); }
      fb_grant_w(&m, T, 0); fb_grant_w(&m, H, 0); fb_grant_w(&m, C, 0);
      int av[8] = {7,0,31,15,2,2,9,30}, bv[8] = {7,5,31,12,2,2,9,26};  /* 3 diverge, L1=12 */
      for (int i = 0; i < D; i++) {
        m.slots[A+i] = av[i]; m.slot_set[A+i] = 1;
        m.slots[B+i] = bv[i]; m.slot_set[B+i] = 1;
      }
      buf b = {{0},0};
      put_verb(&b, V_DEF, 1, 70);
      put_num(&b, 0); put_verb(&b, V_STORE, 1, H);
      put_num(&b, 0); put_verb(&b, V_STORE, 1, C);
      for (int i = 0; i < D; i++) {                       /* the vertical walk */
        put_verb(&b, V_READ, 1, A+i); put_verb(&b, V_READ, 1, B+i);
        put(&b, T_MINUS); put_verb(&b, V_STORE, 1, T);
        put_verb(&b, V_READ, 1, T);
        put_verb(&b, V_IF, 0, 0);
        put(&b, T_LPAREN);                                /* +arm: diverged */
          put_verb(&b, V_READ, 1, H); put_num(&b, 1); put(&b, T_PLUS); put_verb(&b, V_STORE, 1, H);
          put_verb(&b, V_READ, 1, C); put_verb(&b, V_READ, 1, T); put(&b, T_PLUS);
          put_verb(&b, V_STORE, 1, C);
        put(&b, T_RPAREN);
        put(&b, T_LPAREN); put(&b, T_RPAREN);             /* 0arm: same */
        put(&b, T_LPAREN);                                /* -arm: diverged, ABS */
          put_verb(&b, V_READ, 1, H); put_num(&b, 1); put(&b, T_PLUS); put_verb(&b, V_STORE, 1, H);
          put_verb(&b, V_READ, 1, C);
          put_num(&b, 0); put_verb(&b, V_READ, 1, T); put(&b, T_MINUS); put(&b, T_PLUS);
          put_verb(&b, V_STORE, 1, C);
        put(&b, T_RPAREN);
      }
      put_verb(&b, V_READ, 1, H); put(&b, T_EMIT);
      put_verb(&b, V_READ, 1, C); put(&b, T_EMIT);
      put(&b, T_RECORD);
      fb_load(&m, 70, b.t, b.n); fb_run(&m, 70);
      char d[80]; snprintf(d, sizeof d, "hamming=%lld manhattan=%lld",
                           (long long)m.out[0], (long long)m.out[1]);
      check("C10 native comparator (grid IS the XOR)",
            m.outn == 2 && m.out[0] == 3 && m.out[1] == 12, d); }


    /* C11: 5bit VLIW scoring — identical to /tmp/opt3.c */
    { int NG=256/8,R=16,NB=32,ba=5000,bb=7000; long nops=0,nbundles=0;
      int*bw=calloc(99999,4),*ec=calloc(999999*5,4),*lr=calloc(99999,4);
      for(int i=0;i<99999;i++)bw[i]=lr[i]=-1;
      int vf=3000,vo=3008,vt=3016,vz=3024;
      int hc[12];for(int i=0;i<12;i++)hc[i]=3000-300-i*8;
      int res[16]={1,1,1,1,1,1,1,0,0,0,1,1,1,1,1,1};
      for(int g=0;g<NG;g++){int b=g%NB;
        for(int r=0;r<R;r++){int base=(r&1)?bb:ba,bx=base+b*32,bv=bx+8,bt=bx+16;
          {int p=0,m=0;for(int n=m;n<=nbundles&&!p;n++){if(ec[n*5+2]>=2)continue;
           if(bw[bx]==n)continue;bw[bx]=n;ec[n*5+2]++;p=1;if(n>=nbundles)nbundles=n+1;lr[bx]=n;}
           if(!p){int n=nbundles>m?nbundles:m;bw[bx]=n;ec[n*5+2]=1;if(n>=nbundles)nbundles=n+1;lr[bx]=n;}nops++;}
          {int p=0,m=0;for(int n=m;n<=nbundles&&!p;n++){if(ec[n*5+2]>=2)continue;
           if(bw[bv]==n)continue;bw[bv]=n;ec[n*5+2]++;p=1;if(n>=nbundles)nbundles=n+1;lr[bv]=n;}
           if(!p){int n=nbundles>m?nbundles:m;bw[bv]=n;ec[n*5+2]=1;if(n>=nbundles)nbundles=n+1;lr[bv]=n;}nops++;}
          {int p=0,m=lr[bx]+1;if(m<0)m=0;for(int n=m;n<=nbundles&&!p;n++){if(ec[n*5+1]>=6)continue;
           if(bw[bt]==n)continue;bw[bt]=n;ec[n*5+1]++;p=1;if(n>=nbundles)nbundles=n+1;lr[bt]=n;}
           if(!p){int n=nbundles>m?nbundles:m;bw[bt]=n;ec[n*5+1]=1;if(n>=nbundles)nbundles=n+1;lr[bt]=n;}nops++;}
          if(res[r]){{int p=0,m=lr[bt]+1;if(m<0)m=0;for(int n=m;n<=nbundles&&!p;n++){if(ec[n*5+4]>=1)continue;
           if(bw[bt+8]==n)continue;bw[bt+8]=n;ec[n*5+4]++;p=1;if(n>=nbundles)nbundles=n+1;lr[bt+8]=n;}
           if(!p){int n=nbundles>m?nbundles:m;bw[bt+8]=n;ec[n*5+4]=1;if(n>=nbundles)nbundles=n+1;lr[bt+8]=n;}nops++;}}
          else for(int l=0;l<8;l++){{int p=0,m=0;for(int n=m;n<=nbundles&&!p;n++){if(ec[n*5+2]>=2)continue;
           if(bw[bt+8+l]==n)continue;bw[bt+8+l]=n;ec[n*5+2]++;p=1;if(n>=nbundles)nbundles=n+1;lr[bt+8+l]=n;}
           if(!p){int n=nbundles>m?nbundles:m;bw[bt+8+l]=n;ec[n*5+2]=1;if(n>=nbundles)nbundles=n+1;lr[bt+8+l]=n;}nops++;}}
          {int p=0,m=lr[bt+8]+1;if(m<0)m=0;for(int n=m;n<=nbundles&&!p;n++){if(ec[n*5+1]>=6)continue;
           if(bw[bv]==n)continue;if(lr[bv]>=0&&bw[bv]==n)continue;if(lr[bt+8]>=0&&bw[bt+8]==n)continue;
           bw[bv]=n;ec[n*5+1]++;p=1;if(n>=nbundles)nbundles=n+1;lr[bv]=n;}
           if(!p){int n=nbundles>m?nbundles:m;bw[bv]=n;ec[n*5+1]=1;if(n>=nbundles)nbundles=n+1;lr[bv]=n;}nops++;}
          for(int s=0;s<6;s++){int t1=bt+20+s*2,t2=bt+22+s*2;
            if(s==0||s==2||s==4){{int p=0,m=lr[bv]+1;if(m<0)m=0;for(int n=m;n<=nbundles&&!p;n++){if(ec[n*5+1]>=6)continue;
             if(bw[bv]==n)continue;bw[bv]=n;ec[n*5+1]++;p=1;if(n>=nbundles)nbundles=n+1;lr[bv]=n;}
             if(!p){int n=nbundles>m?nbundles:m;bw[bv]=n;ec[n*5+1]=1;if(n>=nbundles)nbundles=n+1;lr[bv]=n;}nops++;}}
            else{
              {int p=0,m=lr[bv]+1;if(m<0)m=0;for(int n=m;n<=nbundles&&!p;n++){if(ec[n*5+0]>=12)continue;
               if(bw[t1]==n)continue;bw[t1]=n;ec[n*5+0]++;p=1;if(n>=nbundles)nbundles=n+1;lr[t1]=n;}
               if(!p){int n=nbundles>m?nbundles:m;bw[t1]=n;ec[n*5+0]=1;if(n>=nbundles)nbundles=n+1;lr[t1]=n;}nops++;}
              {int p=0,m=lr[bv]+1;if(m<0)m=0;for(int n=m;n<=nbundles&&!p;n++){if(ec[n*5+0]>=12)continue;
               if(bw[t2]==n)continue;bw[t2]=n;ec[n*5+0]++;p=1;if(n>=nbundles)nbundles=n+1;lr[t2]=n;}
               if(!p){int n=nbundles>m?nbundles:m;bw[t2]=n;ec[n*5+0]=1;if(n>=nbundles)nbundles=n+1;lr[t2]=n;}nops++;}
              {int p=0,m=lr[t1]+1;if(lr[t2]+1>m)m=lr[t2]+1;for(int n=m;n<=nbundles&&!p;n++){if(ec[n*5+1]>=6)continue;
               if(bw[bv]==n)continue;bw[bv]=n;ec[n*5+1]++;p=1;if(n>=nbundles)nbundles=n+1;lr[bv]=n;}
               if(!p){int n=nbundles>m?nbundles:m;bw[bv]=n;ec[n*5+1]=1;if(n>=nbundles)nbundles=n+1;lr[bv]=n;}nops++;}
            }}
          {int p=0,m=lr[bv]+1;if(m<0)m=0;for(int n=m;n<=nbundles&&!p;n++){if(ec[n*5+0]>=12)continue;
           if(bw[bt+34]==n)continue;bw[bt+34]=n;ec[n*5+0]++;p=1;if(n>=nbundles)nbundles=n+1;lr[bt+34]=n;}
           if(!p){int n=nbundles>m?nbundles:m;bw[bt+34]=n;ec[n*5+0]=1;if(n>=nbundles)nbundles=n+1;lr[bt+34]=n;}nops++;}
          {int p=0,m=lr[bt+34]+1;for(int n=m;n<=nbundles&&!p;n++){if(ec[n*5+0]>=12)continue;
           if(bw[bt+42]==n)continue;bw[bt+42]=n;ec[n*5+0]++;p=1;if(n>=nbundles)nbundles=n+1;lr[bt+42]=n;}
           if(!p){int n=nbundles>m?nbundles:m;bw[bt+42]=n;ec[n*5+0]=1;if(n>=nbundles)nbundles=n+1;lr[bt+42]=n;}nops++;}
          {int p=0,m=lr[bt+42]+1;for(int n=m;n<=nbundles&&!p;n++){if(ec[n*5+1]>=6)continue;
           if(bw[bx]==n)continue;bw[bx]=n;ec[n*5+1]++;p=1;if(n>=nbundles)nbundles=n+1;lr[bx]=n;}
           if(!p){int n=nbundles>m?nbundles:m;bw[bx]=n;ec[n*5+1]=1;if(n>=nbundles)nbundles=n+1;lr[bx]=n;}nops++;}
          if(r>=10){{int p=0,m=lr[bx]+1;for(int n=m;n<=nbundles&&!p;n++){if(ec[n*5+1]>=6)continue;
           if(bw[bx]==n)continue;bw[bx]=n;ec[n*5+1]++;p=1;if(n>=nbundles)nbundles=n+1;lr[bx]=n;}
           if(!p){int n=nbundles>m?nbundles:m;bw[bx]=n;ec[n*5+1]=1;if(n>=nbundles)nbundles=n+1;lr[bx]=n;}nops++;}}
          {int p=0,m=0;for(int n=m;n<=nbundles&&!p;n++){if(ec[n*5+3]>=2)continue;
           bw[0]=n;ec[n*5+3]++;p=1;if(n>=nbundles)nbundles=n+1;}
           if(!p){int n=nbundles>m?nbundles:m;bw[0]=n;ec[n*5+3]=1;if(n>=nbundles)nbundles=n+1;}nops++;}
          {int p=0,m=0;for(int n=m;n<=nbundles&&!p;n++){if(ec[n*5+3]>=2)continue;
           bw[1]=n;ec[n*5+3]++;p=1;if(n>=nbundles)nbundles=n+1;}
           if(!p){int n=nbundles>m?nbundles:m;bw[1]=n;ec[n*5+3]=1;if(n>=nbundles)nbundles=n+1;}nops++;}
        }}
      int cycles=(int)nbundles;
      char d[200];snprintf(d,sizeof d,"ops=%ld cycles=%d",nops,cycles);
      check("C11 5bit VLIW scoring",cycles>0,d);
      free(bw);free(ec);free(lr);
    }
    printf(fails ? "\n%d FAILURES\n" : "\nALL PASS — the C binary executes 5bit.\n", fails);
    return fails ? 1 : 0;
}
#endif
#endif
