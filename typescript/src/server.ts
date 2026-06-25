/**
 * 5bit API Server — TypeScript/Node.js
 * ======================================
 * Same REST endpoints as Python server. Same 5bit engine underneath.
 * Run: npx tsx typescript/src/server.ts --port 8080
 */
import http from 'http';
import { AllocGrid, Encoder, Token, Parser, ParsedNumber, ParsedWord, packToBytes } from './index';
import crypto from 'crypto';

const PORT = parseInt(process.env.PORT || '8080');
const DATA_DIR = process.env.DATA_DIR || './data';

const grid = new AllocGrid(DATA_DIR, 1000); // 1000-entry LRU cache
const etagCache = new Map<number, string>();
const spec = { name: 'records', fields: ['value'] };

function recordToJson(rec: any): any {
  const result: any = {};
  const vals: any[] = []; let pending = '';
  for (const p of rec.parsed) {
    if (p.type === 'number') { if (pending) { vals.push(pending); pending = ''; } vals.push(p.value); }
    else if (p.type === 'word') pending += p.text;
  }
  if (pending) vals.push(pending);
  spec.fields.forEach((f, i) => { if (i < vals.length) result[f] = vals[i]; });
  result._id = rec.recordId;
  result._hash = crypto.createHash('sha256').update(Buffer.from(packToBytes(rec.tokens)[0] as any)).digest('hex').slice(0, 16);
  return result;
}

function send(res: http.ServerResponse, code: number, body: any, etag?: string) {
  const data = JSON.stringify(body);
  if (etag) {
    res.setHeader('ETag', `"${etag}"`);
    etagCache.set(parseInt(res.req?.url?.split('/')[2] || '0'), etag);
  }
  res.setHeader('Content-Type', 'application/json');
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.writeHead(code);
  res.end(data);
}

const server = http.createServer((req, res) => {
  const url = new URL(req.url || '/', `http://localhost:${PORT}`);
  const path = url.pathname;
  const method = req.method || 'GET';

  // GET /records/<id>
  const parts = path.split('/').filter(Boolean);
  if (method === 'GET' && parts[0] === 'records' && parts[1]) {
    const rid = parseInt(parts[1]);
    const rec = grid.read(rid);
    if (!rec || rec.isTombstone) return send(res, 404, { error: 'Not found' });
    const hash = crypto.createHash('sha256').update(Buffer.from(packToBytes(rec.tokens)[0] as any)).digest('hex').slice(0, 16);
    return send(res, 200, recordToJson(rec), hash);
  }

  // POST /records
  if (method === 'POST' && path === '/records') {
    let body = '';
    req.on('data', c => body += c);
    req.on('end', () => {
      try {
        const data = JSON.parse(body);
        const tokens: Token[] = [];
        spec.fields.forEach(f => {
          const v = data[f];
          if (typeof v === 'number') tokens.push(...Encoder.encodeInteger(v));
          else if (typeof v === 'string') tokens.push(...Encoder.encodeWord(v));
        });
        tokens.push(Token.RECORD);
        const rid = grid.totalEntries;
        grid.write(rid, tokens);
        const rec = grid.read(rid);
        if (rec) return send(res, 201, recordToJson(rec));
      } catch (e: any) { return send(res, 400, { error: e.message }); }
    });
    return;
  }

  // GET /records (list)
  if (method === 'GET' && path === '/records') {
    const results: any[] = [];
    for (let rid = 0; rid < Math.min(grid.totalEntries, 100); rid++) {
      const rec = grid.read(rid);
      if (rec && !rec.isTombstone) results.push(recordToJson(rec));
    }
    return send(res, 200, { results, count: results.length });
  }

  // PUT /records/<id>
  if (method === 'PUT' && parts[0] === 'records' && parts[1]) {
    const rid = parseInt(parts[1]);
    let body = '';
    req.on('data', c => body += c);
    req.on('end', () => {
      try {
        const data = JSON.parse(body);
        const tokens: Token[] = [];
        spec.fields.forEach(f => {
          const v = data[f];
          if (typeof v === 'number') tokens.push(...Encoder.encodeInteger(v));
          else if (typeof v === 'string') tokens.push(...Encoder.encodeWord(v));
        });
        tokens.push(Token.RECORD);
        grid.write(rid, tokens);
        const rec = grid.read(rid);
        if (rec) return send(res, 200, recordToJson(rec));
      } catch (e: any) { return send(res, 400, { error: e.message }); }
    });
    return;
  }

  // DELETE /records/<id>
  if (method === 'DELETE' && parts[0] === 'records' && parts[1]) {
    const rid = parseInt(parts[1]);
    grid.delete(rid);
    return send(res, 200, { deleted: rid });
  }

  // GET /health
  if (path === '/health') return send(res, 200, { status: 'ok', entries: grid.totalEntries });

  send(res, 404, { error: 'Not found' });
});

server.listen(PORT, () => {
  console.log(`[5bit API] http://localhost:${PORT}`);
  console.log(`[5bit API] TypeScript/Node.js — same engine as Python`);
  console.log(`[5bit API] Spec: ${spec.name} [${spec.fields.join(', ')}]`);
});
