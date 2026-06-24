#!/usr/bin/env python3
"""
Canonical-compaction proof (Python engine).

Proves three things about AllocGrid.compact():
  1. Space is really reclaimed  — on-disk files actually shrink.
  2. Data survives compaction    — non-tombstoned records still read correct,
                                   tombstones are gone.
  3. Compaction is CANONICAL     — a bloated grid (churn + tombstones), after
                                   compact(), is BYTE-IDENTICAL to a fresh grid
                                   built from only the survivors. i.e. logical
                                   content -> same bytes, regardless of history.

Exit 0 on success, 1 on any failure.
"""
import hashlib
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from binary_grid_db import Encoder            # noqa: E402
from griddb_alloc import AllocGrid            # noqa: E402

N = 100


def name(i: int) -> str:
    return "USER" + chr(65 + (i % 10))


def value(i: int) -> int:
    return i * 100 + 2  # final value after churn rounds (see below)


def sha(path: str) -> str:
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def main() -> int:
    root = tempfile.mkdtemp(prefix="verify_compact_")
    try:
        d1 = os.path.join(root, "a")
        g = AllocGrid(data_dir=d1)

        # bloat: initial write + 3 churn rounds (rewrites append new copies)
        for i in range(N):
            g.write(i, Encoder.encode_record(name(i), i * 100))
        for r in range(3):
            for i in range(N):
                g.write(i, Encoder.encode_record(name(i), i * 100 + r))
        # final settling write so survivors are deterministic at value(i)
        for i in range(N):
            g.write(i, Encoder.encode_record(name(i), value(i)))
        # tombstone the evens
        for i in range(0, N, 2):
            g.delete(i)

        before = os.path.getsize(g.data_path) + os.path.getsize(g.alloc_path)
        freed = g.compact()
        after = os.path.getsize(g.data_path) + os.path.getsize(g.alloc_path)

        # 1) shrink
        shrink = before - after
        ok_shrink = shrink > 0 and freed == shrink

        # 2) survivors intact, tombstones gone
        ok_data = True
        for i in range(N):
            rec = g.read(i)
            if i % 2 == 0:
                if rec is not None and not rec.is_tombstone:
                    ok_data = False
            else:
                if rec is None or rec.is_tombstone:
                    ok_data = False
                    continue
                parsed_vals = [p.value for p in rec.parsed if getattr(p, "value", None) is not None]
                if not parsed_vals or parsed_vals[0] != value(i):
                    ok_data = False
        g.close()

        # 3) canonical: fresh grid with ONLY survivors at same rids/order
        d2 = os.path.join(root, "b")
        fresh = AllocGrid(data_dir=d2)
        for i in range(1, N, 2):
            fresh.write(i, Encoder.encode_record(name(i), value(i)))
        fresh.close()

        h_compacted = sha(os.path.join(d1, "data.grid"))
        h_fresh = sha(os.path.join(d2, "data.grid"))
        ok_canonical = h_compacted == h_fresh

        print("  [py] before/after bytes : %d -> %d  (freed %d, shrink %d)"
              % (before, after, freed, shrink))
        print("  [py] space reclaimed    : %s" % ("PASS" if ok_shrink else "FAIL"))
        print("  [py] survivors/tombstone: %s" % ("PASS" if ok_data else "FAIL"))
        print("  [py] canonical bytes    : %s  (%s == %s)"
              % ("PASS" if ok_canonical else "FAIL", h_compacted[:16], h_fresh[:16]))

        return 0 if (ok_shrink and ok_data and ok_canonical) else 1
    finally:
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
