/* test_grid.c — verify grid_write, grid_read, grid_delete, WAL end-to-end */
#include "fivebit_grid.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int main(void) {
  const char *dir = "/tmp/5bit_test";
  mkdir(dir, 0755);

  /* 1. Init grid */
  printf("1. grid_init: %s\n", grid_init(dir) == 0 ? "OK" : "FAIL");

  /* 2. Encode + write a record */
  uint8_t tk[256]; int n = 0;
  n += enc_int(42, tk + n, 256 - n);
  n += enc_word("Hello", tk + n, 256 - n);
  n += enc_int(500, tk + n, 256 - n);
  tk[n++] = T_RECORD; /* end record */

  uint8_t packed[256]; int pad;
  int plen = pack_tokens(tk, n, packed, &pad);
  int64_t off = grid_write(dir, 1, packed, plen, n * 5);
  printf("2. grid_write record 1: offset=%lld bitlen=%d %s\n", (long long)off, n*5, off >= 0 ? "OK" : "FAIL");

  /* 3. Read it back */
  GridRecord *rec = grid_read(dir, 1);
  printf("3. grid_read: record_id=%d byte_offset=%d bit_length=%d tombstone=%d %s\n",
         rec->record_id, rec->byte_offset, rec->bit_length, rec->is_tombstone,
         rec->record_id == 1 ? "OK" : "FAIL");
  free(rec->parsed); free(rec);

  /* 4. Tombstone delete */
  int dr = grid_delete(dir, 1);
  printf("4. grid_delete: %s\n", dr == 0 ? "OK" : "FAIL");
  rec = grid_read(dir, 1);
  printf("   after delete: tombstone=%d %s\n", rec->is_tombstone, rec->is_tombstone ? "OK" : "FAIL");
  free(rec->parsed); free(rec);

  /* 5. WAL append */
  uint8_t prev_hash[32] = {0}; /* genesis hash */
  uint8_t out_hash[32];
  int wr = wal_append(dir, 1, prev_hash, 1 /* action=create */, packed, plen, out_hash);
  printf("5. wal_append: %s\n", wr == 0 ? "OK" : "FAIL");
  printf("   hash: ");
  for (int i = 0; i < 8; i++) printf("%02x", out_hash[i]); printf("...\n");

  /* 6. WAL replay */
  int ht = wal_replay(dir);
  printf("6. wal_replay: highest_tick=%d %s\n", ht, ht >= 1 ? "OK" : "FAIL");

  /* 7. Checkpoint */
  int ck = wal_checkpoint(dir, 500);
  printf("7. wal_checkpoint: %s\n", ck == 0 ? "OK" : "FAIL");

  /* 8. Total entries */
  int te = grid_total_entries(dir);
  printf("8. total_entries: %d %s\n", te, te >= 1 ? "OK" : "FAIL");

  printf("\nAll tests done.\n");
  return 0;
}
