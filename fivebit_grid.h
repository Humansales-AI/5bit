/* fivebit_grid.h — AllocGrid file management in C.
 * O(1) record reads/writes via alloc.grid index + data.grid blob store.
 * Byte-identical file format to the TypeScript AllocGrid (alloc.ts).
 */
#ifndef FIVEBIT_GRID_H
#define FIVEBIT_GRID_H

#include <stdint.h>
#include <stddef.h>

/* ── Grid entry (what alloc.grid stores per recordId) ── */
typedef struct {
  int     record_id;
  int64_t byte_offset;   /* position in data.grid */
  int32_t bit_length;    /* tokens * 5 */
  int32_t flags;         /* 0=FREE 1=ALLOCATED 2=TOMBSTONE */
} GridEntry;

/* ── A decoded record from data.grid ── */
typedef struct {
  int      record_id;
  uint8_t *tokens;        /* unpacked 5-bit tokens */
  int      token_count;
  uint8_t *parsed;        /* for caller to free: allocated buffer holding tokens */
  int      byte_offset;
  int      bit_length;
  int      is_tombstone;
} GridRecord;

/* ── File magic numbers (match TypeScript AllocGrid) ── */
#define ALLOC_MAGIC  0x414C4F43  /* "ALOC" */
#define DATA_MAGIC   0x44415441  /* "DATA" */
#define ALLOC_ENTRY_SIZE 16

/* ── Token values ── */
#define T_RECORD 28
#define T_END    30
#define T_START  31

/* ── API ──────────────────────────────────────────────────────────────── */

/* Bootstrap a grid directory. Creates alloc.grid + data.grid with headers.
 * Safe to call on an existing directory — does not overwrite. */
int  grid_init(const char *data_dir);

/* O(1): write packed token bytes at recordId. Returns byte offset in data.grid. */
int64_t grid_write(const char *data_dir, int record_id,
                   const uint8_t *packed_bytes, int byte_len,
                   int bit_length);

/* O(1): read a record. Caller must free(rec->parsed) and free(rec). */
GridRecord *grid_read(const char *data_dir, int record_id);

/* O(1): tombstone a record. Returns 0 on success. */
int  grid_delete(const char *data_dir, int record_id);

/* Number of allocated entries (derived from alloc.grid file size). */
int  grid_total_entries(const char *data_dir);

/* ── Tick-tagged WAL ──────────────────────────────────────────────────── */

#define WAL_MAGIC    0x57414C47  /* "WALG" */
#define WAL_ENTRY_HDR 48         /* magic(4) + tick(4) + prevHash(32) + action(1) + payloadLen(4) + pad(3) */
#define SHA256_LEN   32

/* Append a tick-tagged WAL entry: header + packed payload + SHA-256. */
int  wal_append(const char *data_dir, int tick_id,
                const uint8_t *prev_hash,  /* 32 bytes */
                uint8_t action_type,
                const uint8_t *payload_packed, int payload_len,
                uint8_t *out_hash);         /* 32 bytes — becomes next prev_hash */

/* Replay WAL: reads wal.grid, verifies SHA-256, executes grid_write for each entry.
 * Returns the highest tick replayed, or -1 on corruption. */
int  wal_replay(const char *data_dir);

/* Checkpoint: copy alloc+data to checkpoints/ directory, truncate WAL. */
int  wal_checkpoint(const char *data_dir, int tick_id);

/* ── Encode helpers (thin wrappers around the codec) ──────────────────── */

/* Encode a signed integer into a token buffer. Returns token count. */
int  enc_int(long long value, uint8_t *tokens, int cap);

/* Pack 5-bit tokens to bytes (MSB first, zero-padded to byte boundary).
 * Returns byte length, sets *pad_out to padding bits (0-7). */
int  pack_tokens(const uint8_t *tokens, int n, uint8_t *out, int *pad_out);

/* Encode an ASCII word into tokens. Returns token count, -1 on error. */
int  enc_word(const char *text, uint8_t *tokens, int cap);

#endif /* FIVEBIT_GRID_H */
