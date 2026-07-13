/* fivebit_dash.c — 5bit Dash (C binary, zero deps, LABEL-driven records)
 * ========================================================================
 * Projects + Tasks + Dashboard. Self-describing records via CMD_LABEL (§7).
 *
 * Build: cc -O2 -o fivebit_dash fivebit_dash.c fivebit_grid.c -lssl -lcrypto
 * Run:   ./fivebit_dash 8085 ./data
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
#include "fivebit_grid.h"

enum { TK_REC=28, TK_END=30, TK_START=31 };

/* ── Pack / Unpack ───────────────────────────────────────────────────────── */
static int srv_pack(const uint8_t *tk, int n, uint8_t *out, int *pad) {
  return pack_tokens(tk, n, out, pad);
}
static int srv_unpack(const uint8_t *data, int nbytes, int pad, uint8_t *tk, int max_tok) {
  uint64_t acc=0; int nbits=0,ntok=0;
  for(int i=0;i<nbytes&&ntok<max_tok;i++){acc=(acc<<8)|data[i];nbits+=8;while(nbits>=5&&ntok<max_tok){nbits-=5;tk[ntok++]=(acc>>nbits)&0x1F;}}
  (void)pad;return ntok;
}

/* ── Context-aware char tables ───────────────────────────────────────────── */
static const char *CTX[4]={
  "0123456789??????789??????",
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ .",
  "abcdefghijklmnopqrstuvwxyz@-",
  "!\"#$%&'()*+,/:;<=>?[\\]^_`{|}",
};

/* ── LABEL encoding (§7) — START×4 D5 END×5 enc_word(name) END enc_int(pos) */
static int enc_label(int pos, const char *name, uint8_t *tk, int cap) {
  if (cap < 15) return -1;
  int n = 0;
  for (int i = 0; i < 4; i++) tk[n++] = TK_START;
  tk[n++] = 5; /* CMD_LABEL = D5 */
  for (int i = 0; i < 4; i++) tk[n++] = TK_END;
  tk[n++] = TK_END;
  int w = enc_word(name, tk + n, cap - n);
  if (w < 0) return -1;
  n += w;
  tk[n++] = TK_END;
  int ip = enc_int(pos, tk + n, cap - n);
  if (ip < 0) return -1;
  n += ip;
  return n;
}

/* ── Label-aware field extraction ─────────────────────────────────────────── */
typedef struct { char name[64]; char value[512]; } LabeledField;

static int decode_labeled(const uint8_t *data, int nbytes,
                          LabeledField *fields, int max_fields) {
  uint8_t tk[4096]; int ntok = srv_unpack(data, nbytes, 0, tk, 4096);
  int fc = 0, depth = 0, i = 0;

  while (i < ntok && fc < max_fields) {
    /* Detect LABEL: START×4 + D5(5) */
    if (i + 5 <= ntok && tk[i]==TK_START && tk[i+1]==TK_START &&
        tk[i+2]==TK_START && tk[i+3]==TK_START && tk[i+4]==5) {
      i += 5;
      int ends = 0;
      while (i < ntok && ends < 5 && tk[i]==TK_END) { i++; ends++; }
      if (ends < 5) continue;

      /* Read label name (WORD) */
      char ln[128]; int lnp = 0; depth = 0;
      while (i < ntok && lnp < 127) {
        int t = tk[i];
        if (t == TK_START) { depth++; i++; continue; }
        if (t == TK_END) { if (depth == 0) break; depth--; i++; continue; }
        int d = depth > 3 ? 3 : depth;
        if (d >= 1 && t <= 27 && CTX[d] && CTX[d][t]) ln[lnp++] = CTX[d][t];
        else if (depth == 0 && t <= 9) ln[lnp++] = (char)('0' + t);
        i++;
      }
      ln[lnp] = '\0';
      if (i < ntok && tk[i] == TK_END) i++; /* skip label END */

      /* Skip position NUM (we use names, not positions) */
      depth = 0;
      while (i < ntok) {
        int t = tk[i];
        if (t == TK_START) { depth++; i++; continue; }
        if (t == TK_END) { if (depth == 0) break; depth--; i++; continue; }
        i++;
      }
      if (i < ntok && tk[i] == TK_END) i++;

      /* Read value until next LABEL or RECORD */
      char val[512]; int vl = 0; depth = 0;
      while (i < ntok && vl < 511) {
        int t = tk[i];
        if (t == TK_REC) break;
        if (i+5<=ntok && t==TK_START && tk[i+1]==TK_START &&
            tk[i+2]==TK_START && tk[i+3]==TK_START && tk[i+4]==5) break;
        if (t == TK_START) { depth++; i++; continue; }
        if (t == TK_END) { if (depth>0) depth--; i++; continue; }
        int d = depth > 3 ? 3 : depth;
        if (depth > 0 && t <= 27 && CTX[d] && CTX[d][t]) val[vl++] = CTX[d][t];
        else if (depth == 0 && t <= 9) val[vl++] = (char)('0' + t);
        else if (depth == 0 && t >= 17 && t <= 25) {
          val[vl++] = '-'; val[vl++] = (char)('0' + (t - 16));
        }
        i++;
      }
      val[vl] = '\0';
      int nl = (int)strlen(ln); if (nl > 63) nl = 63;
      memcpy(fields[fc].name, ln, nl); fields[fc].name[nl] = '\0';
      memcpy(fields[fc].value, val, vl < 512 ? vl+1 : 512); fields[fc].value[511] = '\0';
      fc++;
    } else { i++; }
  }
  return fc;
}

static const char *fval(LabeledField *f, int fc, const char *name) {
  for (int i = 0; i < fc; i++) if (strcmp(f[i].name, name) == 0) return f[i].value;
  return "";
}
static long long fnum(LabeledField *f, int fc, const char *name) {
  return atoll(fval(f, fc, name));
}

/* ── Record encode (LABEL-driven) ────────────────────────────────────────── */

static int enc_proj(const char *name, const char *desc, const char *color,
                    uint8_t *out, int *pad) {
  uint8_t tk[4096]; int n = 0;
  n += enc_label(0, "name", tk+n, 4096-n);
  n += enc_word(name, tk+n, 4096-n);
  n += enc_label(1, "desc", tk+n, 4096-n);
  n += enc_word(desc, tk+n, 4096-n);
  n += enc_label(2, "color", tk+n, 4096-n);
  n += enc_word(color, tk+n, 4096-n);
  tk[n++] = TK_REC;
  return srv_pack(tk, n, out, pad);
}

static int enc_task(const char *title, const char *status, long long proj_id,
                    uint8_t *out, int *pad) {
  uint8_t tk[4096]; int n = 0;
  n += enc_label(0, "title", tk+n, 4096-n);
  n += enc_word(title, tk+n, 4096-n);
  n += enc_label(1, "status", tk+n, 4096-n);
  n += enc_word(status, tk+n, 4096-n);
  n += enc_label(2, "project_id", tk+n, 4096-n);
  n += enc_int(proj_id, tk+n, 4096-n);
  tk[n++] = TK_REC;
  return srv_pack(tk, n, out, pad);
}

/* ── HTTP ────────────────────────────────────────────────────────────────── */
static void ok(int fd,const char*m,const char*b,int l){char h[512];int hl=snprintf(h,512,"HTTP/1.0 200 OK\r\nContent-Type: %s\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",m,l);write(fd,h,hl);if(b&&l>0)write(fd,b,l);}
static void created(int fd,const char*b,int l){char h[512];int hl=snprintf(h,512,"HTTP/1.0 201 Created\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",l);write(fd,h,hl);if(b&&l>0)write(fd,b,l);}
static void e404(int fd){const char*r="HTTP/1.0 404\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";write(fd,r,strlen(r));}
static void e400(int fd,const char*m){char r[512];int l=snprintf(r,512,"HTTP/1.0 400\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",(int)strlen(m),m);write(fd,r,l);}
static int jesc(char*b,int pos,const char*s){b[pos++]='"';for(const char*p=s;*p;p++){if(*p=='"'||*p=='\\')b[pos++]='\\';b[pos++]=*p;}b[pos++]='"';return pos;}
static int extr_str(char*body,const char*key,char*out,int cap){char s[64];snprintf(s,64,"\"%s\"",key);char*st=strstr(body,s);if(!st)return-1;st=strchr(st,':');if(!st)return-1;st++;while(*st==' '||*st=='"')st++;char*en=st;while(*en){if(*en=='\\'&&(en[1]=='"'||en[1]=='\\')){en+=2;continue;}if(*en=='"')break;en++;}if(!*en)return-1;int len=0;for(char*p=st;p<en&&len<cap-1;){if(*p=='\\'&&(p[1]=='"'||p[1]=='\\')){out[len++]=p[1];p+=2;}else out[len++]=*p++;}out[len]=0;return len;}
static int free_slot(const char*dir){int t=grid_total_entries(dir),rid=t;for(int i=0;i<t;i++){GridRecord*r=grid_read(dir,i);if(!r||r->is_tombstone){rid=i;if(r){free(r->parsed);free(r);}break;}if(r){free(r->parsed);free(r);}}return rid;}

/* ═══════════════════════════════════════════════════════════════════════════
 * HTML UI
 * ═══════════════════════════════════════════════════════════════════════════ */
static const char *HTML =
"<!DOCTYPE html><html lang=en><head><meta charset=UTF-8><meta name=viewport content='width=device-width,initial-scale=1.0'><title>5bit Dash</title><style>"
":root{--bg:#06060d;--surface:#0c0c18;--card:#111122;--border:#1a1a35;--text:#c8c8e0;--muted:#666;--accent:#6c5ce7;--accent2:#a78bfa;--green:#00e676;--amber:#ffab00;--red:#ff5252;--blue:#448aff;--pink:#ff4081;--cyan:#18ffff}"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI','Inter',system-ui,sans-serif;background:var(--bg);color:var(--text);min-height:100vh;display:flex;overflow:hidden}"
"@keyframes fadeIn{from{opacity:0;transform:translateY(8px)}to{opacity:1;transform:translateY(0)}}"
"@keyframes slideIn{from{opacity:0;transform:translateX(-12px)}to{opacity:1;transform:translateX(0)}}"
".sidebar{width:240px;background:var(--surface);border-right:1px solid var(--border);display:flex;flex-direction:column;padding:24px 0;flex-shrink:0;z-index:2}"
".logo{padding:0 24px 24px;display:flex;align-items:center;gap:10px}"
".logo-icon{width:32px;height:32px;background:linear-gradient(135deg,var(--accent),var(--pink));border-radius:8px;display:flex;align-items:center;justify-content:center;font-size:16px;font-weight:800;color:#fff}"
".logo-text{font-size:16px;font-weight:700;letter-spacing:-0.3px}.logo-text span{color:var(--accent2)}"
".nav-section{padding:0 16px;margin-bottom:8px;font-size:10px;text-transform:uppercase;letter-spacing:1.5px;color:var(--muted);font-weight:600}"
".nav-item{display:flex;align-items:center;gap:10px;padding:10px 20px;margin:0 8px 2px;border-radius:8px;cursor:pointer;font-size:13px;color:var(--muted);transition:all .15s;border-left:2px solid transparent;font-weight:500}"
".nav-item:hover{color:var(--text);background:var(--card)}"
".nav-item.active{color:#fff;background:var(--card);border-left-color:var(--accent)}"
".nav-item .ico{font-size:15px;width:20px;text-align:center}"
".nav-badge{background:var(--accent);color:#fff;border-radius:10px;padding:1px 7px;font-size:10px;font-weight:700;margin-left:auto}"
".main{flex:1;overflow-y:auto;padding:32px 40px}"
".topbar{display:flex;justify-content:space-between;align-items:center;margin-bottom:32px;animation:fadeIn .3s ease}"
".topbar h1{font-size:26px;font-weight:700;letter-spacing:-0.5px}"
".topbar h1 span{background:linear-gradient(135deg,var(--accent),var(--pink));-webkit-background-clip:text;-webkit-text-fill-color:transparent}"
".btn{display:inline-flex;align-items:center;gap:6px;padding:10px 18px;border-radius:8px;font-size:13px;font-weight:600;cursor:pointer;border:none;transition:all .15s;text-decoration:none}"
".btn-primary{background:linear-gradient(135deg,var(--accent),#8b5cf6);color:#fff;box-shadow:0 2px 12px #6c5ce740}"
".btn-primary:hover{transform:translateY(-1px);box-shadow:0 4px 20px #6c5ce760}"
".btn-ghost{background:transparent;border:1px solid var(--border);color:var(--muted)}.btn-ghost:hover{color:var(--text);border-color:#333}"
".btn-danger{background:transparent;border:1px solid #3a1a1a;color:var(--red);padding:5px 12px;font-size:11px}"
".btn-danger:hover{background:#3a1a1a}"
".btn-sm{padding:5px 12px;font-size:11px}"
".stats-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:16px;margin-bottom:32px}"
".stat-card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:20px 24px;position:relative;overflow:hidden;animation:fadeIn .4s ease;transition:all .2s}"
".stat-card:hover{border-color:#2a2a50;transform:translateY(-2px);box-shadow:0 8px 30px #00000030}"
".stat-card .ring{position:absolute;top:-20px;right:-20px;width:80px;height:80px;border-radius:50%;opacity:.1}"
".stat-card .ring.purple{background:var(--accent)}.stat-card .ring.green{background:var(--green)}.stat-card .ring.amber{background:var(--amber)}.stat-card .ring.blue{background:var(--blue)}"
".stat-card .ico{font-size:20px;margin-bottom:8px}"
".stat-card .num{font-size:30px;font-weight:800;letter-spacing:-1px;margin-bottom:2px}"
".stat-card .lbl{font-size:11px;color:var(--muted);text-transform:uppercase;letter-spacing:1px;font-weight:600}"
".card{background:var(--card);border:1px solid var(--border);border-radius:14px;padding:24px;margin-bottom:20px;animation:fadeIn .4s ease}"
".card-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:16px}"
".card-header h3{font-size:15px;font-weight:600}"
".card-header .sub{font-size:12px;color:var(--muted)}"
"table{width:100%;border-collapse:collapse}"
"th{text-align:left;font-size:10px;text-transform:uppercase;letter-spacing:1px;color:var(--muted);padding:10px 14px;border-bottom:1px solid var(--border);font-weight:600}"
"td{padding:12px 14px;font-size:13px;border-bottom:1px solid #0d0d20}"
"tr{animation:slideIn .3s ease;transition:background .15s}"
"tr:hover td{background:#1a1a35}"
".badge{display:inline-block;padding:3px 10px;border-radius:10px;font-size:10px;font-weight:700;letter-spacing:.3px}"
".badge-todo{background:#1a2a3a;color:var(--blue)}.badge-progress{background:#2a1a3a;color:var(--accent2)}.badge-done{background:#1a3a2a;color:var(--green)}.badge-review{background:#3a2a1a;color:var(--amber)}"
".color-dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px}"
".empty-state{text-align:center;padding:60px 20px;color:var(--muted)}"
".empty-state .icon{font-size:48px;margin-bottom:12px;opacity:.5}"
".empty-state p{font-size:14px}.empty-state .sub{font-size:12px;margin-top:4px}"
".modal-overlay{display:none;position:fixed;top:0;left:0;right:0;bottom:0;background:#000000cc;z-index:100;align-items:center;justify-content:center;backdrop-filter:blur(4px)}"
".modal{background:var(--surface);border:1px solid var(--border);border-radius:16px;padding:32px;width:500px;max-width:90vw;box-shadow:0 20px 60px #00000080;animation:fadeIn .2s ease}"
".modal h3{font-size:20px;font-weight:700;margin-bottom:24px;letter-spacing:-.3px}"
".modal label{display:block;font-size:11px;text-transform:uppercase;letter-spacing:1px;color:var(--muted);margin-bottom:6px;margin-top:16px;font-weight:600}"
".modal input,.modal select,.modal textarea{width:100%;padding:10px 14px;background:var(--bg);border:1px solid var(--border);border-radius:8px;color:var(--text);font-size:14px;outline:none;font-family:inherit;transition:border-color .15s;resize:vertical}"
".modal input:focus,.modal select:focus,.modal textarea:focus{border-color:var(--accent);box-shadow:0 0 0 3px #6c5ce720}"
".modal select option{background:var(--surface);color:var(--text)}.modal textarea{min-height:80px}"
".modal-actions{display:flex;gap:10px;justify-content:flex-end;margin-top:24px}"
".color-picker{display:flex;gap:8px;margin-top:8px}"
".color-swatch{width:28px;height:28px;border-radius:50%;cursor:pointer;border:3px solid transparent;transition:all .15s}"
".color-swatch:hover{transform:scale(1.15)}.color-swatch.sel{border-color:#fff;box-shadow:0 0 12px currentColor}"
".proj-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(280px,1fr));gap:16px}"
".proj-card{background:var(--card);border:1px solid var(--border);border-radius:14px;padding:24px;cursor:pointer;transition:all .2s;position:relative;overflow:hidden;animation:fadeIn .3s ease}"
".proj-card:hover{border-color:#2a2a50;transform:translateY(-2px);box-shadow:0 8px 30px #00000030}"
".proj-card .bar{position:absolute;top:0;left:0;right:0;height:3px}"
".proj-card h3{font-size:16px;font-weight:600;margin-bottom:6px;margin-top:8px}"
".proj-card .desc{font-size:12px;color:var(--muted);line-height:1.5;display:-webkit-box;-webkit-line-clamp:2;-webkit-box-orient:vertical;overflow:hidden}"
".proj-card .meta{display:flex;gap:12px;margin-top:14px;font-size:11px;color:var(--muted)}"
".board-cols{display:grid;grid-template-columns:repeat(4,1fr);gap:16px}"
".board-col{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:16px;min-height:200px}"
".board-col h4{font-size:12px;text-transform:uppercase;letter-spacing:1px;color:var(--muted);margin-bottom:12px;font-weight:600;display:flex;align-items:center;gap:8px}"
".board-col h4 .cnt{background:var(--border);border-radius:10px;padding:1px 8px;font-size:10px}"
".task-card{background:var(--surface);border:1px solid var(--border);border-radius:10px;padding:14px;margin-bottom:10px;cursor:grab;transition:all .15s;animation:slideIn .2s ease}"
".task-card:hover{border-color:#333;box-shadow:0 4px 12px #00000020}"
".task-card .t-title{font-size:13px;font-weight:500;margin-bottom:4px}"
".task-card .t-meta{font-size:10px;color:var(--muted)}"
".task-card .t-actions{display:flex;gap:6px;margin-top:8px}"
".toast{position:fixed;bottom:24px;right:24px;background:var(--card);border:1px solid var(--border);border-radius:10px;padding:14px 20px;font-size:13px;z-index:200;animation:fadeIn .3s ease;box-shadow:0 8px 30px #00000040;max-width:360px}"
".toast.success{border-color:var(--green)}.toast.error{border-color:var(--red)}"
"@media(max-width:900px){.sidebar{width:60px}.sidebar .logo-text,.sidebar .nav-item span:not(.ico),.sidebar .nav-badge,.sidebar .nav-section{display:none}.main{padding:24px 16px}.board-cols{grid-template-columns:1fr}.proj-grid{grid-template-columns:1fr}}"
"</style></head><body>"
"<div class=sidebar>"
"<div class=logo><div class=logo-icon>◆</div><div class=logo-text>5bit <span>Dash</span></div></div>"
"<div class=nav-section>Menu</div>"
"<div class='nav-item active' onclick=showTab('dashboard')><span class=ico>📊</span> Dashboard</div>"
"<div class=nav-item onclick=showTab('projects')><span class=ico>📁</span> Projects <span class=nav-badge id=projCount>0</span></div>"
"<div class=nav-item onclick=showTab('board')><span class=ico>📋</span> Board <span class=nav-badge id=taskCount>0</span></div>"
"<div style='margin-top:auto;padding:20px;border-top:1px solid var(--border)'>"
"<div style='font-size:10px;color:var(--muted);text-transform:uppercase;letter-spacing:1px;margin-bottom:8px'>Stack</div>"
"<div style='font-size:11px;color:var(--muted)'>C binary + libc</div>"
"<div style='font-size:11px;color:var(--muted)'>5bit AllocGrid</div>"
"<div style='font-size:11px;color:var(--accent2)'>LABEL-driven records (§7)</div></div></div>"
"<div class=main id=main></div>"
"<div class=modal-overlay id=modalOverlay><div class=modal id=modal></div></div>"
"<div id=toastContainer></div>"
"<script>"
"let projects=[],tasks=[];let tab='dashboard';const colors=['#6c5ce7','#ff4081','#448aff','#00e676','#ffab00','#18ffff'];"
"function esc(s){return(s||'').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/\"/g,'&quot;');}"
"function toast(msg,type){let t=document.createElement('div');t.className='toast '+(type||'success');t.textContent=msg;document.getElementById('toastContainer').appendChild(t);setTimeout(()=>t.remove(),2500);}"
"async function load(){"
"  let pr=await fetch('/api/projects');projects=(await pr.json()).projects||[];"
"  let tr=await fetch('/api/tasks');tasks=(await tr.json()).tasks||[];"
"  document.getElementById('projCount').textContent=projects.length;"
"  document.getElementById('taskCount').textContent=tasks.length;"
"  render();}"
"function showTab(t){tab=t;document.querySelectorAll('.nav-item').forEach((el,i)=>{el.classList.toggle('active',i==['dashboard','projects','board'].indexOf(t));});render();}"
"function render(){let m=document.getElementById('main');"
"if(tab==='dashboard'){"
"  let todo=tasks.filter(t=>t.status==='To Do').length,prog=tasks.filter(t=>t.status==='In Progress').length,done=tasks.filter(t=>t.status==='Done').length;"
"  m.innerHTML="
"'<div class=topbar><h1>📊 <span>Dashboard</span></h1><div style=font-size:12px;color:var(--muted)>'+new Date().toLocaleDateString('en-US',{weekday:'long',month:'long',day:'numeric'})+'</div></div>'+\n"
"'<div class=stats-grid>'+\n"
"'<div class=stat-card><div class=\"ring purple\"></div><div class=ico>📁</div><div class=num>'+projects.length+'</div><div class=lbl>Projects</div></div>'+\n"
"'<div class=stat-card><div class=\"ring blue\"></div><div class=ico>📋</div><div class=num>'+tasks.length+'</div><div class=lbl>Total Tasks</div></div>'+\n"
"'<div class=stat-card><div class=\"ring amber\"></div><div class=ico>⚡</div><div class=num>'+prog+'</div><div class=lbl>In Progress</div></div>'+\n"
"'<div class=stat-card><div class=\"ring green\"></div><div class=ico>✅</div><div class=num>'+done+'</div><div class=lbl>Completed</div></div>'+\n"
"'</div>'+\n"
"'<div class=card><div class=card-header><div><h3>Recent Tasks</h3><div class=sub>Latest activity across all projects</div></div></div>'+\n"
"'<table><tr><th>Task</th><th>Status</th><th>Project</th></tr>'+\n"
"tasks.slice(-8).reverse().map(t=>{let p=projects.find(x=>x._id===t.project_id);return'<tr><td>'+esc(t.title)+'</td><td><span class=\"badge badge-'+(t.status||'todo').toLowerCase().replace(/ /g,'')+'\">'+esc(t.status||'To Do')+'</span></td><td>'+(p?esc(p.name):'—')+'</td></tr>';}).join('')+\n"
"(tasks.length===0?'<tr><td colspan=3><div class=empty-state><div class=icon>📋</div><p>No tasks yet</p><div class=sub>Create a project and add tasks to get started</div></div></td></tr>':'')+'</table></div>';"
"}else if(tab==='projects'){"
"m.innerHTML="
"'<div class=topbar><h1>📁 <span>Projects</span></h1><button class=\"btn btn-primary\" onclick=showProjModal()>+ New Project</button></div>'+\n"
"projects.length===0?'<div class=empty-state><div class=icon>📁</div><p>No projects yet</p><div class=sub>Create your first project</div></div>':"
"'<div class=proj-grid>'+projects.map(p=>{let tc=tasks.filter(t=>t.project_id===p._id).length;return'<div class=proj-card onclick=showTab(\"board\")><div class=bar style=background:'+(p.color||'#6c5ce7')+'></div><h3>'+esc(p.name)+'</h3><div class=desc>'+esc(p.desc||'')+'</div><div class=meta><span>📋 '+tc+' tasks</span><span style=margin-left:auto><button class=\"btn btn-danger btn-sm\" onclick=\"event.stopPropagation();deleteProj('+p._id+')\">Delete</button></span></div></div>';}).join('')+'</div>';"
"}else if(tab==='board'){"
"  let statuses=['To Do','In Progress','Review','Done'];"
"  m.innerHTML="
"'<div class=topbar><h1>📋 <span>Board</span></h1><div style=display:flex;gap:8px><select id=boardProj onchange=renderBoard()><option value=all>All Projects</option>'+projects.map(p=>'<option value='+p._id+'>'+esc(p.name)+'</option>').join('')+'</select><button class=\"btn btn-primary\" onclick=showTaskModal()>+ Add Task</button></div></div>'+\n"
"'<div class=board-cols>'+statuses.map(s=>'<div class=board-col><h4>'+s+' <span class=cnt>'+tasks.filter(t=>{let bp=document.getElementById(\"boardProj\");return t.status===s&&(!bp||bp.value===\"all\"||t.project_id===parseInt(bp.value));}).length+'</span></h4><div id=col-'+s.replace(/ /g,'')+'></div></div>').join('')+'</div>';"
"  renderBoard();}"
"}"
"function renderBoard(){if(tab!=='board')return;let bp=document.getElementById('boardProj');let pid=bp?bp.value:'all';['ToDo','InProgress','Review','Done'].forEach(s=>{let status=s.replace(/([A-Z])/g,' $1').trim();let col=document.getElementById('col-'+status.replace(/ /g,''));if(!col)return;let filtered=tasks.filter(t=>t.status===status&&(pid==='all'||t.project_id===parseInt(pid)));if(filtered.length===0){col.innerHTML='<div style=text-align:center;color:var(--muted);padding:20px;font-size:12px>Drop tasks here</div>';return;}col.innerHTML=filtered.map(t=>{let p=projects.find(x=>x._id===t.project_id);return'<div class=task-card draggable=true data-id='+t._id+'><div class=t-title>'+esc(t.title)+'</div><div class=t-meta>'+(p?esc(p.name):'')+'</div><div class=t-actions><select onchange=moveTask('+t._id+',this.value) style=background:var(--bg);border:1px solid var(--border);border-radius:6px;color:var(--text);padding:3px 8px;font-size:10px;outline:none><option value=\"\">Move</option><option>To Do</option><option>In Progress</option><option>Review</option><option>Done</option></select><button class=\"btn btn-danger btn-sm\" onclick=deleteTask('+t._id+')>×</button></div></div>';}).join('');});}"
"function showProjModal(){"
"  document.getElementById('modal').innerHTML="
"'<h3>New Project</h3><label>Name</label><input id=fName placeholder=\"My Project\"><label>Description</label><textarea id=fDesc placeholder=\"What is this project about?\"></textarea><label>Color</label><div class=color-picker>'+colors.map((c,i)=>'<div class=\"color-swatch'+(i===0?' sel':'')+'\" style=background:'+c+';color:'+c+' onclick=\"document.querySelectorAll(\\'.color-swatch\\').forEach(el=>el.classList.remove(\\'sel\\'));this.classList.add(\\'sel\\')\"></div>').join('')+'</div><div class=modal-actions><button class=\"btn btn-ghost\" onclick=closeModal()>Cancel</button><button class=\"btn btn-primary\" onclick=saveProj()>Create Project</button></div>';"
"  document.getElementById('modalOverlay').style.display='flex';}"
"function showTaskModal(){"
"  let popts=projects.map(p=>'<option value='+p._id+'>'+esc(p.name)+'</option>').join('');"
"  document.getElementById('modal').innerHTML="
"'<h3>New Task</h3><label>Title</label><input id=fTitle placeholder=\"What needs to be done?\"><label>Project</label><select id=fProj>'+popts+'</select><label>Status</label><select id=fStatus><option>To Do</option><option>In Progress</option><option>Review</option><option>Done</option></select><div class=modal-actions><button class=\"btn btn-ghost\" onclick=closeModal()>Cancel</button><button class=\"btn btn-primary\" onclick=saveTask()>Create Task</button></div>';"
"  document.getElementById('modalOverlay').style.display='flex';}"
"function closeModal(){document.getElementById('modalOverlay').style.display='none';}"
"async function saveProj(){"
"  let sel=document.querySelector('.color-swatch.sel');let color=sel?sel.style.background:'#6c5ce7';"
"  let d={name:document.getElementById('fName').value,desc:document.getElementById('fDesc').value,color:color};"
"  if(!d.name){toast('Name is required','error');return;}"
"  await fetch('/api/projects',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)});"
"  closeModal();toast('Project created!');load();}"
"async function saveTask(){"
"  let d={title:document.getElementById('fTitle').value,status:document.getElementById('fStatus').value,project_id:parseInt(document.getElementById('fProj').value)||0};"
"  if(!d.title){toast('Title is required','error');return;}"
"  if(!projects.length){toast('Create a project first','error');return;}"
"  await fetch('/api/tasks',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)});"
"  closeModal();toast('Task added!');load();}"
"async function deleteProj(id){await fetch('/api/projects/'+id,{method:'DELETE'});toast('Project deleted');load();}"
"async function deleteTask(id){await fetch('/api/tasks/'+id,{method:'DELETE'});toast('Task deleted');load();}"
"async function moveTask(id,status){if(!status)return;await fetch('/api/tasks/'+id,{method:'PATCH',headers:{'Content-Type':'application/json'},body:JSON.stringify({status:status})});load();}"
"load();</script></body></html>";

/* ── Handler ─────────────────────────────────────────────────────────────── */
static void handle(int fd, const char *base) {
  char buf[8192]; int n=read(fd,buf,8191); if(n<=0){close(fd);return;} buf[n]=0;
  char method[16]={0},path[256]={0}; sscanf(buf,"%15s %255s",method,path);
  char dec[256];int dp=0;
  for(int i=0;path[i]&&dp<255;i++){if(path[i]=='%'&&path[i+1]&&path[i+2]){int h,l;sscanf(path+i+1,"%1x%1x",&h,&l);dec[dp++]=(char)((h<<4)|l);i+=2;}else dec[dp++]=path[i];}dec[dp]=0;
  char*body=strstr(buf,"\r\n\r\n");if(body)body+=4;
  char pdir[512],tdir[512];
  snprintf(pdir,512,"%s/projects",base);snprintf(tdir,512,"%s/tasks",base);

  if(strcmp(method,"GET")==0&&strcmp(dec,"/")==0){ok(fd,"text/html; charset=utf-8",HTML,(int)strlen(HTML));close(fd);return;}

  /* ── PROJECTS (labeled) ───────────────────────────────────────────────── */
  if(strcmp(method,"GET")==0&&strcmp(dec,"/api/projects")==0){
    char json[65536];int pos=snprintf(json,65536,"{\"projects\":[");int first=1;
    for(int i=0;i<grid_total_entries(pdir);i++){GridRecord*r=grid_read(pdir,i);if(!r||r->is_tombstone){if(r){free(r->parsed);free(r);}continue;}
      LabeledField f[8]; int fc = decode_labeled(r->parsed, (r->bit_length+7)/8, f, 8);
      const char *nm = fval(f, fc, "name");
      if (strlen(nm) > 0) {
        if (!first) json[pos++]=',';
        pos+=snprintf(json+pos,65536-pos,"{\"_id\":%d,\"name\":",r->record_id); pos=jesc(json,pos,nm);
        pos+=snprintf(json+pos,65536-pos,",\"desc\":"); pos=jesc(json,pos,fval(f,fc,"desc"));
        pos+=snprintf(json+pos,65536-pos,",\"color\":"); pos=jesc(json,pos,fval(f,fc,"color"));
        pos+=snprintf(json+pos,65536-pos,"}"); first=0;
      }
      free(r->parsed); free(r);
    }
    pos+=snprintf(json+pos,65536-pos,"]}");ok(fd,"application/json",json,pos);close(fd);return;}

  if(strcmp(method,"POST")==0&&strcmp(dec,"/api/projects")==0){
    char nm[256]={0},dc[512]={0},cl[32]="#6c5ce7";
    extr_str(body,"name",nm,256);extr_str(body,"desc",dc,512);extr_str(body,"color",cl,32);
    if(!nm[0]){e400(fd,"name required");close(fd);return;}
    int rid=free_slot(pdir);uint8_t pk[4096];int pad;
    int pl=enc_proj(nm,dc,cl,pk,&pad);if(pl<0){e400(fd,"encode error");close(fd);return;}
    grid_write(pdir,rid,pk,pl,pl*8-pad);
    char j[4096];int p=snprintf(j,4096,"{\"_id\":%d,\"name\":",rid);p=jesc(j,p,nm);p+=snprintf(j+p,4096-p,",\"desc\":");p=jesc(j,p,dc);p+=snprintf(j+p,4096-p,",\"color\":");p=jesc(j,p,cl);p+=snprintf(j+p,4096-p,"}");created(fd,j,p);close(fd);return;}

  if(strcmp(method,"DELETE")==0&&strncmp(dec,"/api/projects/",14)==0){int rid=atoi(dec+14);grid_delete(pdir,rid);
    char j[64];int p=snprintf(j,64,"{\"deleted\":%d}",rid);ok(fd,"application/json",j,p);close(fd);return;}

  /* ── TASKS (labeled) ──────────────────────────────────────────────────── */
  if(strcmp(method,"GET")==0&&strcmp(dec,"/api/tasks")==0){
    char json[65536];int pos=snprintf(json,65536,"{\"tasks\":[");int first=1;
    for(int i=0;i<grid_total_entries(tdir);i++){GridRecord*r=grid_read(tdir,i);if(!r||r->is_tombstone){if(r){free(r->parsed);free(r);}continue;}
      LabeledField f[8]; int fc = decode_labeled(r->parsed, (r->bit_length+7)/8, f, 8);
      const char *tt = fval(f, fc, "title");
      if (strlen(tt) > 0) {
        if (!first) json[pos++]=',';
        pos+=snprintf(json+pos,65536-pos,"{\"_id\":%d,\"title\":",r->record_id); pos=jesc(json,pos,tt);
        pos+=snprintf(json+pos,65536-pos,",\"status\":"); pos=jesc(json,pos,fval(f,fc,"status"));
        pos+=snprintf(json+pos,65536-pos,",\"project_id\":%lld}",fnum(f,fc,"project_id")); first=0;
      }
      free(r->parsed); free(r);
    }
    pos+=snprintf(json+pos,65536-pos,"]}");ok(fd,"application/json",json,pos);close(fd);return;}

  if(strcmp(method,"POST")==0&&strcmp(dec,"/api/tasks")==0){
    char tt[256]={0},st[32]="To Do";long long pid=0;
    extr_str(body,"title",tt,256);extr_str(body,"status",st,32);
    char*ps=strstr(body,"\"project_id\"");if(ps){ps=strchr(ps,':');if(ps)pid=atoll(ps+1);}
    if(!tt[0]){e400(fd,"title required");close(fd);return;}
    int rid=free_slot(tdir);uint8_t pk[4096];int pad;
    int pl=enc_task(tt,st,pid,pk,&pad);if(pl<0){e400(fd,"encode error");close(fd);return;}
    grid_write(tdir,rid,pk,pl,pl*8-pad);
    char j[4096];int p=snprintf(j,4096,"{\"_id\":%d,\"title\":",rid);p=jesc(j,p,tt);
    p+=snprintf(j+p,4096-p,",\"status\":");p=jesc(j,p,st);p+=snprintf(j+p,4096-p,",\"project_id\":%lld}",pid);created(fd,j,p);close(fd);return;}

  if(strcmp(method,"PATCH")==0&&strncmp(dec,"/api/tasks/",11)==0){
    int rid=atoi(dec+11);char st[32]={0};extr_str(body,"status",st,32);
    if(!st[0]){e400(fd,"status required");close(fd);return;}
    GridRecord*r=grid_read(tdir,rid);if(!r||r->is_tombstone){e404(fd);if(r){free(r->parsed);free(r);}close(fd);return;}
    LabeledField f[8]; int fc = decode_labeled(r->parsed, (r->bit_length+7)/8, f, 8);
    const char *tt = fval(f, fc, "title"); long long pid = fnum(f, fc, "project_id");
    uint8_t pk[4096];int pad;int pl=enc_task(tt,st,pid,pk,&pad);
    grid_write(tdir,rid,pk,pl,pl*8-pad);
    char j[4096];int p=snprintf(j,4096,"{\"_id\":%d,\"title\":",rid);p=jesc(j,p,tt);p+=snprintf(j+p,4096-p,",\"status\":");p=jesc(j,p,st);p+=snprintf(j+p,4096-p,",\"project_id\":%lld}",pid);
    ok(fd,"application/json",j,p);free(r->parsed);free(r);close(fd);return;}

  if(strcmp(method,"DELETE")==0&&strncmp(dec,"/api/tasks/",11)==0){int rid=atoi(dec+11);grid_delete(tdir,rid);
    char j[64];int p=snprintf(j,64,"{\"deleted\":%d}",rid);ok(fd,"application/json",j,p);close(fd);return;}

  e404(fd);close(fd);
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
  int port=argc>1?atoi(argv[1]):8085;
  const char*dir=argc>2?argv[2]:"./data_dash";
  char pd[512],td[512];snprintf(pd,512,"%s/projects",dir);snprintf(td,512,"%s/tasks",dir);
  mkdir(dir,0755);mkdir(pd,0755);mkdir(td,0755);
  grid_init(pd);grid_init(td);
  int s=socket(AF_INET,SOCK_STREAM,0);int o=1;setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&o,sizeof(o));
  struct sockaddr_in a={0};a.sin_family=AF_INET;a.sin_addr.s_addr=INADDR_ANY;a.sin_port=htons((uint16_t)port);
  bind(s,(struct sockaddr*)&a,sizeof(a));listen(s,10);
  printf("┌──────────────────────────────────────────┐\n");
  printf("│  5bit Dash — LABEL-driven records (§7)    │\n");
  printf("│  Projects + Kanban Board + Analytics       │\n");
  printf("│  C binary + libc. 0 npm packages.          │\n");
  printf("└──────────────────────────────────────────┘\n");
  printf("  →  http://localhost:%d\n\n",port);
  while(1){struct sockaddr_in c;socklen_t cl=sizeof(c);int cf=accept(s,(struct sockaddr*)&c,&cl);if(cf<0)continue;handle(cf,dir);}
  close(s);return 0;
}
