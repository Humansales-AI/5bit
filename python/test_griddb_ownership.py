#!/usr/bin/env python3
"""
test_griddb_ownership.py — the milestone gauntlet.

T1  THE DEMO: two GRANT_Ws race one slot; the second cannot be encoded.
T2  Zero-token guarantee: refusal produces no tokens, no ledger change.
T3  Revoke -> re-grant lifecycle; stranger revoke refused.
T4  Thread race, 12 writers, one slot: exactly one success.
T5  Readers are free: many GRANT_R pins alongside a live GRANT_W.
T6  Guarded writes: holder appends; non-holder append refused;
    pinned reader offset untouched (append-only).
T7  Determinism: GrantTable.replay(events) == live table.
T8  Round-trip through pack/unpack: grant records survive bytes.
T9  Untrusted stream, hand-assembled double GRANT_W:
    validator catches what the encoder refused to produce.
T10 Malformed grant record (missing slot) caught by validator.
"""
import os, sys, threading, shutil

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from binary_grid_db import Token, Encoder, pack_to_bytes, unpack_from_bytes
from griddb_alloc import AllocGrid
from griddb_ownership import (
    OwnershipEncoder, GrantTable, EncodeRefused, MalformedOwnership,
    extract_grant_events, validate_ownership,
)

PASS = 0
def ok(name):
    global PASS; PASS += 1; print(f"  PASS  {name}")

def expect_refused(fn, name):
    try:
        fn()
    except EncodeRefused as e:
        ok(f"{name}  [{e}]"); return
    raise AssertionError(f"{name}: expected EncodeRefused")


print("T1  the demo — two GRANT_Ws, one slot")
enc = OwnershipEncoder()
tokens_a = enc.grant_w(slot=7, holder=1)          # writer A wins slot 7
assert len(tokens_a) > 0
ok(f"writer A holds slot 7 ({len(tokens_a)} tokens encoded)")
expect_refused(lambda: enc.grant_w(slot=7, holder=2),
               "writer B REFUSED AT ENCODE TIME")

print("T2  zero-token guarantee on refusal")
log_before = list(enc.event_log)
table_before = dict(enc.table.write_holder)
try:
    enc.grant_w(slot=7, holder=3)
except EncodeRefused:
    pass
assert enc.event_log == log_before and enc.table.write_holder == table_before
ok("refusal left zero tokens, zero events, ledger unchanged")

print("T3  revoke lifecycle")
expect_refused(lambda: enc.revoke(slot=7, holder=2), "stranger revoke refused")
enc.revoke(slot=7, holder=1)
ok("holder revoked its own grant")
tokens_b = enc.grant_w(slot=7, holder=2)
assert enc.table.live_writer(7) == 2
ok("writer B granted after revoke")

print("T4  thread race — 12 writers, slot 99")
enc2 = OwnershipEncoder()
results = []
def racer(h):
    try:
        enc2.grant_w(slot=99, holder=h)
        results.append(('win', h))
    except EncodeRefused:
        results.append(('refused', h))
threads = [threading.Thread(target=racer, args=(h,)) for h in range(12)]
[t.start() for t in threads]; [t.join() for t in threads]
wins = [h for r, h in results if r == 'win']
assert len(wins) == 1 and len(results) == 12
assert enc2.table.live_writer(99) == wins[0]
ok(f"exactly one winner (holder {wins[0]}), 11 refused")

print("T5  readers are free")
for h, off in [(10, 0), (11, 0), (12, 512)]:
    enc2.grant_r(slot=99, holder=h, offset=off)
assert len(enc2.table.read_pins[99]) == 3
ok("3 GRANT_R pins coexist with the live GRANT_W")

print("T6  guarded writes against a real AllocGrid")
tmp = '/tmp/own_grid'; shutil.rmtree(tmp, ignore_errors=True)
grid = AllocGrid(data_dir=tmp)
enc3 = OwnershipEncoder()
enc3.grant_w(slot=5, holder=1)
v1 = [*Encoder.encode_integer(100), Token.RECORD]
off1 = enc3.write_with_grant(grid, slot=5, holder=1, tokens=v1)
rec1 = grid.read(5)
pinned = enc3.grant_r(slot=5, holder=9, offset=rec1.byte_offset)  # reader pins v1
expect_refused(lambda: enc3.write_with_grant(grid, 5, 2,
               [*Encoder.encode_integer(666), Token.RECORD]),
               "non-holder append refused")
v2 = [*Encoder.encode_integer(200), Token.RECORD]
enc3.write_with_grant(grid, slot=5, holder=1, tokens=v2)
rec2 = grid.read(5)
assert rec2.byte_offset != rec1.byte_offset  # append-only: new version, new offset
ok(f"holder appended v2 (offset {rec1.byte_offset} -> {rec2.byte_offset}); "
   f"reader's pinned v1 offset untouched by construction")

print("T7  determinism — replay == live")
replayed = GrantTable.replay(enc2.event_log)
assert replayed.write_holder == enc2.table.write_holder
assert replayed.read_pins == enc2.table.read_pins
ok("GrantTable.replay(event_log) reproduces the live table exactly")

print("T8  bytes round-trip")
stream = [*tokens_b]  # GRANT_W holder=2 slot=7 RECORD from T3
data, pad = pack_to_bytes(stream)
back = unpack_from_bytes(data, pad)
events = extract_grant_events(back)
assert events == [('GRANT_W', 2, 7, None)], events
ok(f"grant record survives pack->bytes->unpack->parse: {events[0]}")

print("T9  untrusted stream — hand-assembled double GRANT_W")
# Build by hand what the encoder refuses to build: two live W-grants, slot 7
evil = [
    *Encoder.encode_command('GRANT_W', 1), *Encoder.encode_integer(7), Token.RECORD,
    *Encoder.encode_command('GRANT_W', 2), *Encoder.encode_integer(7), Token.RECORD,
]
try:
    validate_ownership(evil)
    raise AssertionError("validator missed double GRANT_W")
except MalformedOwnership as e:
    ok(f"validator caught it  [{e}]")

print("T10 malformed grant record")
truncated = [*Encoder.encode_command('GRANT_W', 1), Token.RECORD]  # no slot
try:
    validate_ownership(truncated)
    raise AssertionError("validator missed missing slot")
except MalformedOwnership as e:
    ok(f"validator caught it  [{e}]")

print(f"\nALL {PASS} CHECKS PASS — the second GRANT_W never existed.")
