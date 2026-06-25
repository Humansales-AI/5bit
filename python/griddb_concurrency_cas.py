#!/usr/bin/env python3
"""GridDB Concurrency Regression — Multi-Process CAS

Proves write_if() conserves updates under real cross-process contention.
Guards against the reentrant-lock bug where a nested write() released the
lock the outer CAS still held, silently dropping writes (balances wrong,
WAL clean). Uses the intended API: AllocGrid point reads + write_if CAS.

    python3 griddb_concurrency_cas.py     # exits 1 on any lost update
"""
import sys, os, tempfile, time, multiprocessing as mp
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from griddb_alloc import AllocGrid
from binary_grid_db import Encoder, ParsedNumber


def balance(rec):
    if rec is None:
        return 0
    nums = [x.value for x in rec.parsed if isinstance(x, ParsedNumber)]
    return nums[-1] if nums else 0


def _worker(args):
    data_dir, account, n = args
    g = AllocGrid(data_dir)
    done = 0
    while done < n:
        rec = g.read(account)
        toks = Encoder.encode_integer(balance(rec) + 1)
        if rec is None:
            g.write(account, toks); done += 1
        elif g.write_if(account, toks, rec.byte_offset, rec.bit_length):
            done += 1
        # CAS False -> someone else wrote first -> retry
    return 0


def run(accounts, procs_per_account, deposits):
    d = tempfile.mkdtemp()
    g = AllocGrid(d)
    for a in range(accounts):
        g.write(a, Encoder.encode_integer(0))
    jobs = [(d, a, deposits) for a in range(accounts)
            for _ in range(procs_per_account)]
    t0 = time.perf_counter()
    with mp.Pool(len(jobs)) as p:
        p.map(_worker, jobs)
    elapsed = time.perf_counter() - t0
    expected = procs_per_account * deposits
    g2 = AllocGrid(d)
    bals = [balance(g2.read(a)) for a in range(accounts)]
    ok = all(b == expected for b in bals)
    label = f"{accounts} acct x {procs_per_account} procs x {deposits} dep"
    print(f"  {label:<34} expect {expected}/acct  got {bals}  "
          f"{'✓' if ok else '✗'}  ({elapsed:.1f}s)")
    return ok


if __name__ == "__main__":
    print("═" * 62)
    print("  GridDB Concurrency Regression — Multi-Process CAS")
    print("═" * 62)
    results = [
        run(accounts=4, procs_per_account=6,  deposits=50),   # spread
        run(accounts=1, procs_per_account=12, deposits=100),  # one hot account
        run(accounts=1, procs_per_account=12, deposits=100),  # repeat (flake guard)
    ]
    passed = all(results)
    print("─" * 62)
    print(f"  RESULT: {'PASS — no lost updates' if passed else 'FAIL — updates lost under contention'}")
    print("═" * 62)
    sys.exit(0 if passed else 1)
