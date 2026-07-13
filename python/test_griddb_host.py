#!/usr/bin/env python3
"""test_griddb_host.py — the doorman's gauntlet. The fabric touches the world.

H1  A granted program reads the clock (NOW) and emits it — effect performed.
H2  DENY-BY-DEFAULT: a program calls a capability it wasn't granted ->
    HostRefused, mid-program, nothing sent to the world.
H3  The grant is the switch: same program, after the capability is granted,
    now performs the effect. (The I10 pattern, for effects.)
H4  A full request loop in PURE 5BIT: READ_IN a request value, apply a rule
    (value > threshold?), EMIT_OUT the verdict to the host outbox. No Python
    logic — the doorman only moved bytes through the door.
H5  HASH capability: program hashes an input; determinism holds across runs.
H6  Audit: every door opened is recorded; ungranted attempts leave no effect.
H7  Determinism with injected seams: same clock+inbox -> byte-identical
    outbox, twice.
"""
import os, sys, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from binary_grid_db import Token
from griddb_alloc import AllocGrid
from griddb_ownership import OwnershipEncoder
from griddb_interp import (
    Machine, verb, num, region, program,
    CMD_CALL, CMD_IF, CMD_STORE, CMD_READ,
)
from griddb_host import (
    Doorman, attach_doorman, HostRefused,
    CAP_NOW, CAP_RANDOM, CAP_HASH, CAP_EMIT_OUT, CAP_LOG, CAP_READ_IN,
)

PLUS, MINUS, EMIT = Token(10), Token(11), Token(14)
PASS = 0
def ok(name):
    global PASS; PASS += 1; print(f"  PASS  {name}")


def build(holder=0, caps=(), clock=None, inbox=None):
    own = OwnershipEncoder()
    for c in caps:
        own.grant_w(slot=c, holder=holder)          # grant = capability
    tmp = '/tmp/host_grid'; shutil.rmtree(tmp, ignore_errors=True)
    grid = AllocGrid(data_dir=tmp)
    m = Machine(ownership=own, grid=grid, holder=holder, max_steps=200000)
    d = Doorman(own, clock=clock, inbox=inbox)
    attach_doorman(m, d)
    return own, grid, m, d


print("H1  granted clock read")
own, grid, m, d = build(caps=[CAP_NOW], clock=lambda: 1_700_000_000)
m.load(1, program(1, verb(CMD_CALL, CAP_NOW), [EMIT]))
assert m.run(1) == [1_700_000_000]
ok(f"program emitted host time {m.output[0]} via NOW")

print("H2  deny-by-default")
own, grid, m, d = build(caps=[])          # NO capabilities granted
m.load(2, program(2, num(42), verb(CMD_CALL, CAP_EMIT_OUT)))
try:
    m.run(2)
    raise AssertionError("ungranted effect performed!")
except HostRefused as e:
    assert d.outbox == []
    ok(f"EMIT_OUT refused, outbox empty  [{e}]")

print("H3  the grant is the switch")
own, grid, m, d = build(caps=[CAP_EMIT_OUT])
m.load(3, program(3, num(42), verb(CMD_CALL, CAP_EMIT_OUT)))
m.run(3)
assert d.outbox == [42]
ok(f"same shape of program, capability granted -> outbox={d.outbox}")

print("H4  full request loop in pure 5bit")
# rule: if request value > 100, send 1 (approve) else 0 (deny) to the world
own, grid, m, d = build(caps=[CAP_READ_IN, CAP_EMIT_OUT], inbox=[250, 30, 100])
loop_prog = program(4,
    verb(CMD_CALL, CAP_READ_IN), num(100), [MINUS],   # req - 100
    verb(CMD_IF),
    region(num(1), verb(CMD_CALL, CAP_EMIT_OUT)),      # +arm: >100 -> approve
    region(num(0), verb(CMD_CALL, CAP_EMIT_OUT)),      # 0arm: ==100 -> deny
    region(num(0), verb(CMD_CALL, CAP_EMIT_OUT)),      # -arm: <100 -> deny
)
m.load(4, loop_prog)
for _ in range(3):          # process three requests
    m.run(4)
assert d.outbox == [1, 0, 0], d.outbox   # 250>100 approve, 30<100 deny, 100== deny
ok(f"3 requests through the door, verdicts by tokens: outbox={d.outbox}")

print("H5  HASH determinism")
own, grid, m, d = build(caps=[CAP_HASH])
m.load(5, program(5, num(12345), verb(CMD_CALL, CAP_HASH), [EMIT]))
h1 = m.run(5)[0]
m.output.clear()
own2, g2, m2, d2 = build(caps=[CAP_HASH])
m2.load(5, program(5, num(12345), verb(CMD_CALL, CAP_HASH), [EMIT]))
h2 = m2.run(5)[0]
assert h1 == h2
ok(f"HASH(12345) = {h1} stable across independent runs")

print("H6  audit trail")
own, grid, m, d = build(caps=[CAP_NOW, CAP_LOG], clock=lambda: 42)
m.load(6, program(6, verb(CMD_CALL, CAP_NOW), verb(CMD_CALL, CAP_LOG)))
m.run(6)
assert d.calls == ['NOW()', 'LOG(42)'] and d.log == [42]
ok(f"every door logged: calls={d.calls}, log={d.log}")

print("H7  determinism with injected seams")
def run_once():
    own, grid, m, d = build(caps=[CAP_READ_IN, CAP_NOW, CAP_EMIT_OUT],
                            clock=lambda: 999, inbox=[7, 8])
    m.load(7, program(7,
        verb(CMD_CALL, CAP_READ_IN), verb(CMD_CALL, CAP_NOW), [PLUS],
        verb(CMD_CALL, CAP_EMIT_OUT),
        verb(CMD_CALL, CAP_READ_IN), verb(CMD_CALL, CAP_NOW), [PLUS],
        verb(CMD_CALL, CAP_EMIT_OUT),
    ))
    m.run(7)
    return d.outbox
a, b = run_once(), run_once()
assert a == b == [1006, 1007], (a, b)
ok(f"same seams -> identical outbox twice: {a}")

print(f"\nALL {PASS} CHECKS PASS — the fabric reached through the door, only where granted.")
