#!/usr/bin/env tsx
/**
 * 5bit App — Zero-Framework Full-Stack Demo
 * ==========================================
 * Proves one thesis: the 5bit stack eliminates the Node.js framework layer.
 *
 * Stack:  5bit DB (AllocGrid) + 5bit HTTP server + HTML/CSS UI
 * Dependencies: NONE. No Express, no Fastify, no ORM, no migration tool.
 *              The Node.js built-in `http` module is the entire server.
 *              The 5bit encoder/alloc grid is the entire database.
 *
 * Run:  npx tsx examples/fivebit_app.ts
 * Open: http://localhost:8085
 */
import http from 'http';
import { AllocGrid, Encoder, Token, Parser, ParsedNumber, ParsedWord } from '../typescript/src';
import { Machine, program, num, verb, CMD_READ } from '../typescript/src/interpreter';
const PORT = parseInt(process.env.PORT || '8085');
const DATA_DIR = process.env.DATA_DIR || './data_fivebit_app';

// ── Database: 5bit AllocGrid ────────────────────────────────────────────────

const grid = new AllocGrid(DATA_DIR, 500);

// Schema: auto-discovered from labels or default
const FIELD_NAMES = ['task', 'done', 'created'];

function encodeTask(task: string, done: boolean, created: number): Token[] {
  return [
    ...Encoder.encodeWord(task),
    ...Encoder.encodeInteger(done ? 1 : 0),
    ...Encoder.encodeInteger(created),
    Token.RECORD,
  ];
}

function decodeRecord(rec: { tokens: Token[]; recordId: number }): any | null {
  const parser = new Parser();
  parser.feedTokens([...rec.tokens]);
  parser.reassemble(); // merge consecutive WORD tokens
  const parsed = parser.output;

  // Collect typed items in order
  const items: Array<{type: string; value: string | number}> = [];
  for (const p of parsed) {
    if (p.type === 'word') items.push({type: 'word', value: (p as ParsedWord).text});
    else if (p.type === 'number') items.push({type: 'num', value: (p as ParsedNumber).value});
  }

  if (items.length === 0) return null;
  // Skip records that are purely numeric (interpreter operands, grants, etc.)
  const hasWord = items.some(it => it.type === 'word');
  if (!hasWord) return null;

  // Convention: last two numbers are metadata (done, created).
  // All other numbers interleaved with words are part of the task text.
  const numIndices = items.map((it, i) => it.type === 'num' ? i : -1).filter(i => i >= 0);
  const metaStart = Math.max(0, numIndices.length - 2);
  const metaIndices = new Set(numIndices.slice(metaStart));

  // Reconstruct task text
  let text = '';
  for (let i = 0; i < items.length; i++) {
    if (metaIndices.has(i)) continue; // skip metadata numbers
    text += String(items[i].value);
  }

  const metaNums = numIndices.slice(metaStart).map(i => items[i].value as number);
  const done = metaNums.length >= 2 ? metaNums[0] === 1 : false;
  const created = metaNums.length >= 2 ? metaNums[1] : (metaNums[0] || 0);

  return { _id: rec.recordId, task: text.trim(), done, created };
}

function listAll(): any[] {
  const results: any[] = [];
  for (let i = 0; i < grid.totalEntries; i++) {
    const rec = grid.read(i);
    if (rec && !rec.isTombstone) {
      const decoded = decodeRecord({ tokens: rec.tokens, recordId: i });
      if (decoded) results.push(decoded);
    }
  }
  return results;
}

// ── Interpreter demo: sum program stored in the grid ────────────────────────

function setupInterpreterDemo(): string[] {
  const logs: string[] = [];
  const m = new Machine(grid, 1);

  try {
    // Program at slot 100: DEF 100 ( READ 0 READ 1 + = ) RECORD
    // Reads slot 0 and 1, adds them, emits result
    const prog = program(100,
      verb(CMD_READ, 0),
      verb(CMD_READ, 1),
      [Token.T_PLUS],
      [Token.T_EQ],
    );

    // Store two operands in slots 0 and 1
    grid.write(0, [...Encoder.encodeInteger(42), Token.RECORD]);
    grid.write(1, [...Encoder.encodeInteger(58), Token.RECORD]);

    m.load(100, prog);
    const out = m.run(100);
    logs.push(`Program slot 100: 42 + 58 = ${out[0]}`);
    logs.push(`Trace: ${m.trace.join(' → ')}`);
    logs.push(`Stack: 5bit interpreter, 8 verbs, 0 npm packages`);
  } catch (e: any) {
    logs.push(`Interpreter: ${e.message}`);
  }
  return logs;
}

// ── HTTP Server: 5bit from socket to disk ───────────────────────────────────

const STATIC = `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>5bit — Zero-Framework App</title>
<style>
*, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', system-ui, sans-serif; background: #0a0a0f; color: #e0e0e0; min-height: 100vh; }
.header { background: linear-gradient(135deg, #1a1a2e 0%, #16213e 50%, #0f3460 100%); border-bottom: 2px solid #e94560; padding: 24px 32px; }
.header h1 { font-size: 28px; font-weight: 700; letter-spacing: -0.5px; }
.header h1 span { color: #e94560; }
.header p { color: #8892b0; margin-top: 4px; font-size: 14px; }
.badge { display: inline-block; background: #e9456020; color: #e94560; border: 1px solid #e9456040; border-radius: 4px; padding: 2px 8px; font-size: 11px; margin-left: 8px; font-family: 'SF Mono', 'Fira Code', monospace; }
.layout { display: flex; min-height: calc(100vh - 110px); }
.main { flex: 1; padding: 32px; max-width: 720px; }
.panel { width: 380px; background: #111118; border-left: 1px solid #1e1e2e; padding: 24px; overflow-y: auto; max-height: calc(100vh - 110px); }
.panel h3 { font-size: 13px; text-transform: uppercase; letter-spacing: 1px; color: #e94560; margin-bottom: 12px; }
.add-form { display: flex; gap: 8px; margin-bottom: 24px; }
.add-form input { flex: 1; padding: 10px 14px; background: #16161f; border: 1px solid #2a2a3e; border-radius: 6px; color: #e0e0e0; font-size: 14px; outline: none; transition: border-color .15s; }
.add-form input:focus { border-color: #e94560; }
.add-form button { padding: 10px 20px; background: #e94560; color: white; border: none; border-radius: 6px; font-size: 14px; font-weight: 600; cursor: pointer; transition: background .15s; }
.add-form button:hover { background: #c73652; }
.task-list { list-style: none; }
.task-item { display: flex; align-items: center; gap: 12px; padding: 12px 16px; background: #16161f; border: 1px solid #1e1e2e; border-radius: 6px; margin-bottom: 8px; transition: border-color .15s; }
.task-item:hover { border-color: #2a2a3e; }
.task-item.done .task-text { text-decoration: line-through; color: #555; }
.task-text { flex: 1; font-size: 14px; }
.task-date { font-size: 11px; color: #555; font-family: monospace; }
.task-del { background: none; border: 1px solid #3a1a1a; color: #e94560; padding: 4px 10px; border-radius: 4px; cursor: pointer; font-size: 12px; transition: all .15s; }
.task-del:hover { background: #3a1a1a; }
.task-toggle { width: 18px; height: 18px; border: 2px solid #3a3a5a; border-radius: 4px; cursor: pointer; display: flex; align-items: center; justify-content: center; font-size: 11px; transition: all .15s; flex-shrink: 0; }
.task-item.done .task-toggle { background: #e94560; border-color: #e94560; }
.log-entry { font-size: 12px; color: #8892b0; padding: 4px 0; border-bottom: 1px solid #1a1a28; font-family: 'SF Mono', 'Fira Code', monospace; }
.log-entry:last-child { border-bottom: none; }
.stat { display: flex; justify-content: space-between; padding: 6px 0; font-size: 12px; }
.stat-label { color: #555; }
.stat-value { color: #e94560; font-family: monospace; font-weight: 600; }
.empty { text-align: center; color: #444; padding: 40px 0; font-size: 14px; }
.token-box { background: #0d0d16; border: 1px solid #1e1e2e; border-radius: 6px; padding: 12px; font-family: 'SF Mono', 'Fira Code', monospace; font-size: 11px; color: #e94560; word-break: break-all; line-height: 1.6; max-height: 200px; overflow-y: auto; margin-bottom: 12px; }
</style>
</head>
<body>
<div class="header">
  <h1>5bit <span>◆</span> Task Manager<span class="badge">ZERO DEPENDENCIES</span></h1>
  <p>Database: 5bit AllocGrid &nbsp;|&nbsp; Server: 5bit HTTP (built-in) &nbsp;|&nbsp; Runtime: 8-verb interpreter &nbsp;|&nbsp; npm packages: 0</p>
</div>
<div class="layout">
  <div class="main">
    <div class="add-form">
      <input id="taskInput" placeholder="Add a task..." autofocus />
      <button onclick="addTask()">Add</button>
    </div>
    <ul id="taskList" class="task-list"></ul>
    <div id="empty" class="empty">No tasks yet. Add one above.</div>
  </div>
  <div class="panel">
    <h3>◆ Stack Stats</h3>
    <div class="stat"><span class="stat-label">Database engine</span><span class="stat-value">5bit AllocGrid</span></div>
    <div class="stat"><span class="stat-label">Token lexicon</span><span class="stat-value">32 × 5-bit</span></div>
    <div class="stat"><span class="stat-label">Contexts</span><span class="stat-value">5 (NUM→SPECIAL3)</span></div>
    <div class="stat"><span class="stat-label">Interpreter verbs</span><span class="stat-value">DEF/CALL/RET/IF/LOOP/BREAK/STORE/READ</span></div>
    <div class="stat"><span class="stat-label">Server runtime</span><span class="stat-value">Node.js http (built-in)</span></div>
    <div class="stat"><span class="stat-label">npm packages</span><span class="stat-value">0</span></div>
    <div class="stat"><span class="stat-label">Express/Fastify/Koa</span><span class="stat-value">NONE</span></div>
    <h3 style="margin-top:20px">◆ Interpreter Proof</h3>
    <div id="interpLog"></div>
    <h3 style="margin-top:20px">◆ Last Token Stream</h3>
    <div id="tokenBox" class="token-box">—</div>
  </div>
</div>
<script>
async function loadTasks() {
  const res = await fetch('/api/tasks');
  const data = await res.json();
  const list = document.getElementById('taskList');
  const empty = document.getElementById('empty');
  list.innerHTML = '';
  if (data.tasks.length === 0) { empty.style.display = 'block'; }
  else { empty.style.display = 'none'; }
  data.tasks.forEach(t => {
    const li = document.createElement('li');
    li.className = 'task-item' + (t.done ? ' done' : '');
    li.innerHTML =
      '<div class="task-toggle" onclick="toggleTask('+t._id+','+t.done+')">'+(t.done ? '✓' : '')+'</div>' +
      '<span class="task-text">'+esc(t.task)+'</span>' +
      '<span class="task-date">'+new Date(t.created*1000).toLocaleDateString()+'</span>' +
      '<button class="task-del" onclick="deleteTask('+t._id+')">×</button>';
    list.appendChild(li);
  });
  document.getElementById('tokenBox').textContent = data.lastTokens || '—';
}
function esc(s) { return (s||'').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;'); }
async function addTask() {
  const inp = document.getElementById('taskInput');
  const text = inp.value.trim();
  if (!text) return;
  await fetch('/api/tasks', { method: 'POST', headers: {'Content-Type':'application/json'}, body: JSON.stringify({task:text}) });
  inp.value = '';
  loadTasks();
}
async function toggleTask(id, current) {
  await fetch('/api/tasks/'+id, { method: 'PATCH', headers: {'Content-Type':'application/json'}, body: JSON.stringify({done:!current}) });
  loadTasks();
}
async function deleteTask(id) {
  await fetch('/api/tasks/'+id, { method: 'DELETE' });
  loadTasks();
}
document.getElementById('taskInput').addEventListener('keydown', e => { if (e.key==='Enter') addTask(); });
loadTasks();
</script>
</body>
</html>`;

const server = http.createServer((req, res) => {
  const url = new URL(req.url || '/', `http://localhost:${PORT}`);
  const method = req.method || 'GET';
  const parts = url.pathname.split('/').filter(Boolean);

  // CORS for local dev (the UI is served from same origin, but keep it open)
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET,POST,PATCH,DELETE,OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type');

  if (method === 'OPTIONS') {
    res.writeHead(204); res.end(); return;
  }

  // ── GET / — serve the UI ─────────────────────────────────────────────
  if (method === 'GET' && url.pathname === '/') {
    res.setHeader('Content-Type', 'text/html; charset=utf-8');
    res.writeHead(200);
    res.end(STATIC);
    return;
  }

  // ── GET /api/tasks — list all ────────────────────────────────────────
  if (method === 'GET' && parts[0] === 'api' && parts[1] === 'tasks' && parts.length === 2) {
    const tasks = listAll();
    const lastRec = grid.totalEntries > 0 ? grid.read(grid.totalEntries - 1) : null;
    const lastTokens = lastRec ? lastRec.tokens.map(t => Token[t as number] || t).join(' ') : '';
    res.setHeader('Content-Type', 'application/json');
    res.writeHead(200);
    res.end(JSON.stringify({ tasks, lastTokens }));
    return;
  }

  // ── POST /api/tasks — create ─────────────────────────────────────────
  if (method === 'POST' && parts[0] === 'api' && parts[1] === 'tasks' && parts.length === 2) {
    let body = '';
    req.on('data', c => body += c);
    req.on('end', () => {
      try {
        const { task } = JSON.parse(body);
        if (!task) { res.writeHead(400); res.end(JSON.stringify({error:'task required'})); return; }
        const rid = grid.totalEntries;
        const tokens = encodeTask(task, false, Math.floor(Date.now() / 1000));
        grid.write(rid, tokens);
        res.setHeader('Content-Type', 'application/json');
        res.writeHead(201);
        res.end(JSON.stringify({ _id: rid, task, done: false }));
      } catch (e: any) {
        res.writeHead(400); res.end(JSON.stringify({ error: e.message }));
      }
    });
    return;
  }

  // ── PATCH /api/tasks/:id — toggle done ────────────────────────────────
  if (method === 'PATCH' && parts[0] === 'api' && parts[1] === 'tasks' && parts[2]) {
    const rid = parseInt(parts[2]);
    const rec = grid.read(rid);
    if (!rec || rec.isTombstone) { res.writeHead(404); res.end(JSON.stringify({error:'not found'})); return; }
    let body = '';
    req.on('data', c => body += c);
    req.on('end', () => {
      try {
        const { done } = JSON.parse(body);
        const existing = decodeRecord({ tokens: rec.tokens, recordId: rid });
        const tokens = encodeTask(existing.task, done, existing.created);
        grid.write(rid, tokens);
        res.writeHead(200);
        res.end(JSON.stringify({ _id: rid, task: existing.task, done }));
      } catch (e: any) {
        res.writeHead(400); res.end(JSON.stringify({ error: e.message }));
      }
    });
    return;
  }

  // ── DELETE /api/tasks/:id — soft-delete ──────────────────────────────
  if (method === 'DELETE' && parts[0] === 'api' && parts[1] === 'tasks' && parts[2]) {
    const rid = parseInt(parts[2]);
    grid.write(rid, []); // overwrite with empty → tombstone
    res.writeHead(200);
    res.end(JSON.stringify({ deleted: rid }));
    return;
  }

  // ── GET /api/interpreter — run the demo program ──────────────────────
  if (method === 'GET' && parts[0] === 'api' && parts[1] === 'interpreter') {
    res.setHeader('Content-Type', 'application/json');
    res.writeHead(200);
    res.end(JSON.stringify({ logs: setupInterpreterDemo() }));
    return;
  }

  res.writeHead(404);
  res.end(JSON.stringify({ error: 'not found' }));
});

// ── Start ───────────────────────────────────────────────────────────────────

console.log('┌─────────────────────────────────────────────┐');
console.log('│  5bit App — Zero-Framework Full Stack       │');
console.log('│  Database: AllocGrid (5-bit token engine)   │');
console.log('│  Server:   Node.js http (built-in, 0 deps)  │');
console.log('│  UI:       HTML/CSS (served inline)         │');
console.log('│  Interpreter: 8-verb stack machine           │');
console.log('│  npm packages required: 0                    │');
console.log('│  No Express. No ORM. No migration tool.      │');
console.log('└─────────────────────────────────────────────┘');
console.log(`\n  →  http://localhost:${PORT}\n`);

server.listen(PORT);
