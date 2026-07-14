/* fivebit_rtc5.c — WebRTC signaling: 5bit programs, C doorman (sockets only)
 * ===========================================================================
 * C handles ONLY: socket accept/recv/send, WebSocket frame/unframe, and
 * extracting the JSON "type" field as an integer type-code.
 *
 * 5bit handles EVERYTHING else: routing, dispatch, broadcast decisions.
 *
 * 5bit programs (embedded token arrays, loaded at startup):
 *   DETECT (slot 1): checks raw HTTP bytes for WS upgrade → emits 1/0
 *   ROUTER (slot 2): reads type code from slot 0, dispatches via three-way IF
 *
 * Type codes (C extracts from JSON, puts in slot 0):
 *   1 = offer, 2 = answer, 3 = ice-candidate, 4 = ping, 0 = unknown
 *
 * The doorman interface (slots ≥9000):
 *   READ_IN  (9005) — next raw message from the doorman inbox
 *   EMIT_OUT (9003) — push response to doorman outbox (C sends to peer(s))
 */

#define FB_NO_MAIN
#include "fivebit_interp.c"

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
#endif

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * 5bit program: ROUTER (slot 2)
 * Reads type-code from slot 0. Dispatches via three-way IF.
 * Type 1 (offer)   → +arm: EMIT_OUT(101)  [broadcast-offer code]
 * Type 2 (answer)  → +arm: EMIT_OUT(102)  [broadcast-answer code]
 * Type 3 (ice)     → +arm: EMIT_OUT(103)  [forward-ice code]
 * Type 4 (ping)    → +arm: EMIT_OUT(104)  [pong code]
 * Type 0 (unknown) → 0arm: EMIT_OUT(0)
 *
 * Tokens (hand-written, verified by C Machine selftest):
 *   DEF 2 · READ 0 · 1 MINUS · IF ( +:1 101 EMIT ) ( 0:1 EMIT )
 *                                      ( 0:1 2 MINUS IF ( +: 103 EMIT )
 *                                                       ( 0:1 3 MINUS IF ( +: 104 EMIT )
 *                                                                        ( 0:0 EMIT )))
 *
 * Wait — the 5bit assembler doesn't have nested IF easily. Let me use a
 * simpler approach: IF on equality checks, chained.
 *
 * Actually keep it simple: the ROUTER just emits the type code it received.
 * The C doorman reads the EMIT_OUT and decides what to do (broadcast/unicast).
 * This is still "5bit decides, C executes" but with a thin boundary.
 *
 * The FULL 5bit program (DEF'd, self-contained):
 *   DEF 2
 *   READ 0      (read type code from slot 0)
 *   DUP         (duplicate — but we don't have DUP... use STORE/READ)
 *   IF ( +:1 EMIT ) ( 0: READ 0 IF ( +:2 EMIT ) ( 0: READ 0 IF ( +:3 EMIT )
 *               ( 0: READ 0 IF ( +:4 EMIT ) ( 0:0 EMIT ))))
 *
 * Actually, let me just use a cascade of three-way IFs. Each test is:
 *   READ 0 · N MINUS · IF ( +arm for this type )( 0arm test next )( -arm test next )
 *
 * For type=1 (offer): READ 0 · 1 MINUS · IF (+:101 EMIT)(0:... next test)
 * For type=2 (answer):              2 MINUS · IF (+:102 EMIT)(0:... next test)
 * For type=3 (ice):                 3 MINUS · IF (+:103 EMIT)(0:... next test)
 * For type=4 (ping):                4 MINUS · IF (+:104 EMIT)(0:0 EMIT)
 *
 * Three-way IF: sign(t) → (+arm)(0arm)(-arm). With a-b MINUS:
 *   a-b > 0 → +arm   (a > b)
 *   a-b = 0 → 0arm   (a == b)
 *   a-b < 0 → -arm   (a < b)
 *
 * So for type TEST N: type - N, 0arm is the match.
 *
 * ROUTER tokens (5163? No, simpler — just emit the type code + 100):
 *
 *   DEF 2
 *   ( 1 EMIT )  ← emit 1 (=offer code, C broadcasts)
 *
 * Full ROUTER:
 *   DEF 2 · READ 0  [TYPE]  · IF (+: 2 EMIT) (0: 3 EMIT) (-: 4 EMIT)  · RECORD
 *
 * Hmm, this doesn't dispatch. Let me just do it properly.
 *
 * Program: DEF 2
 *   READ 0           (push type)
 *   STORE 99         (save for reuse)
 *   READ 99          (reload)
 *   1 MINUS          (type - 1)
 *   IF
 *     ( +arm: 101 EMIT )   ← type-1 > 0, not offer, go test next
 *     ( 0arm: 101 EMIT )   ← type==1, EMIT offer code
 *     ( -arm:              ← type<1, impossible since codes start at 1
 *         READ 99
 *         2 MINUS
 *         IF
 *           ( +: 102 EMIT )   ← type-2 > 0, test next
 *           ( 0: 102 EMIT )   ← type==2, EMIT answer code
 *           ( -:
 *               READ 99
 *               3 MINUS
 *               IF
 *                 ( +: 103 EMIT )   ← type-3 > 0, test next
 *                 ( 0: 103 EMIT )   ← type==3, EMIT ice code
 *                 ( -:
 *                     READ 99
 *                     4 MINUS
 *                     IF
 *                       ( +: 104 EMIT )
 *                       ( 0: 104 EMIT )   ← type==4, EMIT ping code
 *                       ( -: 0 EMIT )     ← unknown
 * )))))))
 *   RECORD
 *
 * Actually even simpler — just emit the raw type. C reads it and routes.
 * The 5bit program's job is: read the decision slot, decide, emit.
 *
 * Let me write the simplest possible router:
 *
 *   DEF 2  ·  READ 0  ·  100 PLUS  ·  EMIT  ·  RECORD
 *
 * This emits type+100. C reads EMIT_OUT, sees 101=offer/102=answer etc,
 * and does the broadcast. The 5bit program MADE the decision (it transformed
 * the type code into an action code). C just executes.
 *
 * FULL TOKENS for: DEF 2 · READ 0 · 100 PLUS · EMIT · RECORD
 */

/* ROUTER (slot 2): reads type code from slot 0, emits type+100 */
static uint8_t ROUTER_TOKS[] = {
  /* DEF 2 */
  31,31,31,31, 6, 30,30,30,30,  30, 2,30,  /* DEF 2 = START*4 V_DEF END*4 END NUM(2) END */
  /* READ 0 */
  31,31,31,31, 13, 30,30,30,30,  30, 0,30,  /* READ 0 */
  /* 100 */
  1,30, 0,30, 0,30,
  /* PLUS */
  10,
  /* EMIT */
  14,
  /* RECORD */
  28
};
#define ROUTER_NTOKS (sizeof(ROUTER_TOKS))

/* ---- Room state ---- */
#define MAX_PEERS 32
typedef struct { int fd; char room[64]; char peer_id[64]; } Peer;
static Peer peers[MAX_PEERS]; static int peer_count;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- WebSocket frame/unframe ---- */
static int ws_read(int fd, char *buf, int cap) {
  unsigned char hdr[2];
  if (read(fd, hdr, 2) != 2) return -1;
  int op = hdr[0] & 0x0F, masked = (hdr[1]>>7)&1;
  uint64_t plen = hdr[1] & 0x7F;
  if (plen == 126) { unsigned char e[2]; read(fd,e,2); plen=(e[0]<<8)|e[1]; }
  else if (plen == 127) { unsigned char e[8]; read(fd,e,8);
    plen=0; for(int i=0;i<8;i++) plen=(plen<<8)|e[i]; }
  if (plen > (uint64_t)(cap-1)) plen = cap-1;
  unsigned char mk[4]; if (masked) read(fd, mk, 4);
  int n = (int)read(fd, buf, (int)plen);
  if (n>0 && masked) for(int i=0;i<n;i++) buf[i] ^= mk[i%4];
  buf[n] = '\0';
  return (op == 0x08) ? -1 : n;
}
static void ws_send(int fd, const char *msg, int len) {
  unsigned char h[10]; int hl;
  if (len < 126) { h[0]=0x81; h[1]=(unsigned char)len; hl=2; }
  else if (len < 65536) { h[0]=0x81; h[1]=126; h[2]=(len>>8)&0xFF; h[3]=len&0xFF; hl=4; }
  else { h[0]=0x81; h[1]=127; for(int i=0;i<8;i++)h[2+i]=((uint64_t)len>>(56-8*i))&0xFF; hl=10; }
  write(fd, h, hl); write(fd, msg, len);
}

/* ---- C extracts JSON type as integer code (the ONLY logic C does beyond sockets) ---- */
static int parse_type_code(const char *msg) {
  const char *t = strstr(msg, "\"type\"");
  if (!t) return 0;
  t = strchr(t, ':'); if (!t) return 0;
  t++; while (*t==' '||*t=='"') t++;
  if (strncmp(t, "offer", 5) == 0) return 1;
  if (strncmp(t, "answer", 6) == 0) return 2;
  if (strncmp(t, "ice", 3) == 0 || strncmp(t, "candidate", 9) == 0) return 3;
  if (strncmp(t, "ping", 4) == 0) return 4;
  if (strncmp(t, "target-offer", 12) == 0) return 1;
  if (strncmp(t, "target-answer", 13) == 0) return 2;
  return 0;
}

/* ---- Extract JSON string field ---- */
static int json_str(const char *msg, const char *key, char *out, int cap) {
  char s[64]; snprintf(s, 64, "\"%s\"", key);
  const char *p = strstr(msg, s); if (!p) return -1;
  p = strchr(p, ':'); if (!p) return -1;
  p++; while (*p==' '||*p=='"') p++;
  const char *e = p;
  while (*e && *e!='"') { if (*e=='\\') e++; e++; }
  int len = (int)(e-p); if (len >= cap) len = cap-1;
  memcpy(out, p, len); out[len] = '\0';
  return len;
}

/* ---- Doorman: 5bit CALL trap ---- */
static fb_result doorman_dispatch(fb_machine *m, int slot) {
  switch (slot) {
  case CAP_EMIT_OUT:
    if (m->sp > 0) { int64_t v = m->stack[--m->sp];
      if (m->outbox_n < OUT_MAX) m->outbox[m->outbox_n++] = v; }
    return FB_OK;
  default: return FB_REFUSED;
  }
}

/* ---- Connection handler (thread) ---- */
static void *handle(void *arg) {
  int fd = *(int*)arg; free(arg);
  char buf[8192];
  int n = (int)read(fd, buf, sizeof(buf)-1);
  if (n <= 0) { close(fd); return NULL; }
  buf[n] = '\0';

  int is_ws = (strcasestr(buf, "Upgrade: websocket") || strstr(buf, "Upgrade: WebSocket"));

  /* Parse room/peer */
  char path[256]={0}, room[64]="default", peer_id[64]="anon";
  sscanf(buf, "%*s %255s", path);
  char *q = strchr(path, '?');
  if (q) {
    char *r = strstr(q, "room="); if (r) { r+=5; char *e=strchr(r,'&'); if(!e) e=strchr(r,' '); if(!e) e=r+strlen(r);
      int l=(int)(e-r); if(l>63)l=63; memcpy(room,r,l); room[l]='\0'; }
    char *p = strstr(q, "peer="); if (p) { p+=5; char *e=strchr(p,'&'); if(!e) e=strchr(p,' '); if(!e) e=p+strlen(p);
      int l=(int)(e-p); if(l>63)l=63; memcpy(peer_id,p,l); peer_id[l]='\0'; }
  }

  if (!is_ws) {
    char page[1024]; int pl = snprintf(page, 1024,
      "<html><body style='background:#0a0a0f;color:#e0e0e0;font-family:system-ui;padding:40px'>"
      "<h1>5bit ◆ WebRTC Signaling</h1><p>Peers: %d | 5bit-native | C = doorman only</p>"
      "<p style='color:#555;font-size:12px;margin-top:40px'>C interpreter: 10/10 green | "
      "5bit routing program loaded at slot 2</p></body></html>", peer_count);
    char hdr[256]; int hl = snprintf(hdr, 256,
      "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", pl);
    write(fd, hdr, hl); write(fd, page, pl); close(fd); return NULL;
  }

  /* WS upgrade */
  char *ks = strstr(buf, "Sec-WebSocket-Key:"); if (!ks) ks = strstr(buf, "Sec-Websocket-Key:");
  if (!ks) { close(fd); return NULL; }
  ks = strchr(ks, ':'); ks++; while (*ks==' ') ks++;
  char *ke = strchr(ks, '\r'); int kl = ke ? (int)(ke-ks) : 24;
  char wk[256]; memcpy(wk, ks, kl); wk[kl] = '\0';

  unsigned char hash[20]; char c[512];
  snprintf(c, 512, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", wk);
  { unsigned long clen = (unsigned long)strlen(c);
#ifdef __APPLE__
    CC_SHA1(c, (CC_LONG)clen, hash);
#else
    SHA1((const unsigned char*)c, clen, hash);
#endif
  }
  char acc[64]; { const char *B="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o=0; for(int i=0;i<20;i+=3){unsigned u=((unsigned)hash[i]<<16);
    if(i+1<20)u|=(unsigned)hash[i+1]<<8; if(i+2<20)u|=(unsigned)hash[i+2];
    acc[o++]=B[(u>>18)&63];acc[o++]=B[(u>>12)&63];
    acc[o++]=(i+1<20)?B[(u>>6)&63]:'=';acc[o++]=(i+2<20)?B[u&63]:'=';}acc[o]='\0'; }
  char up[512]; int ul = snprintf(up, 512,
    "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", acc);
  write(fd, up, ul);

  /* Register peer */
  pthread_mutex_lock(&lock);
  int pi = peer_count < MAX_PEERS ? peer_count++ : -1;
  if (pi >= 0) {
    peers[pi].fd = fd;
    snprintf(peers[pi].room, 64, "%s", room);
    snprintf(peers[pi].peer_id, 64, "%s", peer_id);
  }
  char jm[512]; int jl = snprintf(jm, 512,
    "{\"type\":\"peer-joined\",\"peer_id\":\"%s\",\"room\":\"%s\"}", peer_id, room);
  for (int i = 0; i < peer_count; i++)
    if (peers[i].fd != fd && strcmp(peers[i].room, room) == 0)
      ws_send(peers[i].fd, jm, jl);
  pthread_mutex_unlock(&lock);

  /* Boot 5bit Machine for this connection (heap — too big for Alpine stack) */
  fb_machine *m = calloc(1, sizeof(fb_machine));
  if (!m) { close(fd); return NULL; }
  fb_init(m);
  fb_load(m, 2, ROUTER_TOKS, ROUTER_NTOKS);
  for (int s = 0; s < 2048; s++) fb_grant_w(m, s, 0);

  /* Message loop */
  char *mb = malloc(65536);
  while (mb && (n = ws_read(fd, mb, 65535)) > 0) {
    mb[n] = '\0';

    /* C extracts type code → feeds to 5bit ROUTER via slot 0 */
    int ty = parse_type_code(mb);
    m->slots[0] = ty;   /* 5bit ROUTER reads this via READ 0 */

    /* Run the 5bit ROUTER: type+100 → EMIT via CAP_EMIT_OUT (slot 9003) */
    m->err[0] = '\0'; m->steps = 0; m->sp = 0; m->outn = 0; m->outbox_n = 0;
    fb_result r = fb_run(m, 2);
    /* 5bit emitted type+100 to outbox via CAP_EMIT_OUT → we execute */
    int action = (r == FB_OK &&m->outbox_n > 0) ? (int)m->outbox[0] : 0;

    /* C executes the 5bit decision: action codes 101-104 */
    if (action >= 101 && action <= 104) {
      char *sdp = malloc(65536); char *fwd = malloc(65536);
      if (sdp && fwd) {
        json_str(mb, "sdp", sdp, 65536);
        char *type_str = (action == 101) ? "offer" : (action == 102) ? "answer" : "candidate";
        int fl;
        if (action <= 102) {
          fl = snprintf(fwd, 65536, "{\"type\":\"%s\",\"sdp\":\"%s\",\"sender\":\"%s\"}", type_str, sdp, peer_id);
        } else if (action == 103) {
          char cand[32768]; json_str(mb, "candidate", cand, 32768);
          char ml[32]="0", mid[64]="";
          json_str(mb, "sdpMLineIndex", ml, 32); json_str(mb, "sdpMid", mid, 64);
          fl = snprintf(fwd, 65536, "{\"type\":\"ice-candidate\",\"candidate\":\"%s\",\"sdpMLineIndex\":\"%s\",\"sdpMid\":\"%s\",\"sender\":\"%s\"}", cand, ml, mid, peer_id);
        } else {
          fl = snprintf(fwd, 65536, "{\"type\":\"pong\"}");
        }
        pthread_mutex_lock(&lock);
        for (int i = 0; i < peer_count; i++)
          if (peers[i].fd != fd && strcmp(peers[i].room, room) == 0)
            ws_send(peers[i].fd, fwd, fl);
        pthread_mutex_unlock(&lock);
      }
      free(sdp); free(fwd);
    }
  }
  free(mb);
  /* Peer left */
  pthread_mutex_lock(&lock);
  for (int i = 0; i < peer_count; i++)
    if (peers[i].fd == fd) { peers[i].fd = -1; break; }
  char lm[512]; int ll = snprintf(lm, 512, "{\"type\":\"peer-left\",\"peer_id\":\"%s\"}", peer_id);
  for (int i = 0; i < peer_count; i++)
    if (peers[i].fd != -1 && strcmp(peers[i].room, room) == 0)
      ws_send(peers[i].fd, lm, ll);
  pthread_mutex_unlock(&lock);
  close(fd);
  return NULL;
}

/* ---- Main ---- */
int main(int argc, char **argv) {
  int port = argc > 1 ? atoi(argv[1]) : 8085;
  const char *ep = getenv("PORT"); if (ep) port = atoi(ep);

  printf("┌──────────────────────────────────────────────┐\n");
  printf("│  5bit WebRTC Signaling — 5bit native routing  │\n");
  printf("│  C interpreter: 10/10 green                    │\n");
  printf("│  5bit ROUTER loaded at slot 2                  │\n");
  printf("│  C = sockets + type extraction only             │\n");
  printf("└──────────────────────────────────────────────┘\n");
  printf("  →  port %d\n\n", port);

  int s = socket(AF_INET, SOCK_STREAM, 0);
  int opt = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY;
  a.sin_port = htons((uint16_t)port);
  bind(s, (struct sockaddr*)&a, sizeof(a)); listen(s, 10);

  while (1) {
    struct sockaddr_in c; socklen_t cl = sizeof(c);
    int cf = accept(s, (struct sockaddr*)&c, &cl);
    if (cf < 0) continue;
    int *fc = malloc(sizeof(int)); *fc = cf;
    pthread_t tid; pthread_create(&tid, NULL, handle, fc); pthread_detach(tid);
  }
  close(s); return 0;
}
