#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define FB_NO_MAIN
#include "/Users/georgeikanos/griddb/c/fivebit_interp.c"

/* Minimal 5-bit unpack: bytes -> tokens */
static int unpack5(const uint8_t *data, int nbytes, int pad, uint8_t *tk, int max_tok) {
    uint64_t acc = 0; int nbits = 0, ntok = 0;
    for (int i = 0; i < nbytes && ntok < max_tok; i++) {
        acc = (acc << 8) | data[i]; nbits += 8;
        while (nbits >= 5 && ntok < max_tok) { nbits -= 5; tk[ntok++] = (acc >> nbits) & 0x1F; }
    }
    return ntok;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: 5bit <file.5b>\n"); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(sz); fread(data, 1, sz, f); fclose(f);
    int pad = data[0];
    int nbytes = (int)sz - 1;

    /* Unpack packed bytes -> token array */
    int total_bits = nbytes * 8 - pad;
    int ntok = total_bits / 5;
    uint8_t *tokens = malloc(ntok);
    unpack5(data + 1, nbytes, pad, tokens, ntok);

    fb_machine m; fb_init(&m);
    for (int s = 0; s < 2048; s++) fb_grant_w(&m, s, 0);
    fb_grant_w(&m, 9003, 0); fb_grant_w(&m, 9005, 0);

    if (fb_load(&m, 1, tokens, ntok) != 0) {
        fprintf(stderr, "Error: no DEF header (first tok=%d, expected 31)\n", tokens[0]);
        free(data); free(tokens); return 1;
    }
    fb_result r = fb_run(&m, 1);
    if (r != FB_OK) { fprintf(stderr, "Error: %s\n", m.err); free(data); free(tokens); return 1; }
    printf("[");
    for (int i = 0; i < m.outn; i++) printf("%s%lld", i?", ":"", (long long)m.out[i]);
    printf("]\n");
    free(data); free(tokens); return 0;
}
