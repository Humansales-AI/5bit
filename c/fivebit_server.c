/* fivebit_server.c — The 5bit HTTP Server (depends on libc + fivebit_grid.o)
 * ========================================================================
 * One C binary. One port. The database IS the server.
 *
 * Stack:  TCP socket (BSD/POSIX) → HTTP/1.0 parser (hand-rolled)
 *         → 5bit AllocGrid (fivebit_grid.c) — encode, pack, read, write
 *         → HTML/CSS UI (inline string) → response
 *
 * Dependencies: libc only. No Node.js. No Python. No nginx. No framework.
 * The C binary is the entire server. Fivebit_grid.c provides the DB engine.
 *
 * Build: cc -O2 -o fivebit_server fivebit_server.c fivebit_grid.c -lssl -lcrypto
 * Run:   ./fivebit_server 8085 ./data
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ── Grid engine (from fivebit_grid.c, linked at build time) ─────────────── */
#include "fivebit_grid.h"
/* Our token names avoid the T_END/T_START/T_RECORD macros in the header. */

/* ── Token values (our own — no macro conflicts) ──────────────────────────── */
enum { TK_D0=0,TK_D1=1,TK_D2=2,TK_D3=3,TK_D4=4,TK_D5=5,TK_D6=6,TK_D7=7,
       TK_D8=8,TK_D9=9,TK_N1=17,TK_N2=18,TK_N3=19,TK_N4=20,TK_N5=21,
       TK_N6=22,TK_N7=23,TK_N8=24,TK_N9=25,
       TK_RECORD=28, TK_END=30, TK_START=31 };
enum { NEG_BASE = 16 };

/* ── Our own pack/unpack (encoded bytes ↔ token arrays) ────────────────────
 * These are the transport layer — pack_tokens from grid.c does the same thing
 * but we need unpack too. Both are deterministic 5-bit ↔ 8-bit converters. */

static int srv_pack(const uint8_t *tk, int n, uint8_t *out, int *pad) {
  uint64_t acc = 0; int nbits = 0, len = 0;
  for (int i = 0; i < n; i++) {
    acc = (acc << 5) | (tk[i] & 0x1F); nbits += 5;
    while (nbits >= 8) { nbits -= 8; out[len++] = (uint8_t)((acc >> nbits) & 0xFF); }
  }
  if (nbits) { *pad = 8 - nbits; out[len++] = (uint8_t)((acc << *pad) & 0xFF); }
  else *pad = 0;
  return len;
}

static int srv_unpack(const uint8_t *data, int nbytes, int pad, uint8_t *tk, int max_tok) {
  uint64_t acc = 0; int nbits = 0, ntok = 0;
  for (int i = 0; i < nbytes && ntok < max_tok; i++) {
    acc = (acc << 8) | data[i]; nbits += 8;
    while (nbits >= 5 && ntok < max_tok) { nbits -= 5; tk[ntok++] = (acc >> nbits) & 0x1F; }
  }
  (void)pad;
  return ntok;
}

/* ── Task record: WORD(text) INT(done) INT(created_ts) RECORD ───────────── */

static int encode_task(const char *task, int done, long long created,
                       uint8_t *out, int *pad_out) {
  uint8_t tk[1024]; int n = 0;
  int w = enc_word(task, tk + n, 1024 - n);
  if (w < 0) return -1;  /* unsupported character in task text */
  n += w;
  n += enc_int(done ? 1 : 0, tk + n, 1024 - n);
  n += enc_int(created, tk + n, 1024 - n);
  tk[n++] = TK_RECORD;
  return srv_pack(tk, n, out, pad_out);
}

static int decode_task(const uint8_t *data, int nbytes, int pad,
                       char **task_buf, int *done, long long *created) {
  uint8_t tk[4096]; int ntok = srv_unpack(data, nbytes, pad, tk, 4096);

  static const char *CTX[4] = {
    "0123456789??????789??????",                          /* depth 0: NUM     */
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ .",                       /* depth 1: WORD    */
    "abcdefghijklmnopqrstuvwxyz@-",                       /* depth 2: SPECIAL */
    "!\"#$%&'()*+,/:;<=>?[\\]^_`{|}",                     /* depth 3: SPECIAL2 */
  };

  /* Build text stream with all chars interleaved. Mark positions where
   * depth-0 END fires (each marks the end of a digit group). */
  char text[4096]; int tpos = 0;
  int depth = 0;
  int num_ends[256]; int ne = 0;  /* text positions after each depth-0 END */
  int prev_tpos = -1;

  for (int i = 0; i < ntok; i++) {
    int t = tk[i];
    if (t == TK_START) { depth++; continue; }
    if (t == TK_END) {
      if (depth == 0) {
        if (tpos > prev_tpos) { num_ends[ne++] = tpos; prev_tpos = tpos; }
      } else {
        depth--;
        /* WORD→NUM transition: depth just became 0 — record word boundary */
        if (depth == 0 && tpos > prev_tpos) { num_ends[ne++] = tpos; prev_tpos = tpos; }
      }
      continue;
    }
    if (t == TK_RECORD) break;
    int d = depth > 3 ? 3 : depth;
    if (depth == 0 && t >= 17 && t <= 25) text[tpos++] = '-'; /* negative sign */
    if (t >= 0 && t <= 27 && CTX[d] && CTX[d][t])
      text[tpos++] = CTX[d][t];
    if (tpos >= 4095) tpos = 4094;
  }
  text[tpos] = '\0';

  /* The encode order is: enc_word(text) + enc_int(done) + enc_int(created) + RECORD.
   * enc_word always ends with WORD→NUM END (plus possibly extra trailing ENDs).
   * enc_int(done) ends with END. enc_int(created) ends with END.
   * So num_ends[] = [text digit groups..., word_end, done_end, created_end].
   * The last two entries are always the metadata boundaries.
   * Truncate at num_ends[ne-3] (the word_end) to strip metadata digits. */
  if (ne >= 3) {
    /* num_ends[ne-3] = word boundary, [ne-2] = after done, [ne-1] = after created */
    *done    = (text[num_ends[ne-3]] == '1');   /* single digit at word boundary */
    *created = atoll(text + num_ends[ne-2]);     /* rest is created timestamp */
    text[num_ends[ne-3]] = '\0';                 /* cut before done digit */
  } else if (ne >= 2) {
    *done    = (text[num_ends[ne-2]] == '1');
    *created = atoll(text + num_ends[ne-1]);
    text[num_ends[ne-2]] = '\0';
  } else if (ne >= 1) {
    *created = atoll(text);
    *done = 0;
    text[0] = '\0';
  } else {
    *done = 0; *created = 0;
  }

  *task_buf = strdup(text);
  return ntok;
}

/* ── JSON builders (hand-rolled) ─────────────────────────────────────────── */

static void json_str(char *b, int *pos, const char *s) {
  (*pos) += snprintf(b + *pos, 65536 - *pos, "\"");
  for (const char *p = s; *p; p++) {
    if (*p == '"' || *p == '\\') b[(*pos)++] = '\\';
    b[(*pos)++] = *p;
  }
  (*pos) += snprintf(b + *pos, 65536 - *pos, "\"");
}

/* ── HTTP response helpers ───────────────────────────────────────────────── */

static void http_ok(int fd, const char *mime, const char *body, int blen) {
  char h[512]; int hl = snprintf(h, 512,
    "HTTP/1.0 200 OK\r\nContent-Type: %s\r\n"
    "Access-Control-Allow-Origin: *\r\nContent-Length: %d\r\n"
    "Connection: close\r\n\r\n", mime, blen);
  write(fd, h, hl); if (body && blen > 0) write(fd, body, blen);
}
static void http_created(int fd, const char *body, int blen) {
  char h[512]; int hl = snprintf(h, 512,
    "HTTP/1.0 201 Created\r\nContent-Type: application/json\r\n"
    "Access-Control-Allow-Origin: *\r\nContent-Length: %d\r\n"
    "Connection: close\r\n\r\n", blen);
  write(fd, h, hl); if (body && blen > 0) write(fd, body, blen);
}
static void http_404(int fd) {
  const char *r = "HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  write(fd, r, (int)strlen(r));
}
static void http_400(int fd, const char *msg) {
  char r[512]; int l = snprintf(r, 512,
    "HTTP/1.0 400 Bad Request\r\nContent-Type: text/plain\r\n"
    "Content-Length: %d\r\nConnection: close\r\n\r\n%s", (int)strlen(msg), msg);
  write(fd, r, l);
}

/* ── HTML UI ─────────────────────────────────────────────────────────────── */
static const char *HTML =
"<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"UTF-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1.0\">\n"
"<title>5bit — C Server (Zero Dependencies)</title>\n<style>\n"
"*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}\n"
"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',system-ui,sans-serif;background:#0a0a0f;color:#e0e0e0;min-height:100vh}\n"
".header{background:linear-gradient(135deg,#1a1a2e 0%,#16213e 50%,#0f3460 100%);border-bottom:2px solid #e94560;padding:24px 32px}\n"
".header h1{font-size:28px;font-weight:700;letter-spacing:-0.5px}\n"
".header h1 span{color:#e94560}\n"
".header p{color:#8892b0;margin-top:4px;font-size:14px}\n"
".badge{display:inline-block;background:#e9456020;color:#e94560;border:1px solid #e9456040;border-radius:4px;padding:2px 8px;font-size:11px;margin-left:8px;font-family:monospace}\n"
".layout{display:flex;min-height:calc(100vh - 110px)}\n"
".main{flex:1;padding:32px;max-width:720px}\n"
".panel{width:380px;background:#111118;border-left:1px solid #1e1e2e;padding:24px;overflow-y:auto;max-height:calc(100vh - 110px)}\n"
".panel h3{font-size:13px;text-transform:uppercase;letter-spacing:1px;color:#e94560;margin-bottom:12px}\n"
".add-form{display:flex;gap:8px;margin-bottom:24px}\n"
".add-form input{flex:1;padding:10px 14px;background:#16161f;border:1px solid #2a2a3e;border-radius:6px;color:#e0e0e0;font-size:14px;outline:none}\n"
".add-form input:focus{border-color:#e94560}\n"
".add-form button{padding:10px 20px;background:#e94560;color:#fff;border:none;border-radius:6px;font-size:14px;font-weight:600;cursor:pointer}\n"
".add-form button:hover{background:#c73652}\n"
".task-list{list-style:none}\n"
".task-item{display:flex;align-items:center;gap:12px;padding:12px 16px;background:#16161f;border:1px solid #1e1e2e;border-radius:6px;margin-bottom:8px}\n"
".task-item:hover{border-color:#2a2a3e}\n"
".task-item.done .task-text{text-decoration:line-through;color:#555}\n"
".task-text{flex:1;font-size:14px}\n"
".task-date{font-size:11px;color:#555;font-family:monospace}\n"
".task-del{background:none;border:1px solid #3a1a1a;color:#e94560;padding:4px 10px;border-radius:4px;cursor:pointer;font-size:12px}\n"
".task-del:hover{background:#3a1a1a}\n"
".task-toggle{width:18px;height:18px;border:2px solid #3a3a5a;border-radius:4px;cursor:pointer;display:inline-flex;align-items:center;justify-content:center;font-size:11px;flex-shrink:0}\n"
".task-item.done .task-toggle{background:#e94560;border-color:#e94560}\n"
".stat{display:flex;justify-content:space-between;padding:6px 0;font-size:12px}\n"
".stat-label{color:#555}\n"
".stat-value{color:#e94560;font-family:monospace;font-weight:600;font-size:11px}\n"
".empty{text-align:center;color:#444;padding:40px 0;font-size:14px}\n"
".token-box{background:#0d0d16;border:1px solid #1e1e2e;border-radius:6px;padding:12px;font-family:monospace;font-size:10px;color:#e94560;word-break:break-all;line-height:1.6;max-height:200px;overflow-y:auto;margin-bottom:12px}\n"
"</style>\n</head>\n<body>\n"
"<div class=\"header\">\n"
"  <h1>5bit <span>◆</span> Task Manager<span class=\"badge\">C BINARY — ZERO DEPS</span></h1>\n"
"  <p>Server: <b>C binary</b> (BSD sockets + hand-rolled HTTP) &nbsp;|&nbsp; DB: <b>5bit AllocGrid</b> &nbsp;|&nbsp; Dependencies: <b>libc only</b></p>\n"
"</div>\n"
"<div class=\"layout\">\n"
"  <div class=\"main\">\n"
"    <div class=\"add-form\">\n"
"      <input id=\"taskInput\" placeholder=\"Add a task...\" autofocus />\n"
"      <button onclick=\"addTask()\">Add</button>\n"
"    </div>\n"
"    <ul id=\"taskList\" class=\"task-list\"></ul>\n"
"    <div id=\"empty\" class=\"empty\">No tasks yet. Add one above.</div>\n"
"  </div>\n"
"  <div class=\"panel\">\n"
"    <h3>◆ The Stack</h3>\n"
"    <div class=\"stat\"><span class=\"stat-label\">Server binary</span><span class=\"stat-value\">fivebit_server (C, ~50KB)</span></div>\n"
"    <div class=\"stat\"><span class=\"stat-label\">HTTP parser</span><span class=\"stat-value\">hand-rolled, ~60 lines</span></div>\n"
"    <div class=\"stat\"><span class=\"stat-label\">Database engine</span><span class=\"stat-value\">5bit AllocGrid (C)</span></div>\n"
"    <div class=\"stat\"><span class=\"stat-label\">Encode/decode</span><span class=\"stat-value\">libfivebit (C shared lib)</span></div>\n"
"    <div class=\"stat\"><span class=\"stat-label\">Token lexicon</span><span class=\"stat-value\">32 x 5-bit</span></div>\n"
"    <div class=\"stat\"><span class=\"stat-label\">Runtime needed</span><span class=\"stat-value\">NONE</span></div>\n"
"    <div class=\"stat\"><span class=\"stat-label\">Node.js / Python</span><span class=\"stat-value\">NOT INSTALLED</span></div>\n"
"    <div class=\"stat\"><span class=\"stat-label\">Express / npm / pip</span><span class=\"stat-value\">NOTHING</span></div>\n"
"    <h3 style=\"margin-top:20px\">◆ Last Token Stream</h3>\n"
"    <div id=\"tokenBox\" class=\"token-box\">—</div>\n"
"  </div>\n"
"</div>\n"
"<script>\n"
"async function loadTasks(){\n"
"  const r=await fetch('/api/tasks');const d=await r.json();\n"
"  const list=document.getElementById('taskList');\n"
"  const empty=document.getElementById('empty');\n"
"  list.innerHTML='';\n"
"  empty.style.display=d.tasks.length?'none':'block';\n"
"  d.tasks.forEach(t=>{\n"
"    const li=document.createElement('li');\n"
"    li.className='task-item'+(t.done?' done':'');\n"
"    li.innerHTML=\n"
"      '<div class=\"task-toggle\" onclick=\"toggleTask('+t._id+','+t.done+')\">'+(t.done?'\\u2713':'')+'</div>'+\n"
"      '<span class=\"task-text\">'+esc(t.task)+'</span>'+\n"
"      '<span class=\"task-date\">'+new Date(t.created*1000).toLocaleDateString()+'</span>'+\n"
"      '<button class=\"task-del\" onclick=\"deleteTask('+t._id+')\">\\u00d7</button>';\n"
"    list.appendChild(li);\n"
"  });\n"
"  document.getElementById('tokenBox').textContent=d.lastTokens||'—';\n"
"}\n"
"function esc(s){return(s||'').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/\"/g,'&quot;');}\n"
"async function addTask(){\n"
"  const inp=document.getElementById('taskInput');\n"
"  const text=inp.value.trim();if(!text)return;\n"
"  await fetch('/api/tasks',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({task:text})});\n"
"  inp.value='';loadTasks();\n"
"}\n"
"async function toggleTask(id,cur){\n"
"  await fetch('/api/tasks/'+id,{method:'PATCH',headers:{'Content-Type':'application/json'},body:JSON.stringify({done:!cur})});\n"
"  loadTasks();\n"
"}\n"
"async function deleteTask(id){\n"
"  await fetch('/api/tasks/'+id,{method:'DELETE'});loadTasks();\n"
"}\n"
"document.getElementById('taskInput').addEventListener('keydown',e=>{if(e.key==='Enter')addTask();});\n"
"loadTasks();\n"
"</script>\n</body>\n</html>";

/* ── Request handler ─────────────────────────────────────────────────────── */

static void handle(int fd, const char *data_dir) {
  char buf[8192];
  int n = (int)read(fd, buf, sizeof(buf) - 1);
  if (n <= 0) { close(fd); return; }
  buf[n] = '\0';

  char method[16] = {0}, path[256] = {0};
  sscanf(buf, "%15s %255s", method, path);

  /* Decode %xx in path */
  char dec[256]; int dp = 0;
  for (int i = 0; path[i] && dp < 255; i++) {
    if (path[i] == '%' && path[i+1] && path[i+2]) {
      int hi, lo; sscanf(path+i+1, "%1x%1x", &hi, &lo);
      dec[dp++] = (char)((hi << 4) | lo); i += 2;
    } else dec[dp++] = path[i];
  }
  dec[dp] = '\0';

  char *body = strstr(buf, "\r\n\r\n");
  if (body) body += 4;

  /* ── GET / ──────────────────────────────────────────────────────────── */
  if (strcmp(method, "GET") == 0 && strcmp(dec, "/") == 0) {
    http_ok(fd, "text/html; charset=utf-8", HTML, (int)strlen(HTML));
    close(fd); return;
  }

  /* ── GET /api/tasks ─────────────────────────────────────────────────── */
  if (strcmp(method, "GET") == 0 && strcmp(dec, "/api/tasks") == 0) {
    char json[65536]; int pos = 0;
    pos += snprintf(json + pos, sizeof(json) - pos, "{\"tasks\":[");
    int total = grid_total_entries(data_dir);
    int first = 1;
    uint8_t last_tokens[4096]; int last_ntok = 0;

    for (int i = 0; i < total; i++) {
      GridRecord *rec = grid_read(data_dir, i);
      if (!rec || rec->is_tombstone) {
        if (rec) { free(rec->parsed); free(rec); } continue;
      }
      /* Unpack record bytes to token array for display */
      int nbytes = (rec->bit_length + 7) / 8;
      uint8_t tk[4096];
      int ntok = srv_unpack(rec->parsed, nbytes, 0, tk, 4096);
      if (ntok > 0) { memcpy(last_tokens, tk, ntok); last_ntok = ntok; }

      char *task_text = NULL; int done; long long created;
      decode_task(rec->parsed, nbytes, 0, &task_text, &done, &created);
      if (task_text && strlen(task_text) > 0) {
        if (!first) json[pos++] = ',';
        pos += snprintf(json + pos, sizeof(json) - pos,
          "{\"_id\":%d,\"task\":\"%s\",\"done\":%s,\"created\":%lld}",
          rec->record_id, task_text, done ? "true" : "false", created);
        first = 0;
      }
      free(task_text);
      free(rec->parsed); free(rec);
    }
    pos += snprintf(json + pos, sizeof(json) - pos, "],\"lastTokens\":\"");

    /* Token names for display */
    for (int i = 0; i < last_ntok && i < 200; i++) {
      int t = last_tokens[i];
      if (t == TK_START) pos += snprintf(json + pos, sizeof(json) - pos, "START ");
      else if (t == TK_END) pos += snprintf(json + pos, sizeof(json) - pos, "END ");
      else if (t == TK_RECORD) pos += snprintf(json + pos, sizeof(json) - pos, "RECORD ");
      else pos += snprintf(json + pos, sizeof(json) - pos, "D%d ", t >= 17 ? -(t-16) : t);
    }
    if (pos > 1 && json[pos-1] == ' ') pos--;
    pos += snprintf(json + pos, sizeof(json) - pos, "\"}");
    http_ok(fd, "application/json", json, pos);
    close(fd); return;
  }

  /* ── POST /api/tasks ────────────────────────────────────────────────── */
  if (strcmp(method, "POST") == 0 && strcmp(dec, "/api/tasks") == 0) {
    if (!body) { http_400(fd, "missing body"); close(fd); return; }
    char *start = strstr(body, "\"task\"");
    if (!start) { http_400(fd, "missing task field"); close(fd); return; }
    start = strchr(start, ':'); if (!start) { http_400(fd, "malformed"); close(fd); return; }
    start++; while (*start == ' ' || *start == '"') start++;
    char *end = strchr(start, '"');
    if (!end) { http_400(fd, "unterminated"); close(fd); return; }
    int tlen = (int)(end - start); if (tlen >= 4095) tlen = 4094;
    char task_text[4096]; memcpy(task_text, start, tlen); task_text[tlen] = '\0';

    /* Find a free slot */
    int total = grid_total_entries(data_dir), rid = total;
    for (int i = 0; i < total; i++) {
      GridRecord *r = grid_read(data_dir, i);
      if (!r || r->is_tombstone) { rid = i; if (r) { free(r->parsed); free(r); } break; }
      if (r) { free(r->parsed); free(r); }
    }

    long long now_ts = (long long)time(NULL);
    uint8_t packed[8192]; int pad;
    int plen = encode_task(task_text, 0, now_ts, packed, &pad);
    if (plen < 0) {
      http_400(fd, "task contains unsupported characters");
      close(fd); return;
    }
    int bit_len = plen * 8 - pad;
    grid_write(data_dir, rid, packed, plen, bit_len);

    char json[8192]; int pos = 0;
    pos += snprintf(json + pos, sizeof(json) - pos,
      "{\"_id\":%d,\"task\":\"%s\",\"done\":false}", rid, task_text);
    http_created(fd, json, pos);
    close(fd); return;
  }

  /* ── PATCH /api/tasks/:id ───────────────────────────────────────────── */
  if (strcmp(method, "PATCH") == 0 && strncmp(dec, "/api/tasks/", 11) == 0) {
    int rid = atoi(dec + 11);
    if (!body) { http_400(fd, "missing body"); close(fd); return; }
    int new_done = strstr(body, "true") ? 1 : 0;
    GridRecord *rec = grid_read(data_dir, rid);
    if (!rec || rec->is_tombstone) {
      http_404(fd); if (rec) { free(rec->parsed); free(rec); } close(fd); return;
    }
    int nbytes = (rec->bit_length + 7) / 8;
    char *task_text = NULL; int old_done; long long created;
    decode_task(rec->parsed, nbytes, 0, &task_text, &old_done, &created);

    uint8_t packed[8192]; int pad;
    int plen = encode_task(task_text ? task_text : "", new_done, created, packed, &pad);
    grid_write(data_dir, rid, packed, plen, plen * 8 - pad);

    char json[8192]; int pos = 0;
    pos += snprintf(json + pos, sizeof(json) - pos,
      "{\"_id\":%d,\"task\":\"%s\",\"done\":%s}",
      rid, task_text ? task_text : "", new_done ? "true" : "false");
    http_ok(fd, "application/json", json, pos);
    free(task_text); free(rec->parsed); free(rec);
    close(fd); return;
  }

  /* ── DELETE /api/tasks/:id ──────────────────────────────────────────── */
  if (strcmp(method, "DELETE") == 0 && strncmp(dec, "/api/tasks/", 11) == 0) {
    int rid = atoi(dec + 11);
    grid_delete(data_dir, rid);
    char json[64]; int pos = snprintf(json, 64, "{\"deleted\":%d}", rid);
    http_ok(fd, "application/json", json, pos);
    close(fd); return;
  }

  http_404(fd); close(fd);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
  int port = argc > 1 ? atoi(argv[1]) : 8085;
  const char *data_dir = argc > 2 ? argv[2] : "./data_fivebit_c";

  mkdir(data_dir, 0755);
  grid_init(data_dir);

  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) { perror("socket"); return 1; }
  int opt = 1; setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons((uint16_t)port);
  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind"); close(sock); return 1;
  }
  if (listen(sock, 10) < 0) { perror("listen"); close(sock); return 1; }

  printf("┌─────────────────────────────────────────────┐\n");
  printf("│  5bit Server — C Binary, Zero Dependencies   │\n");
  printf("│  HTTP:  BSD sockets (hand-rolled parser)     │\n");
  printf("│  DB:    5bit AllocGrid (fivebit_grid.c)       │\n");
  printf("│  Codec: libfivebit (encode/decode/pack)       │\n");
  printf("│  Stack: libc + fivebit_grid = entire dep list │\n");
  printf("│  No Node.js. No Python. No nginx. No runtime. │\n");
  printf("└─────────────────────────────────────────────┘\n");
  printf("  →  http://localhost:%d\n", port);
  printf("  Binary links against: libc, libssl, libcrypto\n\n");

  while (1) {
    struct sockaddr_in client; socklen_t clen = sizeof(client);
    int cfd = accept(sock, (struct sockaddr *)&client, &clen);
    if (cfd < 0) { perror("accept"); continue; }
    handle(cfd, data_dir);
  }
  close(sock);
  return 0;
}
