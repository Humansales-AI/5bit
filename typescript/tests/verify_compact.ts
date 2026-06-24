/**
 * Canonical-compaction proof (TypeScript engine).
 *
 * Proves AllocGrid.compact():
 *   1. Space is really reclaimed  — files actually shrink, reported == actual.
 *   2. Data survives compaction    — survivors read correct, tombstones gone.
 *   3. Compaction is CANONICAL     — bloated grid after compact() is BYTE-
 *      IDENTICAL to a fresh grid built from only the survivors.
 *
 * Exit 0 on success, 1 on any failure.
 */
import fs from 'fs';
import path from 'path';
import os from 'os';
import crypto from 'crypto';
import { AllocGrid } from '../src/alloc';
import { Encoder } from '../src/encoder';

const N = 100;
const name = (i: number) => 'USER' + String.fromCharCode(65 + (i % 10));
const value = (i: number) => i * 100 + 2; // final value after churn
const sha = (p: string) => crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex');

function main(): number {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'verify_compact_'));
  try {
    const d1 = path.join(root, 'a');
    const g = new AllocGrid(d1);

    for (let i = 0; i < N; i++) g.write(i, Encoder.encodeRecord(name(i), i * 100));
    for (let r = 0; r < 3; r++) for (let i = 0; i < N; i++) g.write(i, Encoder.encodeRecord(name(i), i * 100 + r));
    for (let i = 0; i < N; i++) g.write(i, Encoder.encodeRecord(name(i), value(i)));
    for (let i = 0; i < N; i += 2) g.delete(i);

    const before = g.dataFileSize + g.allocFileSize;
    const freed = g.compact();
    const after = g.dataFileSize + g.allocFileSize;

    const shrink = before - after;
    const okShrink = shrink > 0 && freed === shrink;

    let okData = true;
    for (let i = 0; i < N; i++) {
      const rec = g.read(i);
      if (i % 2 === 0) {
        if (rec && !rec.isTombstone) okData = false;
      } else {
        if (!rec || rec.isTombstone) { okData = false; continue; }
        const v = (rec.parsed as any[]).find(p => p.type === 'number');
        if (!v || v.value !== value(i)) okData = false;
      }
    }

    const d2 = path.join(root, 'b');
    const fresh = new AllocGrid(d2);
    for (let i = 1; i < N; i += 2) fresh.write(i, Encoder.encodeRecord(name(i), value(i)));

    const hCompacted = sha(path.join(d1, 'data.grid'));
    const hFresh = sha(path.join(d2, 'data.grid'));
    const okCanonical = hCompacted === hFresh;

    console.log(`  [ts] before/after bytes : ${before} -> ${after}  (freed ${freed}, shrink ${shrink})`);
    console.log(`  [ts] space reclaimed    : ${okShrink ? 'PASS' : 'FAIL'}`);
    console.log(`  [ts] survivors/tombstone: ${okData ? 'PASS' : 'FAIL'}`);
    console.log(`  [ts] canonical bytes    : ${okCanonical ? 'PASS' : 'FAIL'}  (${hCompacted.slice(0, 16)} == ${hFresh.slice(0, 16)})`);

    return okShrink && okData && okCanonical ? 0 : 1;
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
}

process.exit(main());
