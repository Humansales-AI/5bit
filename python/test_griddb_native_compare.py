#!/usr/bin/env python3
"""test_griddb_native_compare.py — the beast's gauntlet.

THE RULE OF THIS FILE: Python never compares. Python lays lanes on the grid,
runs the 5bit program, and reads what it emitted. Every hamming count, every
manhattan sum, every match verdict below was computed BY TOKENS.

N1  Identical fingerprints -> the program emits hamming=0, manhattan=0,
    and the exact-match program emits 1.
N2  Known divergence (3 lanes differ) -> emits hamming=3, manhattan=|sum|,
    match=0. Values asserted against hand-computed constants.
N3  Oracle cross-check: 25 RANDOM fingerprint pairs; the 5bit program's
    answers must equal an independent reference computation for every pair.
    (Python here is the test's oracle, not the feature's computer.)
N4  CRM demo: query deal vs 3 stored deals -> three native distances;
    the nearest deal is the one the program said is nearest.
N5  Programs are bytes: the comparator survives pack->unpack->run, and its
    size on disk is reported. The comparator IS a record.
"""
import os, sys, shutil, random
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from binary_grid_db import Token, Encoder, pack_to_bytes, unpack_from_bytes
from griddb_alloc import AllocGrid
from griddb_ownership import OwnershipEncoder
from griddb_interp import Machine
from griddb_native_compare import build_column_comparator, build_exact_match

PASS = 0
def ok(name):
    global PASS; PASS += 1; print(f"  PASS  {name}")

D = 8                    # lanes per fingerprint
BASE_A, BASE_B = 500, 520
SLOT_T, SLOT_H, SLOT_C = 590, 591, 592
PROG_CMP, PROG_MATCH = 60, 61


def fresh():
    tmp = '/tmp/native_cmp'; shutil.rmtree(tmp, ignore_errors=True)
    grid = AllocGrid(data_dir=tmp)
    own = OwnershipEncoder()
    for s in list(range(BASE_A, BASE_A + D)) + list(range(BASE_B, BASE_B + D)) \
             + [SLOT_T, SLOT_H, SLOT_C]:
        own.grant_w(slot=s, holder=0)
    m = Machine(ownership=own, grid=grid, holder=0, max_steps=1_000_000)
    m.load(PROG_CMP, build_column_comparator(PROG_CMP, BASE_A, BASE_B, D,
                                             SLOT_T, SLOT_H, SLOT_C))
    m.load(PROG_MATCH, build_exact_match(PROG_MATCH, SLOT_H))
    return grid, own, m


def lay(own, grid, base, vec):
    """Python's ONLY job: put tokens on the grid."""
    for i, v in enumerate(vec):
        own.write_with_grant(grid, base + i, 0,
                             [*Encoder.encode_integer(v), Token.RECORD])


def compare(m, grid, own, a, b):
    """Run the 5bit comparator; return what IT emitted."""
    lay(own, grid, BASE_A, a); lay(own, grid, BASE_B, b)
    m.output.clear()
    m.run(PROG_CMP)
    m.run(PROG_MATCH)
    hamming, manhattan, match = m.output
    return hamming, manhattan, match


print("N1  identical fingerprints")
grid, own, m = fresh()
fp = [7, 0, 31, 15, 2, 2, 9, 30]
h, c, eq = compare(m, grid, own, fp, fp)
assert (h, c, eq) == (0, 0, 1), (h, c, eq)
ok(f"tokens said: hamming={h} manhattan={c} match={eq}")

print("N2  known divergence")
a = [7, 0, 31, 15, 2, 2, 9, 30]
b = [7, 5, 31, 12, 2, 2, 9, 26]     # lanes 1,3,7 diverge: |0-5|+|15-12|+|30-26| = 12
h, c, eq = compare(m, grid, own, a, b)
assert (h, c, eq) == (3, 12, 0), (h, c, eq)
ok(f"tokens said: hamming={h} manhattan={c} match={eq} (expected 3, 12, 0)")

print("N3  oracle cross-check: 25 random pairs")
random.seed(31)
for trial in range(25):
    a = [random.randrange(32) for _ in range(D)]
    b = a[:] if trial % 5 == 0 else [random.randrange(32) for _ in range(D)]
    h, c, eq = compare(m, grid, own, a, b)
    ref_h = sum(1 for x, y in zip(a, b) if x != y)          # oracle
    ref_c = sum(abs(x - y) for x, y in zip(a, b))           # oracle
    assert (h, c, eq) == (ref_h, ref_c, 1 if ref_h == 0 else 0), \
        (trial, a, b, (h, c, eq), (ref_h, ref_c))
ok("25/25 pairs: token-computed hamming/manhattan/match == reference oracle")

print("N4  CRM: nearest deal, natively")
query = [12, 4, 0, 22, 7, 7, 1, 19]
deals = {
    "deal_X": [12, 4, 0, 22, 7, 9, 1, 19],    # manhattan 2
    "deal_Y": [30, 4, 0, 22, 7, 7, 1, 19],    # manhattan 18
    "deal_Z": [12, 4, 0, 22, 7, 7, 1, 19],    # manhattan 0 — exact
}
dist = {}
for name, fp in deals.items():
    _, c, eq = compare(m, grid, own, query, fp)
    dist[name] = (c, eq)
assert dist["deal_Z"] == (0, 1) and dist["deal_X"][0] == 2 and dist["deal_Y"][0] == 18
nearest = min(dist, key=lambda k: dist[k][0])
assert nearest == "deal_Z"
ok(f"native distances {dict((k, v[0]) for k, v in dist.items())} -> nearest={nearest}, exact match flagged by tokens")

print("N5  the comparator is a record")
toks = build_column_comparator(PROG_CMP, BASE_A, BASE_B, D, SLOT_T, SLOT_H, SLOT_C)
data, pad = pack_to_bytes(toks)
m2_grid, m2_own, m2 = fresh()
m2.load_bytes(PROG_CMP, data, pad)          # reload the comparator FROM BYTES
lay(m2_own, m2_grid, BASE_A, a := [1, 2, 3, 4, 5, 6, 7, 8])
lay(m2_own, m2_grid, BASE_B, [1, 2, 3, 9, 5, 6, 7, 8])
m2.output.clear(); m2.run(PROG_CMP)
assert m2.output == [1, 5]
ok(f"comparator = {len(toks)} tokens = {len(data)} bytes on disk; ran from bytes: hamming=1 manhattan=5")

print(f"\nALL {PASS} CHECKS PASS — Python laid tokens; the fabric did every comparison.")
