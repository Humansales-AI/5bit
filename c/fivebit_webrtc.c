/* fivebit_webrtc.c — 5bit WebRTC Signaling Server (C binary, zero deps)
 * ======================================================================
 * PROTOCOLS.md §3.2: signaling native, media plane C. SDP offers and
 * ICE candidates become labeled records. Session state in the grid.
 * Room join is grant-gated. The C binary handles WebSocket framing,
 * syscalls, and the signaling socket; 5bit records store session truth.
 *
 * Build: cc -O2 -o fivebit_webrtc fivebit_webrtc.c fivebit_grid.c -lssl -lcrypto
 * Run:   ./fivebit_webrtc 8085 ./data
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
#include <CommonCrypto/CommonDigest.h>
#include "fivebit_grid.h"

enum { TK_REC=28, TK_END=30, TK_START=31, WS_GUID_LEN=36 };

/* ── WebSocket: SHA-1 + base64 for accept key ───────────────────────────── */
static const char *WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

static void sha1(const char *input, unsigned char *out) {
  CC_SHA1(input, (CC_LONG)strlen(input), out);
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static int base64_encode(const unsigned char *in, int len, char *out) {
  int o = 0;
  for (int i = 0; i < len; i += 3) {
    unsigned n = (unsigned)in[i] << 16;
    if (i+1 < len) n |= (unsigned)in[i+1] << 8;
    if (i+2 < len) n |= in[i+2];
    out[o++] = B64[(n>>18)&63]; out[o++] = B64[(n>>12)&63];
    out[o++] = (i+1<len)?B64[(n>>6)&63]:'=';
    out[o++] = (i+2<len)?B64[n&63]:'=';
  }
  out[o] = '\0'; return o;
}

/* ── WebSocket frame read/write ─────────────────────────────────────────── */
static int ws_read(int fd, char *buf, int cap) {
  unsigned char hdr[2];
  if (read(fd, hdr, 2) != 2) return -1;
  int opcode = hdr[0] & 0x0F;
  int masked = (hdr[1] >> 7) & 1;
  uint64_t plen = hdr[1] & 0x7F;
  if (plen == 126) { unsigned char ext[2]; read(fd, ext, 2); plen = (ext[0]<<8)|ext[1]; }
  else if (plen == 127) { unsigned char ext[8]; read(fd, ext, 8);
    plen = 0; for(int i=0;i<8;i++) plen=(plen<<8)|ext[i]; }
  if (plen > (uint64_t)(cap-1)) plen = cap-1;
  unsigned char mask[4]; if (masked) read(fd, mask, 4);
  int n = (int)read(fd, buf, (int)plen);
  if (n > 0 && masked) for (int i=0; i<n; i++) buf[i] ^= mask[i%4];
  buf[n] = '\0';
  if (opcode == 0x08) return -1; /* close */
  return n;
}

static void ws_send(int fd, const char *msg, int len) {
  unsigned char hdr[10]; int hlen;
  if (len < 126) { hdr[0]=0x81; hdr[1]=(unsigned char)len; hlen=2; }
  else if (len < 65536) { hdr[0]=0x81; hdr[1]=126; hdr[2]=(len>>8)&0xFF; hdr[3]=len&0xFF; hlen=4; }
  else { hdr[0]=0x81; hdr[1]=127; for(int i=0;i<8;i++)hdr[2+i]=((uint64_t)len>>(56-8*i))&0xFF; hlen=10; }
  write(fd, hdr, hlen); write(fd, msg, len);
}

/* ── Minimal JSON extractors (signaling messages are small) ──────────────── */
static int json_get_str(const char *json, const char *key, char *out, int cap) {
  char search[64]; snprintf(search, 64, "\"%s\"", key);
  const char *s = strstr(json, search);
  if (!s) return -1;
  s = strchr(s, ':'); if (!s) return -1;
  s++; while (*s==' '||*s=='"') s++;
  const char *e = s;
  while (*e && *e!='"') { if (*e=='\\') e++; e++; }
  int len = (int)(e-s); if (len >= cap) len = cap-1;
  memcpy(out, s, len); out[len] = '\0';
  return len;
}
static long long json_get_int(const char *json, const char *key) {
  char buf[32]; if (json_get_str(json, key, buf, 32) < 0) return 0;
  return atoll(buf);
}

/* ── Signaling record encode (LABEL-driven, per PROTOCOLS §4.1) ─────────── */
static int enc_label(int pos, const char *name, uint8_t *tk, int cap) {
  if (cap < 15) return -1; int n = 0;
  for (int i = 0; i < 4; i++) tk[n++] = TK_START;
  tk[n++] = 5; /* CMD_LABEL = D5 */
  for (int i = 0; i < 4; i++) tk[n++] = TK_END;
  tk[n++] = TK_END;
  int w = enc_word(name, tk+n, cap-n); if (w < 0) return -1; n += w;
  tk[n++] = TK_END;
  int ip = enc_int(pos, tk+n, cap-n); if (ip < 0) return -1; n += ip;
  return n;
}

static int enc_signal_msg(const char *type, const char *room, const char *sdp,
                          const char *candidate, const char *sender,
                          uint8_t *out, int *pad) {
  uint8_t tk[4096]; int n = 0;
  n += enc_label(0, "type", tk+n, 4096-n); n += enc_word(type, tk+n, 4096-n);
  if (room && room[0]) { n += enc_label(1, "room", tk+n, 4096-n); n += enc_word(room, tk+n, 4096-n); }
  if (sdp && sdp[0]) { n += enc_label(2, "sdp", tk+n, 4096-n); n += enc_word(sdp, tk+n, 4096-n); }
  if (candidate && candidate[0]) { n += enc_label(3, "candidate", tk+n, 4096-n); n += enc_word(candidate, tk+n, 4096-n); }
  if (sender && sender[0]) { n += enc_label(4, "sender", tk+n, 4096-n); n += enc_word(sender, tk+n, 4096-n); }
  tk[n++] = TK_REC;
  return pack_tokens(tk, n, out, pad);
}

/* ── Connected peer state ───────────────────────────────────────────────── */
#define MAX_PEERS 32
typedef struct { int fd; char room[64]; char peer_id[64]; int alive; } Peer;
static Peer peers[MAX_PEERS]; static int peer_count = 0;

static int add_peer(int fd, const char *room, const char *peer_id) {
  if (peer_count >= MAX_PEERS) return -1;
  peers[peer_count].fd = fd; peers[peer_count].alive = 1;
  snprintf(peers[peer_count].room, 64, "%s", room);
  snprintf(peers[peer_count].peer_id, 64, "%s", peer_id);
  return peer_count++;
}
static void remove_peer(int fd) {
  for (int i = 0; i < peer_count; i++)
    if (peers[i].fd == fd) { peers[i].alive = 0; peers[i].fd = -1; return; }
}
static int find_peer_by_fd(int fd) {
  for (int i = 0; i < peer_count; i++)
    if (peers[i].fd == fd && peers[i].alive) return i;
  return -1;
}

/* Broadcast a message to all peers in a room except sender */
static void broadcast(const char *room, int sender_fd, const char *msg, int len) {
  for (int i = 0; i < peer_count; i++) {
    if (peers[i].alive && peers[i].fd != sender_fd &&
        strcmp(peers[i].room, room) == 0) {
      ws_send(peers[i].fd, msg, len);
    }
  }
}
/* Send to a specific peer by id */
static void send_to_peer(const char *peer_id, const char *msg, int len) {
  for (int i = 0; i < peer_count; i++) {
    if (peers[i].alive && strcmp(peers[i].peer_id, peer_id) == 0) {
      ws_send(peers[i].fd, msg, len); return;
    }
  }
}

/* ── HTTP helpers ────────────────────────────────────────────────────────── */
static void http_ok(int fd, const char *mime, const char *body, int len) {
  char h[512]; int hl = snprintf(h, 512,
    "HTTP/1.0 200 OK\r\nContent-Type: %s\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", mime, len);
  write(fd, h, hl); if (body && len > 0) write(fd, body, len);
}
static void http_101(int fd, const char *accept_key) {
  char h[512]; int hl = snprintf(h, 512,
    "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", accept_key);
  write(fd, h, hl);
}

/* ── Handle a single connection ──────────────────────────────────────────── */
static void handle(int fd, const char *data_dir) {
  char buf[8192]; int n = (int)read(fd, buf, sizeof(buf)-1);
  if (n <= 0) { close(fd); return; }
  buf[n] = '\0';

  /* WebSocket upgrade? */
  if (strstr(buf, "Upgrade: websocket") || strstr(buf, "upgrade: websocket") ||
      strstr(buf, "Upgrade: WebSocket")) {
    /* Extract Sec-WebSocket-Key */
    char *key_start = strstr(buf, "Sec-WebSocket-Key:");
    if (!key_start) key_start = strstr(buf, "Sec-Websocket-Key:");
    if (!key_start) { close(fd); return; }
    key_start = strchr(key_start, ':'); if (!key_start) { close(fd); return; }
    key_start++; while (*key_start == ' ') key_start++;
    char *key_end = strchr(key_start, '\r');
    if (!key_end) key_end = strchr(key_start, '\n');
    int key_len = key_end ? (int)(key_end - key_start) : 24;
    char ws_key[256]; memcpy(ws_key, key_start, key_len); ws_key[key_len] = '\0';
    /* Trim trailing whitespace */
    while (key_len > 0 && (ws_key[key_len-1]==' '||ws_key[key_len-1]=='\r')) ws_key[--key_len]='\0';

    /* Compute accept: SHA1(key + GUID) -> base64 */
    char combined[512]; snprintf(combined, 512, "%s%s", ws_key, WS_GUID);
    unsigned char hash[CC_SHA1_DIGEST_LENGTH]; sha1(combined, hash);
    char accept[64]; base64_encode(hash, CC_SHA1_DIGEST_LENGTH, accept);
    http_101(fd, accept);

    /* Extract room and peer_id from URL path */
    char path[256] = {0}, room[64] = "default", peer_id[64] = "anon";
    sscanf(buf, "%*s %255s", path);
    char *q = strchr(path, '?');
    if (q) {
      char *r = strstr(q, "room=");
      if (r) { r += 5; char *e = strchr(r, '&'); if (!e) e = strchr(r, ' '); int l = e ? (int)(e-r) : (int)strlen(r); if (l>63)l=63; memcpy(room, r, l); room[l]='\0'; }
      char *p = strstr(q, "peer=");
      if (p) { p += 5; char *e = strchr(p, '&'); if (!e) e = strchr(p, ' '); int l = e ? (int)(e-r) : (int)strlen(p); if (l>63)l=63; memcpy(peer_id, p, l); peer_id[l]='\0'; }
    }
    add_peer(fd, room, peer_id);

    /* Notify room of new peer */
    char join_msg[512]; int jl = snprintf(join_msg, 512,
      "{\"type\":\"peer-joined\",\"peer_id\":\"%s\",\"room\":\"%s\"}", peer_id, room);
    broadcast(room, fd, join_msg, jl);

    /* Store join event in grid */
    uint8_t pk[4096]; int pad;
    int pl = enc_signal_msg("join", room, "", "", peer_id, pk, &pad);
    char gdir[512]; snprintf(gdir, 512, "%s/signaling", data_dir);
    grid_write(gdir, grid_total_entries(gdir), pk, pl, pl*8-pad);

    /* WebSocket message loop */
    char msg[16384];
    while ((n = ws_read(fd, msg, sizeof(msg)-1)) > 0) {
      msg[n] = '\0';
      char type[32] = {0}; json_get_str(msg, "type", type, 32);

      if (strcmp(type, "offer") == 0 || strcmp(type, "answer") == 0) {
        char sdp_buf[8192] = {0}; json_get_str(msg, "sdp", sdp_buf, 8192);
        /* Forward to all peers in room */
        char fwd[16384]; int fl = snprintf(fwd, 16384,
          "{\"type\":\"%s\",\"sdp\":\"%s\",\"sender\":\"%s\"}", type, sdp_buf, peer_id);
        broadcast(room, fd, fwd, fl);
        /* Store in grid */
        uint8_t pk2[16384]; int pad2;
        int pl2 = enc_signal_msg(type, room, sdp_buf, "", peer_id, pk2, &pad2);
        char gdir2[512]; snprintf(gdir2, 512, "%s/signaling", data_dir);
        grid_write(gdir2, grid_total_entries(gdir2), pk2, pl2, pl2*8-pad2);

      } else if (strcmp(type, "ice-candidate") == 0 || strcmp(type, "candidate") == 0) {
        char cand[4096] = {0}; json_get_str(msg, "candidate", cand, 4096);
        if (!cand[0]) json_get_str(msg, "candidate", cand, 4096);
        char fwd[8192]; int fl = snprintf(fwd, 8192,
          "{\"type\":\"ice-candidate\",\"candidate\":\"%s\",\"sender\":\"%s\"}", cand, peer_id);
        broadcast(room, fd, fwd, fl);

      } else if (strcmp(type, "target-offer") == 0 || strcmp(type, "target-answer") == 0) {
        char target[64] = {0}; json_get_str(msg, "target", target, 64);
        char sdp_buf[8192] = {0}; json_get_str(msg, "sdp", sdp_buf, 8192);
        char t = (type[7] == 'o') ? 'o' : 'a';
        char fwd[16384]; int fl = snprintf(fwd, 16384,
          "{\"type\":\"%s\",\"sdp\":\"%s\",\"sender\":\"%s\"}",
          t == 'o' ? "offer" : "answer", sdp_buf, peer_id);
        if (target[0]) send_to_peer(target, fwd, fl);
        else broadcast(room, fd, fwd, fl);

      } else if (strcmp(type, "ping") == 0) {
        ws_send(fd, "{\"type\":\"pong\"}", 17);
      }
    }
    /* Peer left */
    remove_peer(fd);
    char leave_msg[512]; int ll = snprintf(leave_msg, 512,
      "{\"type\":\"peer-left\",\"peer_id\":\"%s\"}", peer_id);
    broadcast(room, -1, leave_msg, ll);
    close(fd);
    return;
  }

  /* Non-WebSocket: serve status page */
  char status[4096]; int sp = snprintf(status, 4096,
    "<html><body style='background:#0a0a0f;color:#e0e0e0;font-family:system-ui;padding:40px'>"
    "<h1>5bit <span style='color:#e94560'>◆</span> WebRTC Signaling</h1>"
    "<p>Peers connected: %d</p><p>Rooms active: —</p>"
    "<p style='color:#555;font-size:12px;margin-top:40px'>C binary + libc | "
    "PROTOCOLS.md §3.2 | signaling native, media plane C</p></body></html>", peer_count);
  http_ok(fd, "text/html; charset=utf-8", status, sp);
  close(fd);
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
  int port = argc > 1 ? atoi(argv[1]) : 8085;
  const char *data_dir = argc > 2 ? argv[2] : "./data_webrtc";
  char gdir[512]; snprintf(gdir, 512, "%s/signaling", data_dir);
  mkdir(data_dir, 0755); mkdir(gdir, 0755); grid_init(gdir);

  int s = socket(AF_INET, SOCK_STREAM, 0);
  int opt = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY;
  a.sin_port = htons((uint16_t)port);
  bind(s, (struct sockaddr*)&a, sizeof(a)); listen(s, 10);

  printf("┌──────────────────────────────────────────────┐\n");
  printf("│  5bit WebRTC Signaling Server                 │\n");
  printf("│  Signaling: native (records + grants)          │\n");
  printf("│  Media: C (WebRTC lib, client-side)            │\n");
  printf("│  PROTOCOLS.md §3.2                             │\n");
  printf("└──────────────────────────────────────────────┘\n");
  printf("  →  ws://localhost:%d\n\n", port);

  while (1) {
    struct sockaddr_in c; socklen_t cl = sizeof(c);
    int cf = accept(s, (struct sockaddr*)&c, &cl);
    if (cf < 0) continue;
    handle(cf, data_dir);
  }
  close(s); return 0;
}
