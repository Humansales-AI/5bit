/* bootstrap.c — THE LADDER-KICKING CEREMONY
 * ==========================================
 * Runs compiler.5b (the 5bit compiler, as tokens) on the C Machine, feeding
 * it a 5bit source program. The compiler emits x86-64 bytes into output
 * slots; we write them to a file and execute them. ZERO PYTHON.
 *
 * This is the moment: C runs the 5bit compiler, which emits native code.
 * After this, the emitted compiler compiles 5bit — C is a historical artifact.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include "compiler5b_tokens.h"

/* pull in the machine (selftest main is guarded out) */
#define FB_NO_MAIN
#include "fivebit_interp.c"

int main(void) {
    printf("=== BOOTSTRAP CEREMONY: C runs the 5bit compiler ===\n\n");

    fb_machine m; fb_init(&m); m.max_steps = 5000000;

    /* grant all slots the compiler touches (holder 0) */
    for (int s = 0; s < 2048; s++) fb_grant_w(&m, s, 0);

    /* load the source program into SRC slots: SRC_N=10, SRC_BASE=100 */
    m.slots[10] = SRC_NTOKS; m.slot_set[10] = 1;              /* SRC_N */
    for (int i = 0; i < SRC_NTOKS; i++) {
        m.slots[100 + i] = SRC_TOKS[i]; m.slot_set[100 + i] = 1;
    }

    /* load compiler.5b as a DEF'd program at its slot (1) and run it */
    if (fb_load(&m, 1, COMPILER_TOKS, COMPILER_NTOKS) != 0) {
        printf("FAILED to load compiler.5b (no DEF header?)\n"); return 1;
    }
    printf("  compiler.5b: %d tokens loaded onto the C Machine\n", COMPILER_NTOKS);
    printf("  source program: [4 2 + 3 * EMIT]  (expect (4+2)*3 = 18)\n\n");

    fb_result r = fb_run(&m, 1);
    if (r != FB_OK) { printf("  compile FAILED: %s\n", m.err); return 1; }

    /* collect emitted bytes: OUT_N=11, OUT_BASE=1000 */
    int nbytes = (int)m.slots[11];
    printf("  the 5bit compiler emitted %d bytes of x86-64:\n  ", nbytes);
    uint8_t code[512];
    for (int j = 0; j < nbytes && j < 512; j++) {
        code[j] = (uint8_t)(m.slots[1000 + j] & 0xFF);
        printf("%02x", code[j]);
    }
    printf("\n\n");

    /* EXECUTE the emitted native code */
    void *mem = mmap(0, 4096, PROT_READ|PROT_WRITE|PROT_EXEC,
                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    memcpy(mem, code, nbytes);
    int64_t (*fn)(void) = (int64_t(*)(void))mem;
    int64_t result = fn();

    printf("  native CPU executed the emitted code -> %lld\n", (long long)result);
    printf("\n  %s\n", result == 18 ?
        "*** SELF-HOSTED. C ran the 5bit compiler; the 5bit compiler emitted\n"
        "      native code; the CPU ran it. No Python. The ladder is kicked." :
        "MISMATCH");
    return result == 18 ? 0 : 1;
}
