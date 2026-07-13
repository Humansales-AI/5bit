/* fivebit_grid.c — AllocGrid file management + tick-WAL.
 * Build: cc -O2 -Wall -c fivebit_grid.c → fivebit_grid.o
 * Link with -lssl -lcrypto for SHA-256.
 */
#include "fivebit_grid.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#define SHA256_LEN CC_SHA256_DIGEST_LENGTH
#else
#include <openssl/sha.h>
#define SHA256_LEN SHA256_DIGEST_LENGTH
/* Single-shot SHA-256 wrapper matching CC_SHA256 signature */
static inline void CC_SHA256(const void *data, unsigned long len, unsigned char *md) {
  SHA256((const unsigned char*)data, len, md);
}
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * Internal helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static char *_path(const char *dir, const char *name) {
  size_t dl = strlen(dir), nl = strlen(name);
  char *p = malloc(dl + nl + 2);
  if (!p) return NULL;
  memcpy(p, dir, dl); p[dl] = '/'; memcpy(p + dl + 1, name, nl + 1);
  return p;
}

static int _file_size(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) return 0;
  return (int)st.st_size;
}

static int _write_all(int fd, const uint8_t *buf, int len) {
  int w = 0;
  while (w < len) {
    int n = (int)write(fd, buf + w, len - w);
    if (n <= 0) return -1;
    w += n;
  }
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Grid Init — bootstrap alloc.grid + data.grid
 * ═══════════════════════════════════════════════════════════════════════════ */

int grid_init(const char *data_dir) {
  char *ap = _path(data_dir, "alloc.grid");
  char *dp = _path(data_dir, "data.grid");
  int ok = 0;

  /* alloc.grid */
  if (access(ap, F_OK) != 0) {
    int fd = open(ap, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { ok = -1; goto done; }
    uint8_t hdr[8];
    hdr[0] = (ALLOC_MAGIC >> 24) & 0xFF;
    hdr[1] = (ALLOC_MAGIC >> 16) & 0xFF;
    hdr[2] = (ALLOC_MAGIC >> 8) & 0xFF;
    hdr[3] = ALLOC_MAGIC & 0xFF;
    hdr[4] = 0; hdr[5] = 0; hdr[6] = 0; hdr[7] = 1; /* version 1 */
    if (_write_all(fd, hdr, 8) != 0) { close(fd); ok = -1; goto done; }
    fsync(fd); close(fd);
  }

  /* data.grid */
  if (access(dp, F_OK) != 0) {
    int fd = open(dp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { ok = -1; goto done; }
    uint8_t hdr[12];
    hdr[0] = (DATA_MAGIC >> 24) & 0xFF;
    hdr[1] = (DATA_MAGIC >> 16) & 0xFF;
    hdr[2] = (DATA_MAGIC >> 8) & 0xFF;
    hdr[3] = DATA_MAGIC & 0xFF;
    /* dataEnd = 12 */
    hdr[4] = 0; hdr[5] = 0; hdr[6] = 0; hdr[7] = 0;
    hdr[8] = 0; hdr[9] = 0; hdr[10] = 0; hdr[11] = 12;
    if (_write_all(fd, hdr, 12) != 0) { close(fd); ok = -1; goto done; }
    fsync(fd); close(fd);
  }

done:
  free(ap); free(dp);
  return ok;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Alloc entry read/write
 * ═══════════════════════════════════════════════════════════════════════════ */

static int _read_alloc_entry(const char *alloc_path, int record_id, GridEntry *out) {
  memset(out, 0, sizeof(*out));
  out->record_id = record_id;
  int off = 8 + record_id * ALLOC_ENTRY_SIZE;
  int fd = open(alloc_path, O_RDONLY);
  if (fd < 0) return -1;
  uint8_t buf[ALLOC_ENTRY_SIZE];
  int n = (int)pread(fd, buf, ALLOC_ENTRY_SIZE, off);
  close(fd);
  if (n < ALLOC_ENTRY_SIZE) return 0; /* past EOF = free slot */
  out->byte_offset = ((int64_t)buf[0] << 56) | ((int64_t)buf[1] << 48) |
                     ((int64_t)buf[2] << 40) | ((int64_t)buf[3] << 32) |
                     ((int64_t)buf[4] << 24) | ((int64_t)buf[5] << 16) |
                     ((int64_t)buf[6] << 8)  | (int64_t)buf[7];
  out->bit_length  = ((int32_t)buf[8] << 24) | ((int32_t)buf[9] << 16) |
                     ((int32_t)buf[10] << 8) | (int32_t)buf[11];
  out->flags       = ((int32_t)buf[12] << 24) | ((int32_t)buf[13] << 16) |
                     ((int32_t)buf[14] << 8) | (int32_t)buf[15];
  return 1;
}

static int _write_alloc_entry(const char *alloc_path, const GridEntry *entry) {
  int off = 8 + entry->record_id * ALLOC_ENTRY_SIZE;
  int fd = open(alloc_path, O_RDWR);
  if (fd < 0) return -1;

  /* ensure file is large enough */
  int needed = off + ALLOC_ENTRY_SIZE;
  int sz = _file_size(alloc_path);
  if (sz < needed) {
    if (ftruncate(fd, needed) != 0) { close(fd); return -1; }
  }

  uint8_t buf[ALLOC_ENTRY_SIZE];
  int64_t bo = entry->byte_offset;
  buf[0]  = (bo >> 56) & 0xFF; buf[1] = (bo >> 48) & 0xFF;
  buf[2]  = (bo >> 40) & 0xFF; buf[3] = (bo >> 32) & 0xFF;
  buf[4]  = (bo >> 24) & 0xFF; buf[5] = (bo >> 16) & 0xFF;
  buf[6]  = (bo >> 8)  & 0xFF; buf[7] = bo & 0xFF;
  int32_t bl = entry->bit_length;
  buf[8]  = (bl >> 24) & 0xFF; buf[9]  = (bl >> 16) & 0xFF;
  buf[10] = (bl >> 8)  & 0xFF; buf[11] = bl & 0xFF;
  int32_t fl = entry->flags;
  buf[12] = (fl >> 24) & 0xFF; buf[13] = (fl >> 16) & 0xFF;
  buf[14] = (fl >> 8)  & 0xFF; buf[15] = fl & 0xFF;

  int n = (int)pwrite(fd, buf, ALLOC_ENTRY_SIZE, off);
  fsync(fd); close(fd);
  return (n == ALLOC_ENTRY_SIZE) ? 0 : -1;
}

static int64_t _read_data_end(const char *data_path) {
  int fd = open(data_path, O_RDONLY);
  if (fd < 0) return 12;
  uint8_t buf[8];
  pread(fd, buf, 8, 4);
  close(fd);
  return ((int64_t)buf[0] << 56) | ((int64_t)buf[1] << 48) |
         ((int64_t)buf[2] << 40) | ((int64_t)buf[3] << 32) |
         ((int64_t)buf[4] << 24) | ((int64_t)buf[5] << 16) |
         ((int64_t)buf[6] << 8)  | (int64_t)buf[7];
}

static int _write_data_end(const char *data_path, int64_t end) {
  int fd = open(data_path, O_RDWR);
  if (fd < 0) return -1;
  uint8_t buf[8];
  buf[0] = (end >> 56) & 0xFF; buf[1] = (end >> 48) & 0xFF;
  buf[2] = (end >> 40) & 0xFF; buf[3] = (end >> 32) & 0xFF;
  buf[4] = (end >> 24) & 0xFF; buf[5] = (end >> 16) & 0xFF;
  buf[6] = (end >> 8)  & 0xFF; buf[7] = end & 0xFF;
  pwrite(fd, buf, 8, 4);
  fsync(fd); close(fd);
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public: grid_write
 * ═══════════════════════════════════════════════════════════════════════════ */

int64_t grid_write(const char *data_dir, int record_id,
                   const uint8_t *packed_bytes, int byte_len,
                   int bit_length) {
  char *dp = _path(data_dir, "data.grid");
  char *ap = _path(data_dir, "alloc.grid");
  int64_t data_end = _read_data_end(dp);
  int64_t offset = data_end;

  /* Append to data.grid */
  int dfd = open(dp, O_RDWR);
  if (dfd < 0) { free(dp); free(ap); return -1; }
  lseek(dfd, data_end, SEEK_SET);
  _write_all(dfd, packed_bytes, byte_len);
  fsync(dfd); close(dfd);

  /* Update data end */
  _write_data_end(dp, data_end + byte_len);

  /* Update alloc entry */
  GridEntry entry = { .record_id = record_id, .byte_offset = offset,
                       .bit_length = bit_length, .flags = 1 /* ALLOCATED */ };
  _write_alloc_entry(ap, &entry);

  free(dp); free(ap);
  return offset;
}

/* ── grid_read_bytes: returns malloc'd buffer, caller frees ── */
void *grid_read_bytes(const char *data_dir, int record_id,
                      int *out_byte_len, int *out_bit_length, int *out_tombstone) {
  GridRecord *rec = grid_read(data_dir, record_id);
  if (!rec) return NULL;
  *out_byte_len = (rec->bit_length + 7) / 8;
  *out_bit_length = rec->bit_length;
  *out_tombstone = rec->is_tombstone;
  void *buf = rec->parsed;
  rec->parsed = NULL; /* transfer ownership to caller */
  free(rec);
  return buf;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public: grid_read
 * ═══════════════════════════════════════════════════════════════════════════ */

GridRecord *grid_read(const char *data_dir, int record_id) {
  char *ap = _path(data_dir, "alloc.grid");
  char *dp = _path(data_dir, "data.grid");

  GridEntry entry;
  if (_read_alloc_entry(ap, record_id, &entry) < 1 || entry.flags == 0 || entry.flags == 2) {
    free(ap); free(dp);
    GridRecord *r = malloc(sizeof(GridRecord));
    memset(r, 0, sizeof(*r));
    r->record_id = record_id;
    r->is_tombstone = (entry.flags == 2);
    return r;
  }

  int byte_len = (entry.bit_length + 7) / 8;
  uint8_t *buf = malloc(byte_len + 1);
  if (!buf) { free(ap); free(dp); return NULL; }

  int dfd = open(dp, O_RDONLY);
  pread(dfd, buf, byte_len, entry.byte_offset);
  close(dfd);

  GridRecord *rec = malloc(sizeof(GridRecord));
  rec->record_id    = record_id;
  rec->parsed       = buf; /* caller frees this */
  rec->tokens       = NULL;
  rec->token_count  = 0;
  rec->byte_offset  = (int)entry.byte_offset;
  rec->bit_length   = entry.bit_length;
  rec->is_tombstone = 0;

  free(ap); free(dp);
  return rec;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public: grid_delete
 * ═══════════════════════════════════════════════════════════════════════════ */

int grid_delete(const char *data_dir, int record_id) {
  char *ap = _path(data_dir, "alloc.grid");
  GridEntry entry;
  if (_read_alloc_entry(ap, record_id, &entry) < 1 || entry.flags == 0) {
    free(ap); return -1;
  }
  entry.flags = 2; /* TOMBSTONE */
  int r = _write_alloc_entry(ap, &entry);
  free(ap);
  return r;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public: grid_total_entries
 * ═══════════════════════════════════════════════════════════════════════════ */

int grid_total_entries(const char *data_dir) {
  char *ap = _path(data_dir, "alloc.grid");
  int sz = _file_size(ap);
  free(ap);
  if (sz <= 8) return 0;
  return (sz - 8) / ALLOC_ENTRY_SIZE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public: enc_int
 * ═══════════════════════════════════════════════════════════════════════════ */

int enc_int(long long value, uint8_t *tokens, int cap) {
  int n = 0;
  if (value == 0) {
    if (n + 2 > cap) return -1;
    tokens[n++] = 0;   /* T_D0 */
    tokens[n++] = 30;  /* T_END */
    return n;
  }
  int neg = value < 0;
  unsigned long long a = neg ?
    (unsigned long long)(-(value + 1)) + 1ULL :
    (unsigned long long)value;
  char d[32]; int k = 0;
  while (a) { d[k++] = (char)('0' + a % 10); a /= 10; }
  if (n + k + 1 > cap) return -1;
  for (int i = k - 1; i >= 0; i--) {
    int dig = d[i] - '0';
    tokens[n++] = dig == 0 ? 0 : (uint8_t)(neg ? 16 + dig : dig);
  }
  tokens[n++] = 30; /* T_END */
  return n;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public: pack_tokens
 * ═══════════════════════════════════════════════════════════════════════════ */

int pack_tokens(const uint8_t *tokens, int n, uint8_t *out, int *pad_out) {
  uint32_t acc = 0; int nbits = 0, len = 0;
  for (int i = 0; i < n; i++) {
    acc = (acc << 5) | (tokens[i] & 0x1F);
    nbits += 5;
    while (nbits >= 8) { nbits -= 8; out[len++] = (uint8_t)((acc >> nbits) & 0xFF); }
  }
  int pad = 0;
  if (nbits > 0) { out[len++] = (uint8_t)((acc << (8 - nbits)) & 0xFF); pad = 8 - nbits; }
  if (pad_out) *pad_out = pad;
  return len;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public: enc_word
 * ═══════════════════════════════════════════════════════════════════════════ */

static int _word_tok(char c)   { if(c>='A'&&c<='Z')return c-'A'; if(c==' ')return 26; if(c=='.')return 27; return -1; }
static int _spec_tok(char c)   { if(c>='a'&&c<='z')return c-'a'; if(c=='@')return 26; if(c=='-')return 27; return -1; }
static int _spec2_tok(char c)  { static const char *S="!\"#$%&'()*+,/:;<=>?[\\]^_`{|}"; const char *p=strchr(S,c); return p?(int)(p-S):-1; }

int enc_word(const char *text, uint8_t *tokens, int cap) {
  int n = 0;
  if (n >= cap) return -1;
  tokens[n++] = T_START;
  int depth = 0; /* 0=WORD, 1=SPECIAL, 2=SPECIAL2 */
  for (const char *p = text; *p; p++) {
    char ch = *p; int v;
    if (n + 4 > cap) return -1;
    if (isdigit((unsigned char)ch)) {
      for (int k = 0; k < depth; k++) tokens[n++] = T_END;
      depth = 0;
      tokens[n++] = T_END;        /* WORD->NUM */
      tokens[n++] = (uint8_t)(ch - '0'); /* digit */
      tokens[n++] = T_START;      /* NUM->WORD */
      continue;
    }
    if ((v = _word_tok(ch)) >= 0) {
      for (int k = 0; k < depth; k++) tokens[n++] = T_END;
      depth = 0; tokens[n++] = (uint8_t)v; continue;
    }
    if ((v = _spec_tok(ch)) >= 0) {
      if (depth > 1) { tokens[n++] = T_END; depth = 1; }
      else if (depth < 1) { tokens[n++] = T_START; depth = 1; }
      tokens[n++] = (uint8_t)v; continue;
    }
    if ((v = _spec2_tok(ch)) >= 0) {
      if (depth < 2) { if (depth < 1) { tokens[n++] = T_START; depth = 1; } tokens[n++] = T_START; depth = 2; }
      tokens[n++] = (uint8_t)v; continue;
    }
    if (isalpha((unsigned char)ch)) { /* fallback uppercase */
      char u = (char)toupper((unsigned char)ch);
      for (int k = 0; k < depth; k++) tokens[n++] = T_END;
      depth = 0; tokens[n++] = (uint8_t)_word_tok(u); continue;
    }
    return -1;
  }
  for (int k = 0; k < depth; k++) tokens[n++] = T_END;
  if (n >= cap) return -1;
  tokens[n++] = T_END; /* final WORD->NUM */
  return n;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * WAL: wal_append — tick-tagged, hash-chained WAL entry
 * ═══════════════════════════════════════════════════════════════════════════ */

int wal_append(const char *data_dir, int tick_id,
               const uint8_t *prev_hash,
               uint8_t action_type,
               const uint8_t *payload_packed, int payload_len,
               uint8_t *out_hash) {

  /* Build WAL entry: header(48) + payload + SHA-256(32) */
  int total = WAL_ENTRY_HDR + payload_len + SHA256_LEN;
  uint8_t *entry = malloc(total);
  if (!entry) return -1;

  memset(entry, 0, WAL_ENTRY_HDR);
  /* magic(4) */
  entry[0] = (WAL_MAGIC >> 24) & 0xFF;
  entry[1] = (WAL_MAGIC >> 16) & 0xFF;
  entry[2] = (WAL_MAGIC >> 8) & 0xFF;
  entry[3] = WAL_MAGIC & 0xFF;
  /* tick(4) — big-endian */
  entry[4] = (tick_id >> 24) & 0xFF;
  entry[5] = (tick_id >> 16) & 0xFF;
  entry[6] = (tick_id >> 8) & 0xFF;
  entry[7] = tick_id & 0xFF;
  /* prev_hash(32) */
  memcpy(entry + 8, prev_hash, 32);
  /* action_type(1) */
  entry[40] = action_type;
  /* payload_len(4) */
  entry[41] = (payload_len >> 24) & 0xFF;
  entry[42] = (payload_len >> 16) & 0xFF;
  entry[43] = (payload_len >> 8) & 0xFF;
  entry[44] = payload_len & 0xFF;
  /* payload */
  memcpy(entry + WAL_ENTRY_HDR, payload_packed, payload_len);

  /* SHA-256 over header + payload (excluding the hash tail) */
  int body_len = WAL_ENTRY_HDR + payload_len;
  CC_SHA256(entry, body_len, entry + body_len);

  /* Copy output hash */
  if (out_hash) memcpy(out_hash, entry + body_len, SHA256_LEN);

  /* Append to wal.grid */
  char *wp = _path(data_dir, "wal.grid");
  int fd = open(wp, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) { free(wp); free(entry); return -1; }
  if (_write_all(fd, entry, total) != 0) { close(fd); free(wp); free(entry); return -1; }
  fsync(fd); close(fd);

  free(wp); free(entry);
  return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * WAL: wal_replay
 * ═══════════════════════════════════════════════════════════════════════════ */

int wal_replay(const char *data_dir) {
  char *wp = _path(data_dir, "wal.grid");
  if (access(wp, F_OK) != 0) { free(wp); return 0; }

  int sz = _file_size(wp);
  if (sz == 0) { free(wp); return 0; }

  uint8_t *data = malloc(sz);
  int fd = open(wp, O_RDONLY);
  if (fd < 0 || read(fd, data, sz) != sz) { close(fd); free(wp); free(data); return -1; }
  close(fd);

  int highest_tick = 0, off = 0;
  while (off + WAL_ENTRY_HDR + SHA256_LEN <= sz) {
    uint32_t magic = (data[off] << 24) | (data[off+1] << 16) | (data[off+2] << 8) | data[off+3];
    if (magic != WAL_MAGIC) break;

    int tick_id = (data[off+4] << 24) | (data[off+5] << 16) | (data[off+6] << 8) | data[off+7];
    int payload_len = (data[off+41] << 24) | (data[off+42] << 16) | (data[off+43] << 8) | data[off+44];
    int body_len = WAL_ENTRY_HDR + payload_len;

    if (off + body_len + SHA256_LEN > sz) break;

    /* Verify SHA-256 */
    uint8_t computed[SHA256_LEN];
    CC_SHA256(data + off, body_len, computed);
    if (memcmp(computed, data + off + body_len, SHA256_LEN) != 0) {
      /* corruption — stop replay */
      break;
    }

    /* Extract payload and write to grid */
    uint8_t *payload = data + off + WAL_ENTRY_HDR;
    grid_write(data_dir, tick_id, payload, payload_len, payload_len * 8 /* approx bit length */);

    if (tick_id > highest_tick) highest_tick = tick_id;
    off += body_len + SHA256_LEN;
  }

  free(data); free(wp);
  return highest_tick;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * WAL: wal_checkpoint
 * ═══════════════════════════════════════════════════════════════════════════ */

int wal_checkpoint(const char *data_dir, int tick_id) {
  /* Create checkpoints directory */
  char *ck_dir = _path(data_dir, "checkpoints");
  mkdir(ck_dir, 0755);

  /* Copy alloc.grid + data.grid */
  char *ap = _path(data_dir, "alloc.grid");
  char *dp = _path(data_dir, "data.grid");

  char ck_alloc[1024], ck_data[1024];
  snprintf(ck_alloc, sizeof(ck_alloc), "%s/tick_%06d_alloc.grid", ck_dir, tick_id);
  snprintf(ck_data, sizeof(ck_data), "%s/tick_%06d_data.grid", ck_dir, tick_id);

  /* Simple copy via read/write */
  {
    int sz = _file_size(ap);
    uint8_t *buf = malloc(sz);
    int fd = open(ap, O_RDONLY); read(fd, buf, sz); close(fd);
    fd = open(ck_alloc, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    _write_all(fd, buf, sz); fsync(fd); close(fd);
    free(buf);
  }
  {
    int sz = _file_size(dp);
    uint8_t *buf = malloc(sz);
    int fd = open(dp, O_RDONLY); read(fd, buf, sz); close(fd);
    fd = open(ck_data, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    _write_all(fd, buf, sz); fsync(fd); close(fd);
    free(buf);
  }

  /* Truncate WAL */
  char *wp = _path(data_dir, "wal.grid");
  int fd = open(wp, O_WRONLY | O_TRUNC);
  if (fd >= 0) { fsync(fd); close(fd); }

  free(ap); free(dp); free(wp); free(ck_dir);
  return 0;
}
