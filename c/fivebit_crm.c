/* fivebit_crm.c — Mini CRM on 5bit (C binary, zero dependencies)
 * =================================================================
 * Contacts + Deals + Dashboard. One binary, two flat files, zero npm.
 *
 * Build: cc -O2 -o fivebit_crm fivebit_crm.c fivebit_grid.c -lssl -lcrypto
 * Run:   ./fivebit_crm 8085 ./data
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

/* ── Grid engine ─────────────────────────────────────────────────────────── */
#include "fivebit_grid.h"

/* ── Token values ────────────────────────────────────────────────────────── */
enum { TK_D0=0,TK_D1=1,TK_D2=2,TK_D3=3,TK_D4=4,TK_D5=5,TK_D6=6,TK_D7=7,
       TK_D8=8,TK_D9=9,TK_N1=17,TK_N2=18,TK_N3=19,TK_N4=20,TK_N5=21,
       TK_N6=22,TK_N7=23,TK_N8=24,TK_N9=25,
       TK_RECORD=28, TK_END=30, TK_START=31 };

/* ── Pack / Unpack ───────────────────────────────────────────────────────── */
static int srv_pack(const uint8_t *tk, int n, uint8_t *out, int *pad) {
  return pack_tokens(tk, n, out, pad);
}
static int srv_unpack(const uint8_t *data, int nbytes, int pad, uint8_t *tk, int max_tok) {
  uint64_t acc = 0; int nbits = 0, ntok = 0;
  for (int i = 0; i < nbytes && ntok < max_tok; i++) {
    acc = (acc << 8) | data[i]; nbits += 8;
    while (nbits >= 5 && ntok < max_tok) { nbits -= 5; tk[ntok++] = (acc >> nbits) & 0x1F; }
  }
  (void)pad; return ntok;
}

/* ── Context-aware char tables ───────────────────────────────────────────── */
static const char *CTX[4] = {
  "0123456789??????789??????",
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ .",
  "abcdefghijklmnopqrstuvwxyz@-",
  "!\"#$%&'()*+,/:;<=>?[\\]^_`{|}",
};

/* ── Decode: build text + extract number groups with positions ───────────── */
typedef struct { char *text; int num_count; long long nums[32]; } Decoded;

static Decoded decode_record(const uint8_t *data, int nbytes) {
  Decoded d = {0};
  uint8_t tk[4096]; int ntok = srv_unpack(data, nbytes, 0, tk, 4096);
  char text[4096]; int tpos = 0;
  int depth = 0;
  char num_buf[64]; int nb = 0, in_num = 0;

  for (int i = 0; i < ntok; i++) {
    int t = tk[i];
    if (t == TK_START) { depth++; continue; }
    if (t == TK_END) {
      if (depth == 0) {
        if (in_num) { num_buf[nb]='\0'; d.nums[d.num_count++]=atoll(num_buf); in_num=0; nb=0; }
      } else {
        depth--;
        if (depth == 0 && in_num) { num_buf[nb]='\0'; d.nums[d.num_count++]=atoll(num_buf); in_num=0; nb=0; }
      }
      continue;
    }
    if (t == TK_RECORD) break;
    int ctx = depth > 3 ? 3 : depth;
    if (depth == 0) {
      if (!in_num) in_num = 1;
      if (t >= 17 && t <= 25) { num_buf[nb++]='-'; text[tpos++]='-'; }
      if (t >= 0 && t <= 27 && CTX[ctx][t]) { num_buf[nb++]=CTX[ctx][t]; text[tpos++]=CTX[ctx][t]; }
    } else {
      if (t >= 0 && t <= 27 && CTX[ctx] && CTX[ctx][t]) text[tpos++] = CTX[ctx][t];
    }
    if (tpos >= 4095) tpos = 4094;
    if (nb >= 63) nb = 62;
  }
  text[tpos] = '\0';
  d.text = strdup(text);
  return d;
}

/* ── Record encode/decode ────────────────────────────────────────────────── */

static int encode_contact(const char *name, const char *email,
                          const char *phone, const char *company,
                          uint8_t *out, int *pad_out) {
  uint8_t tk[2048]; int n = 0;
  n += enc_word(name, tk + n, 2048 - n);
  n += enc_word("|", tk + n, 2048 - n);   /* pipe = unambiguous field separator */
  n += enc_word(email, tk + n, 2048 - n);
  n += enc_word("|", tk + n, 2048 - n);
  n += enc_word(phone, tk + n, 2048 - n);
  n += enc_word("|", tk + n, 2048 - n);
  n += enc_word(company, tk + n, 2048 - n);
  tk[n++] = TK_RECORD;
  return srv_pack(tk, n, out, pad_out);
}

static int encode_deal(const char *title, long long value,
                       const char *stage, long long contact_id,
                       uint8_t *out, int *pad_out) {
  uint8_t tk[2048]; int n = 0;
  n += enc_word(title, tk + n, 2048 - n);
  n += enc_int(value, tk + n, 2048 - n);
  n += enc_word(stage, tk + n, 2048 - n);
  n += enc_int(contact_id, tk + n, 2048 - n);
  tk[n++] = TK_RECORD;
  return srv_pack(tk, n, out, pad_out);
}

/* ── HTTP helpers ────────────────────────────────────────────────────────── */
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
  const char *r = "HTTP/1.0 404\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  write(fd, r, (int)strlen(r));
}
static void http_400(int fd, const char *msg) {
  char r[512]; int l = snprintf(r, 512,
    "HTTP/1.0 400\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
    (int)strlen(msg), msg);
  write(fd, r, l);
}
static int json_esc(char *b, int pos, const char *s) {
  b[pos++] = '"';
  for (const char *p = s; *p; p++) {
    if (*p == '"' || *p == '\\') b[pos++] = '\\';
    b[pos++] = *p;
  }
  b[pos++] = '"';
  return pos;
}

/* ── JSON body string extractor (handles escaped quotes) ─────────────────── */
static int extract_json_str(char *body, const char *key, char *out, int cap) {
  char search[64]; snprintf(search, 64, "\"%s\"", key);
  char *start = strstr(body, search);
  if (!start) return -1;
  start = strchr(start, ':'); if (!start) return -1;
  start++; while (*start == ' ' || *start == '"') start++;
  char *end = start;
  while (*end) {
    if (*end == '\\' && (end[1] == '"' || end[1] == '\\')) { end += 2; continue; }
    if (*end == '"') break;
    end++;
  }
  if (!*end) return -1;
  int len = 0;
  for (char *p = start; p < end && len < cap-1; ) {
    if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) { out[len++] = p[1]; p += 2; }
    else out[len++] = *p++;
  }
  out[len] = '\0';
  return len;
}

/* ── HTML UI ─────────────────────────────────────────────────────────────── */
static const char *HTML =
"<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"UTF-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1.0\">"
"<title>5bit CRM</title><style>"
"*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',system-ui,sans-serif;background:#0a0a0f;color:#e0e0e0;min-height:100vh;display:flex}"
".sidebar{width:220px;background:#111118;border-right:1px solid #1e1e2e;padding:24px 0;display:flex;flex-direction:column;min-height:100vh}"
".sidebar h2{font-size:16px;padding:0 20px;margin-bottom:24px;letter-spacing:-0.5px}"
".sidebar h2 span{color:#e94560}"
".nav-item{padding:10px 20px;cursor:pointer;font-size:14px;color:#8892b0;transition:all .15s;border-left:2px solid transparent;margin-bottom:2px}"
".nav-item:hover{color:#e0e0e0;background:#1a1a28}"
".nav-item.active{color:#e94560;border-left-color:#e94560;background:#1a1a28}"
".nav-badge{float:right;background:#e9456020;color:#e94560;border-radius:10px;padding:0 8px;font-size:11px}"
".main{flex:1;padding:32px;overflow-y:auto;max-height:100vh}"
".header-row{display:flex;justify-content:space-between;align-items:center;margin-bottom:24px}"
".header-row h1{font-size:24px;font-weight:700;letter-spacing:-0.5px}"
".header-row h1 span{color:#e94560}"
"button{background:#e94560;color:#fff;border:none;border-radius:6px;padding:8px 16px;font-size:13px;font-weight:600;cursor:pointer;transition:background .15s}"
"button:hover{background:#c73652}"
"button.ghost{background:transparent;border:1px solid #2a2a3e;color:#8892b0}"
"button.ghost:hover{background:#1a1a28;color:#e0e0e0}"
"button.danger{background:transparent;border:1px solid #3a1a1a;color:#e94560;padding:4px 10px;font-size:11px}"
"button.danger:hover{background:#3a1a1a}"
"button.small{padding:4px 10px;font-size:11px}"
".card{background:#111118;border:1px solid #1e1e2e;border-radius:8px;padding:20px;margin-bottom:16px}"
".card-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:12px}"
".card-header h3{font-size:15px;font-weight:600}"
".card-row{display:flex;gap:16px;flex-wrap:wrap}"
".card-field{flex:1;min-width:140px}"
".card-field label{display:block;font-size:10px;text-transform:uppercase;letter-spacing:1px;color:#555;margin-bottom:4px}"
".card-field span{font-size:14px}"
".stage{display:inline-block;padding:2px 10px;border-radius:10px;font-size:11px;font-weight:600}"
".stage.lead{background:#1a2a3a;color:#4a9eff}"
".stage.qualified{background:#2a1a3a;color:#b44aff}"
".stage.proposal{background:#3a2a1a;color:#ffaa4a}"
".stage.closed{background:#1a3a2a;color:#4aff8a}"
".stage.lost{background:#3a1a1a;color:#ff4a4a}"
"table{width:100%;border-collapse:collapse}"
"th{text-align:left;font-size:10px;text-transform:uppercase;letter-spacing:1px;color:#555;padding:8px 12px;border-bottom:1px solid #1e1e2e}"
"td{padding:10px 12px;font-size:13px;border-bottom:1px solid #0d0d16}"
"tr:hover td{background:#1a1a28}"
".modal-overlay{display:none;position:fixed;top:0;left:0;right:0;bottom:0;background:#000000aa;z-index:10;align-items:center;justify-content:center}"
".modal{background:#111118;border:1px solid #2a2a3e;border-radius:10px;padding:28px;width:480px;max-width:90vw}"
".modal h3{font-size:18px;margin-bottom:20px}"
".modal label{display:block;font-size:11px;text-transform:uppercase;letter-spacing:1px;color:#555;margin-bottom:4px;margin-top:12px}"
".modal input,.modal select{width:100%;padding:10px 14px;background:#16161f;border:1px solid #2a2a3e;border-radius:6px;color:#e0e0e0;font-size:14px;outline:none}"
".modal input:focus,.modal select:focus{border-color:#e94560}"
".modal select option{background:#111118;color:#e0e0e0}"
".modal-actions{display:flex;gap:8px;justify-content:flex-end;margin-top:20px}"
".stats-row{display:flex;gap:16px;margin-bottom:24px}"
".stat-card{flex:1;background:#111118;border:1px solid #1e1e2e;border-radius:8px;padding:20px;text-align:center}"
".stat-card .num{font-size:32px;font-weight:700;color:#e94560}"
".stat-card .lbl{font-size:11px;color:#555;text-transform:uppercase;letter-spacing:1px;margin-top:4px}"
".empty-state{text-align:center;padding:60px 0;color:#444}"
".empty-state .icon{font-size:40px;margin-bottom:12px}"
".tab{font-size:18px}.tab span{color:#e94560}"
"select{background:#16161f;border:1px solid #2a2a3e;border-radius:6px;color:#e0e0e0;padding:6px 10px;font-size:12px;outline:none}"
"</style></head><body>"
"<div class=\"sidebar\">"
"<h2>5bit <span>◆</span> CRM</h2>"
"<div class=\"nav-item active\" onclick=\"showTab('dashboard')\">Dashboard</div>"
"<div class=\"nav-item\" onclick=\"showTab('contacts')\">Contacts <span class=\"nav-badge\" id=\"contactCount\">0</span></div>"
"<div class=\"nav-item\" onclick=\"showTab('deals')\">Deals <span class=\"nav-badge\" id=\"dealCount\">0</span></div>"
"<div style=\"margin-top:auto;padding:20px;border-top:1px solid #1e1e2e\">"
"<div style=\"font-size:10px;color:#444;text-transform:uppercase;letter-spacing:1px;margin-bottom:8px\">Stack</div>"
"<div style=\"font-size:11px;color:#555\">C binary + libc only</div>"
"<div style=\"font-size:11px;color:#555\">5bit AllocGrid engine</div>"
"<div style=\"font-size:11px;color:#555\">0 npm packages</div>"
"</div></div>"
"<div class=\"main\" id=\"main\"></div>"
"<div class=\"modal-overlay\" id=\"modalOverlay\"><div class=\"modal\" id=\"modal\"></div></div>"
"<script>"
"let contacts=[],deals=[];let currentTab='dashboard';"
"function esc(s){return(s||'').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/\"/g,'&quot;');}"
"async function loadAll(){"
"  const cr=await fetch('/api/contacts');contacts=(await cr.json()).contacts||[];"
"  const dr=await fetch('/api/deals');deals=(await dr.json()).deals||[];"
"  document.getElementById('contactCount').textContent=contacts.length;"
"  document.getElementById('dealCount').textContent=deals.length;"
"  render();"
"}"
"function showTab(t){currentTab=t;"
"  document.querySelectorAll('.nav-item').forEach((el,i)=>{el.classList.toggle('active',i==['dashboard','contacts','deals'].indexOf(t));});"
"  render();}"
"function render(){"
"  let m=document.getElementById('main');"
"  if(currentTab==='dashboard'){"
"    let totalVal=deals.reduce((s,d)=>s+(d.value||0),0);"
"    let won=deals.filter(d=>d.stage==='Closed Won').length;"
"    m.innerHTML="
"      '<div class=\"header-row\"><h1><span>◆</span> Dashboard</h1></div>'+\n"
"      '<div class=\"stats-row\">'+\n"
"      '<div class=\"stat-card\"><div class=\"num\">'+contacts.length+'</div><div class=\"lbl\">Contacts</div></div>'+\n"
"      '<div class=\"stat-card\"><div class=\"num\">'+deals.length+'</div><div class=\"lbl\">Deals</div></div>'+\n"
"      '<div class=\"stat-card\"><div class=\"num\">$'+(totalVal/1000).toFixed(1)+'k</div><div class=\"lbl\">Pipeline Value</div></div>'+\n"
"      '<div class=\"stat-card\"><div class=\"num\">'+won+'</div><div class=\"lbl\">Closed Won</div></div>'+\n"
"      '</div>'+\n"
"      '<div class=\"card\"><div class=\"card-header\"><h3>Recent Deals</h3></div>'+\n"
"      '<table><tr><th>Deal</th><th>Value</th><th>Stage</th></tr>'+\n"
"      deals.slice(-5).reverse().map(d=>'<tr><td>'+esc(d.title)+'</td><td>$'+(d.value||0).toLocaleString()+'</td><td><span class=\"stage '+stageClass(d.stage)+'\">'+esc(d.stage||'')+'</span></td></tr>').join('')+\n"
"      (deals.length===0?'<tr><td colspan=3 class=empty-state>No deals yet</td></tr>':'')+\n"
"      '</table></div>';"
"  }else if(currentTab==='contacts'){"
"    m.innerHTML="
"      '<div class=\"header-row\"><h1><span>◆</span> Contacts</h1>'+\n"
"      '<button onclick=\"showContactModal()\">+ Add Contact</button></div>'+\n"
"      (contacts.length===0?'<div class=\"empty-state\"><div class=\"icon\">👤</div>No contacts yet</div>':"
"      '<table><tr><th>Name</th><th>Email</th><th>Phone</th><th>Company</th><th></th></tr>'+\n"
"      contacts.map(c=>'<tr><td>'+esc(c.name)+'</td><td>'+esc(c.email)+'</td><td>'+esc(c.phone)+'</td><td>'+esc(c.company)+'</td><td><button class=danger onclick=deleteContact('+c._id+')>×</button></td></tr>').join('')+\n"
"      '</table>');"
"  }else if(currentTab==='deals'){"
"    m.innerHTML="
"      '<div class=\"header-row\"><h1><span>◆</span> Deals</h1>'+\n"
"      '<button onclick=\"showDealModal()\">+ Add Deal</button></div>'+\n"
"      (deals.length===0?'<div class=\"empty-state\"><div class=\"icon\">💼</div>No deals yet</div>':"
"      '<table><tr><th>Deal</th><th>Value</th><th>Stage</th><th>Contact</th><th></th></tr>'+\n"
"      deals.map(d=>{let c=contacts.find(x=>x._id===d.contact_id);return '<tr><td>'+esc(d.title)+'</td><td>$'+(d.value||0).toLocaleString()+'</td><td><span class=\"stage '+stageClass(d.stage)+'\">'+esc(d.stage||'')+'</span></td><td>'+(c?esc(c.name):'—')+'</td><td><select onchange=moveDeal('+d._id+',this.value)><option value=\"\">Move…</option><option>Lead</option><option>Qualified</option><option>Proposal</option><option>Closed Won</option><option>Closed Lost</option></select></td></tr>';}).join('')+\n"
"      '</table>');"
"  }"
"}"
"function stageClass(s){return{'Lead':'lead','Qualified':'qualified','Proposal':'proposal','Closed Won':'closed','Closed Lost':'lost'}[s]||'';}"
"function showContactModal(){"
"  document.getElementById('modal').innerHTML="
"    '<h3>New Contact</h3>'+\n"
"    '<label>Name</label><input id=fName>'+\n"
"    '<label>Email</label><input id=fEmail>'+\n"
"    '<label>Phone</label><input id=fPhone>'+\n"
"    '<label>Company</label><input id=fCompany>'+\n"
"    '<div class=modal-actions><button class=ghost onclick=closeModal()>Cancel</button><button onclick=saveContact()>Save</button></div>';"
"  document.getElementById('modalOverlay').style.display='flex';"
"}"
"function showDealModal(){"
"  let copts=contacts.map(c=>'<option value='+c._id+'>'+esc(c.name)+'</option>').join('');"
"  document.getElementById('modal').innerHTML="
"    '<h3>New Deal</h3>'+\n"
"    '<label>Title</label><input id=fTitle>'+\n"
"    '<label>Value ($)</label><input id=fValue type=number>'+\n"
"    '<label>Contact</label><select id=fContact>'+copts+'</select>'+\n"
"    '<div class=modal-actions><button class=ghost onclick=closeModal()>Cancel</button><button onclick=saveDeal()>Save</button></div>';"
"  document.getElementById('modalOverlay').style.display='flex';"
"}"
"function closeModal(){document.getElementById('modalOverlay').style.display='none';}"
"async function saveContact(){"
"  let d={name:document.getElementById('fName').value,email:document.getElementById('fEmail').value,phone:document.getElementById('fPhone').value,company:document.getElementById('fCompany').value};"
"  await fetch('/api/contacts',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)});"
"  closeModal();loadAll();"
"}"
"async function saveDeal(){"
"  let d={title:document.getElementById('fTitle').value,value:parseInt(document.getElementById('fValue').value)||0,contact_id:parseInt(document.getElementById('fContact').value)||0};"
"  await fetch('/api/deals',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)});"
"  closeModal();loadAll();"
"}"
"async function deleteContact(id){await fetch('/api/contacts/'+id,{method:'DELETE'});loadAll();}"
"async function deleteDeal(id){await fetch('/api/deals/'+id,{method:'DELETE'});loadAll();}"
"async function moveDeal(id,stage){if(!stage)return;await fetch('/api/deals/'+id,{method:'PATCH',headers:{'Content-Type':'application/json'},body:JSON.stringify({stage:stage})});loadAll();}"
"loadAll();"
"</script></body></html>";

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static void make_path(char *out, const char *base, const char *sub) {
  snprintf(out, 512, "%s/%s", base, sub);
}

/* ── Request handler ─────────────────────────────────────────────────────── */
static void handle(int fd, const char *data_dir) {
  char buf[8192]; int n = (int)read(fd, buf, sizeof(buf)-1);
  if (n <= 0) { close(fd); return; }
  buf[n] = '\0';
  char method[16]={0}, path[256]={0};
  sscanf(buf, "%15s %255s", method, path);
  char dec[256]; int dp=0;
  for (int i=0; path[i]&&dp<255; i++) {
    if (path[i]=='%'&&path[i+1]&&path[i+2]) { int hi,lo; sscanf(path+i+1,"%1x%1x",&hi,&lo); dec[dp++]=(char)((hi<<4)|lo); i+=2; }
    else dec[dp++]=path[i];
  }
  dec[dp]='\0';
  char *body = strstr(buf, "\r\n\r\n"); if (body) body += 4;

  /* Route: / */
  if (strcmp(method,"GET")==0 && strcmp(dec,"/")==0) {
    http_ok(fd, "text/html; charset=utf-8", HTML, (int)strlen(HTML)); close(fd); return;
  }

  /* Build table subdirectory paths */
  char contacts_dir[512], deals_dir[512];
  make_path(contacts_dir, data_dir, "contacts");
  make_path(deals_dir, data_dir, "deals");

  /* ── CONTACTS ────────────────────────────────────────────────────────── */

  /* GET /api/contacts */
  if (strcmp(method,"GET")==0 && strcmp(dec,"/api/contacts")==0) {
    char json[65536]; int pos=snprintf(json,65536,"{\"contacts\":[");
    int total=grid_total_entries(contacts_dir), first=1;
    /* Contacts stored at even IDs (0,2,4,...) */
    for (int i=0; i<total; i++) {
      GridRecord *rec=grid_read(contacts_dir,i);
      if (!rec||rec->is_tombstone) { if(rec){free(rec->parsed);free(rec);} continue; }
      int nbytes=(rec->bit_length+7)/8;
      Decoded d=decode_record(rec->parsed,nbytes);
      if (d.text && strlen(d.text)>0) {
        /* Contact fields are separated by pipe '|' characters */
        char *text=d.text;
        char fields[4][512]; int fc=0;
        char *p=text;
        for (int fi=0; fi<4; fi++) {
          char *pipe=strchr(p, '|');
          int flen = pipe ? (int)(pipe-p) : (int)strlen(p);
          if (flen>511) flen=511;
          memcpy(fields[fc], p, flen); fields[fc][flen]='\0'; fc++;
          if (!pipe) break;
          p = pipe+1;
        }
        if (fc>=1 && strlen(fields[0])>0) {
          if (!first) json[pos++]=',';
          pos+=snprintf(json+pos,65536-pos,"{\"_id\":%d,\"name\":",rec->record_id);
          pos=json_esc(json,pos,fields[0]);
          pos+=snprintf(json+pos,65536-pos,",\"email\":");
          pos=json_esc(json,pos,fc>1?fields[1]:"");
          pos+=snprintf(json+pos,65536-pos,",\"phone\":");
          pos=json_esc(json,pos,fc>2?fields[2]:"");
          pos+=snprintf(json+pos,65536-pos,",\"company\":");
          pos=json_esc(json,pos,fc>3?fields[3]:"");
          pos+=snprintf(json+pos,65536-pos,"}");
          first=0;
        }
      }
      free(d.text); free(rec->parsed); free(rec);
    }
    pos+=snprintf(json+pos,65536-pos,"]}");
    http_ok(fd,"application/json",json,pos); close(fd); return;
  }

  /* POST /api/contacts */
  if (strcmp(method,"POST")==0 && strcmp(dec,"/api/contacts")==0) {
    char name[256]={0}, email[256]={0}, phone[256]={0}, company[256]={0};
    extract_json_str(body,"name",name,256);
    extract_json_str(body,"email",email,256);
    extract_json_str(body,"phone",phone,256);
    extract_json_str(body,"company",company,256);
    if (!name[0]) { http_400(fd,"name required"); close(fd); return; }
    int total=grid_total_entries(contacts_dir), rid=total;
    for (int i=0;i<total;i++) {
      GridRecord *r=grid_read(contacts_dir,i);
      if (!r||r->is_tombstone) { rid=i; if(r){free(r->parsed);free(r);} break; }
      if(r){free(r->parsed);free(r);}
    }
    uint8_t packed[4096]; int pad;
    int plen=encode_contact(name,email,phone,company,packed,&pad);
    if (plen<0) { http_400(fd,"encode error"); close(fd); return; }
    grid_write(contacts_dir,rid,packed,plen,plen*8-pad);
    char json[4096]; int pos=0;
    pos+=snprintf(json+pos,4096-pos,"{\"_id\":%d,\"name\":",rid);
    pos=json_esc(json,pos,name);
    pos+=snprintf(json+pos,4096-pos,",\"email\":");
    pos=json_esc(json,pos,email);
    pos+=snprintf(json+pos,4096-pos,",\"phone\":");
    pos=json_esc(json,pos,phone);
    pos+=snprintf(json+pos,4096-pos,",\"company\":");
    pos=json_esc(json,pos,company);
    pos+=snprintf(json+pos,4096-pos,"}");
    http_created(fd,json,pos); close(fd); return;
  }

  /* DELETE /api/contacts/:id */
  if (strcmp(method,"DELETE")==0 && strncmp(dec,"/api/contacts/",14)==0) {
    int rid=atoi(dec+14);
    grid_delete(contacts_dir,rid);
    char json[64]; int pos=snprintf(json,64,"{\"deleted\":%d}",rid);
    http_ok(fd,"application/json",json,pos); close(fd); return;
  }

  /* ── DEALS ────────────────────────────────────────────────────────────── */

  /* GET /api/deals */
  if (strcmp(method,"GET")==0 && strcmp(dec,"/api/deals")==0) {
    char json[65536]; int pos=snprintf(json,65536,"{\"deals\":[");
    int total=grid_total_entries(deals_dir), first=1;
    for (int i=0; i<total; i++) {
      GridRecord *rec=grid_read(deals_dir,i);
      if (!rec||rec->is_tombstone) { if(rec){free(rec->parsed);free(rec);} continue; }
      int nbytes=(rec->bit_length+7)/8;
      Decoded d=decode_record(rec->parsed,nbytes);
      /* Deal: WORD(title) NUM(value) WORD(stage) NUM(contact_id)
       * d.text has interleaved text+nums. d.nums has number values.
       * Fields: title (text before first num), value (first num), stage (text between nums), contact_id (last num) */
      if (d.num_count>=2) {
        /* Find stage text between first and last num */
        char title[512]={0}, stage[256]={0};
        /* Reconstruct: text = title_text + value_digits + stage_text + contact_digits */
        char *t=d.text;
        /* Skip title (text before digits of first num) */
        char *p=t;
        while (*p && (*p<'0'||*p>'9') && *p!='-') p++;
        int tlen=(int)(p-t); if(tlen>511)tlen=511;
        memcpy(title,t,tlen); title[tlen]='\0';
        /* Skip value digits */
        while (*p && ((*p>='0'&&*p<='9')||*p=='-')) p++;
        /* Stage text (between value and contact_id digits) */
        char *stage_start=p;
        while (*p && (*p<'0'||*p>'9') && *p!='-') p++;
        int slen=(int)(p-stage_start); if(slen>255)slen=255;
        memcpy(stage,stage_start,slen); stage[slen]='\0';

        if (strlen(title)>0 || strlen(stage)>0) {
          if (!first) json[pos++]=',';
          pos+=snprintf(json+pos,65536-pos,"{\"_id\":%d,\"title\":",rec->record_id);
          pos=json_esc(json,pos,strlen(title)>0?title:"Untitled");
          pos+=snprintf(json+pos,65536-pos,",\"value\":%lld,\"stage\":",d.nums[0]);
          pos=json_esc(json,pos,strlen(stage)>0?stage:"Lead");
          pos+=snprintf(json+pos,65536-pos,",\"contact_id\":%lld}",d.nums[1]);
          first=0;
        }
      }
      free(d.text); free(rec->parsed); free(rec);
    }
    pos+=snprintf(json+pos,65536-pos,"]}");
    http_ok(fd,"application/json",json,pos); close(fd); return;
  }

  /* POST /api/deals */
  if (strcmp(method,"POST")==0 && strcmp(dec,"/api/deals")==0) {
    char title[256]={0}, stage[256]={0};
    extract_json_str(body,"title",title,256);
    extract_json_str(body,"stage",stage,256);
    /* Extract numeric fields */
    long long value=0, contact_id=0;
    char *vstart=strstr(body,"\"value\""); if(vstart){vstart=strchr(vstart,':'); if(vstart)value=atoll(vstart+1);}
    char *cstart=strstr(body,"\"contact_id\""); if(cstart){cstart=strchr(cstart,':'); if(cstart)contact_id=atoll(cstart+1);}
    if (!title[0]) { http_400(fd,"title required"); close(fd); return; }
    if (!stage[0]) strcpy(stage,"Lead");
    int total=grid_total_entries(deals_dir), rid=total;
    for (int i=0;i<total;i++) {
      GridRecord *r=grid_read(deals_dir,i);
      if (!r||r->is_tombstone) { rid=i; if(r){free(r->parsed);free(r);} break; }
      if(r){free(r->parsed);free(r);}
    }
    uint8_t packed[4096]; int pad;
    int plen=encode_deal(title,value,stage,contact_id,packed,&pad);
    if (plen<0) { http_400(fd,"encode error"); close(fd); return; }
    grid_write(deals_dir,rid,packed,plen,plen*8-pad);
    char json[4096]; int pos=0;
    pos+=snprintf(json+pos,4096-pos,"{\"_id\":%d,\"title\":",rid);
    pos=json_esc(json,pos,title);
    pos+=snprintf(json+pos,4096-pos,",\"value\":%lld,\"stage\":",value);
    pos=json_esc(json,pos,stage);
    pos+=snprintf(json+pos,4096-pos,",\"contact_id\":%lld}",contact_id);
    http_created(fd,json,pos); close(fd); return;
  }

  /* PATCH /api/deals/:id */
  if (strcmp(method,"PATCH")==0 && strncmp(dec,"/api/deals/",11)==0) {
    int rid=atoi(dec+11);
    char stage[256]={0}; extract_json_str(body,"stage",stage,256);
    if (!stage[0]) { http_400(fd,"stage required"); close(fd); return; }
    GridRecord *rec=grid_read(deals_dir,rid);
    if (!rec||rec->is_tombstone) { http_404(fd); if(rec){free(rec->parsed);free(rec);} close(fd); return; }
    int nbytes=(rec->bit_length+7)/8;
    Decoded d=decode_record(rec->parsed,nbytes);
    /* Reconstruct title from text before first num */
    char title[512]={0}; char *p=d.text;
    while (*p && (*p<'0'||*p>'9') && *p!='-') p++;
    int tlen=(int)(p-d.text); if(tlen>511)tlen=511;
    memcpy(title,d.text,tlen); title[tlen]='\0';
    long long value=d.num_count>=1?d.nums[0]:0;
    long long contact_id=d.num_count>=2?d.nums[1]:0;
    uint8_t packed[4096]; int pad;
    int plen=encode_deal(title,value,stage,contact_id,packed,&pad);
    grid_write(deals_dir,rid,packed,plen,plen*8-pad);
    char json[2048]; int pos=0;
    pos+=snprintf(json+pos,2048-pos,"{\"_id\":%d,\"title\":",rid);
    pos=json_esc(json,pos,title);
    pos+=snprintf(json+pos,2048-pos,",\"value\":%lld,\"stage\":",value);
    pos=json_esc(json,pos,stage);
    pos+=snprintf(json+pos,2048-pos,",\"contact_id\":%lld}",contact_id);
    http_ok(fd,"application/json",json,pos);
    free(d.text); free(rec->parsed); free(rec); close(fd); return;
  }

  /* DELETE /api/deals/:id */
  if (strcmp(method,"DELETE")==0 && strncmp(dec,"/api/deals/",11)==0) {
    int rid=atoi(dec+11);
    grid_delete(deals_dir,rid);
    char json[64]; int pos=snprintf(json,64,"{\"deleted\":%d}",rid);
    http_ok(fd,"application/json",json,pos); close(fd); return;
  }

  http_404(fd); close(fd);
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
  int port = argc > 1 ? atoi(argv[1]) : 8085;
  const char *data_dir = argc > 2 ? argv[2] : "./data_crm";
  char cdir[512], ddir[512];
  snprintf(cdir, 512, "%s/contacts", data_dir);
  snprintf(ddir, 512, "%s/deals", data_dir);
  mkdir(data_dir, 0755);
  mkdir(cdir, 0755);
  mkdir(ddir, 0755);
  grid_init(cdir);
  grid_init(ddir);
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) { perror("socket"); return 1; }
  int opt = 1; setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons((uint16_t)port);
  if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); close(sock); return 1; }
  if (listen(sock, 10) < 0) { perror("listen"); close(sock); return 1; }
  printf("┌─────────────────────────────────────────────┐\n");
  printf("│  5bit CRM — C Binary, Zero Dependencies      │\n");
  printf("│  Contacts + Deals + Dashboard                 │\n");
  printf("│  Stack: libc + fivebit_grid = everything      │\n");
  printf("│  No MongoDB. No SQLite. No Node.js.           │\n");
  printf("└─────────────────────────────────────────────┘\n");
  printf("  →  http://localhost:%d\n\n", port);
  while (1) {
    struct sockaddr_in client; socklen_t clen = sizeof(client);
    int cfd = accept(sock, (struct sockaddr*)&client, &clen);
    if (cfd < 0) { perror("accept"); continue; }
    handle(cfd, data_dir);
  }
  close(sock); return 0;
}
