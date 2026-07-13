#!/usr/bin/env python3
"""test_griddb_interp.py — the walker's gauntlet. Tokens that DO.

I1  Arithmetic program: (4+2)*10-5 emits 55 (postfix over the value stack).
I2  DEF enforcement: a record without a DEF header cannot be loaded/executed.
I3  IF: both arms — then-path and else-path — take the right region.
I4  LOOP+BREAK+slots-as-variables: sum 1..5 == 15, state lives in the grid.
I5  CALL/RET: nested calls share the value stack (square-then-add pipeline).
I6  THE REFUSAL, MID-PROGRAM: running as holder 2, STORE to a slot whose
    GRANT_W belongs to holder 1 -> EncodeRefused halts execution; the slot's
    value is untouched; the trace shows exactly how far the program got.
I7  Gas: an infinite LOOP hits OutOfGas deterministically (no hang, ever).
I8  Programs survive bytes: pack -> unpack -> load -> run, same output.
I9  Early RET: statements after RET never execute.
I10 Grants make effects legal: after REVOKE + re-grant to holder 2, the
    exact program that was refused in I6 now runs. Ownership is the switch.
"""
import os, sys, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from binary_grid_db import Token, Encoder, pack_to_bytes, unpack_from_bytes
from griddb_alloc import AllocGrid
from griddb_ownership import OwnershipEncoder, EncodeRefused
from griddb_interp import (
    Machine, InterpreterError, OutOfGas,
    verb, num, region, program,
    CMD_DEF, CMD_CALL, CMD_RET, CMD_IF, CMD_LOOP, CMD_BREAK, CMD_STORE, CMD_READ,
    LPAREN, RPAREN, EMIT,
)

PLUS, MINUS, MUL = Token(10), Token(11), Token(12)
PASS = 0
def ok(name):
    global PASS; PASS += 1; print(f"  PASS  {name}")


print("I1  arithmetic: (4+2)*10-5")
m = Machine()
m.load(1, program(1, num(4), num(2), [PLUS], num(10), [MUL], num(5), [MINUS], [EMIT]))
assert m.run(1) == [55], m.output
ok("emitted 55")

print("I2  DEF enforcement")
try:
    Machine().load(2, [*num(42), Token.RECORD])   # plain data record
    raise AssertionError("data record loaded as program")
except InterpreterError as e:
    ok(f"data is not executable  [{e}]")

print("I3  IF: boolean degenerate case (1 -> +arm, 0 -> 0arm) — old style intact")
m = Machine()
# test=1 -> then emits 111 ; test=0 -> else emits 222
m.load(3, program(3, num(1), verb(CMD_IF),
                  region(num(111), [EMIT]), region(num(222), [EMIT])))
m.load(4, program(4, num(0), verb(CMD_IF),
                  region(num(111), [EMIT]), region(num(222), [EMIT])))
m.run(3); m.run(4)
assert m.output == [111, 222], m.output
ok("flag 1 took +arm (111), flag 0 took 0-arm (222): booleans unchanged")

print("I4  LOOP + BREAK + slots as variables: sum 1..5")
tmp = '/tmp/interp_grid'; shutil.rmtree(tmp, ignore_errors=True)
grid = AllocGrid(data_dir=tmp)
own = OwnershipEncoder()
own.grant_w(slot=100, holder=0)   # i
own.grant_w(slot=101, holder=0)   # acc
m = Machine(ownership=own, grid=grid, holder=0)
m.load(10, program(10,
    num(0), verb(CMD_STORE, 101),                     # acc = 0
    num(1), verb(CMD_STORE, 100),                     # i = 1
    verb(CMD_LOOP), region(
        verb(CMD_READ, 100), num(6), [MINUS],         # test: i-6
        verb(CMD_IF),
        region(verb(CMD_BREAK)),                       # +arm: i>6 (never)
        region(verb(CMD_BREAK)),                       # 0arm: i==6 -> done
        region(                                        # -arm: i<6 -> body
            verb(CMD_READ, 101), verb(CMD_READ, 100), [PLUS], verb(CMD_STORE, 101),
            verb(CMD_READ, 100), num(1), [PLUS], verb(CMD_STORE, 100),
        ),
    ),
    verb(CMD_READ, 101), [EMIT],
))
assert m.run(10) == [15], m.output
ok(f"sum(1..5) == 15; program state lived in grid slots 100/101 "
   f"(i ended at {m._parse_ints(grid.read(100).tokens)[0]})")

print("I5  CALL/RET: nested calls, shared stack")
m2 = Machine()
m2.load(20, program(20, num(7), verb(CMD_CALL, 21), num(1), [PLUS], [EMIT]))  # square(7)+1
m2.load(21, program(21, verb(CMD_RET)))  # placeholder replaced below
# square: pops? stack model: caller pushed 7; square does x*x needs dup —
# variables-are-slots instead: store, read twice.
own2 = OwnershipEncoder(); own2.grant_w(slot=200, holder=0)
tmp2 = '/tmp/interp_grid2'; shutil.rmtree(tmp2, ignore_errors=True)
grid2 = AllocGrid(data_dir=tmp2)
m2 = Machine(ownership=own2, grid=grid2, holder=0)
m2.load(20, program(20, num(7), verb(CMD_CALL, 21), num(1), [PLUS], [EMIT]))
m2.load(21, program(21,
    verb(CMD_STORE, 200),                              # x = pop
    verb(CMD_READ, 200), verb(CMD_READ, 200), [MUL],   # push x*x
))
assert m2.run(20) == [50], m2.output
assert "CALL 21" in m2.trace and "RET  21" in m2.trace
ok("square(7)+1 == 50 through CALL/RET; arg and return rode the shared stack")

print("I6  THE REFUSAL — mid-program, effects are grants")
own3 = OwnershipEncoder()
own3.grant_w(slot=300, holder=1)                       # holder 1 owns slot 300
tmp3 = '/tmp/interp_grid3'; shutil.rmtree(tmp3, ignore_errors=True)
grid3 = AllocGrid(data_dir=tmp3)
own3.write_with_grant(grid3, 300, 1, [*num(1000), Token.RECORD])  # v1 = 1000
intruder = Machine(ownership=own3, grid=grid3, holder=2)          # runs as holder 2
prog = program(30, num(777), [EMIT], num(666), verb(CMD_STORE, 300), num(999), [EMIT])
intruder.load(30, prog)
try:
    intruder.run(30)
    raise AssertionError("intruder program completed!")
except EncodeRefused as e:
    assert intruder.output == [777]                    # got exactly this far
    assert 999 not in intruder.output                  # never reached
    v = intruder._parse_ints(grid3.read(300).tokens)[0]
    assert v == 1000                                   # slot untouched
    ok(f"halted mid-program at STORE; slot 300 still {v}; "
       f"trace: {intruder.trace + [f'REFUSED: {e}']}"[:120] + "...")

print("I7  gas: infinite LOOP terminates deterministically")
m3 = Machine(max_steps=5000)
m3.load(40, program(40, verb(CMD_LOOP), region(num(1), num(2), [PLUS], [EMIT])))
try:
    m3.run(40)
    raise AssertionError("infinite loop completed?!")
except OutOfGas as e:
    ok(f"OutOfGas after budget  [{e}]")

print("I8  programs survive bytes")
toks = program(50, num(21), num(2), [MUL], [EMIT])
data, pad = pack_to_bytes(toks)
m4 = Machine()
m4.load_bytes(50, data, pad)
assert m4.run(50) == [42]
ok(f"pack -> {len(data)} bytes -> unpack -> run: emitted 42")

print("I9  early RET")
m5 = Machine()
m5.load(60, program(60, num(1), [EMIT], verb(CMD_RET), num(2), [EMIT]))
assert m5.run(60) == [1]
ok("statements after RET never executed")

print("I10 grants are the switch: refused program runs after re-grant")
own3.revoke(slot=300, holder=1)
own3.grant_w(slot=300, holder=2)
intruder2 = Machine(ownership=own3, grid=grid3, holder=2)
intruder2.load(30, prog)                               # the EXACT same program
assert intruder2.run(30) == [777, 999]
v = intruder2._parse_ints(grid3.read(300).tokens)[0]
assert v == 666
ok(f"same program, grant flipped: completed, slot 300 now {v}")

print("I11 P1 acceptance: 'deal value > 5000 -> promote', one MINUS, three-way IF")
# stages: 1=Lead, 2=Qualified. slot V holds value, slot S holds stage.
own4 = OwnershipEncoder()
tmp4 = '/tmp/interp_grid4'; shutil.rmtree(tmp4, ignore_errors=True)
grid4 = AllocGrid(data_dir=tmp4)
for V, S in [(400, 401), (410, 411), (420, 421)]:
    own4.grant_w(slot=V, holder=0); own4.grant_w(slot=S, holder=0)
m6 = Machine(ownership=own4, grid=grid4, holder=0)
def deal(V, S, value):
    own4.write_with_grant(grid4, V, 0, [*num(value), Token.RECORD])
    own4.write_with_grant(grid4, S, 0, [*num(1), Token.RECORD])      # Lead
    slot_id = 70 + V
    m6.load(slot_id, program(slot_id,
        verb(CMD_READ, V), num(5000), [MINUS],
        verb(CMD_IF), region(num(2), verb(CMD_STORE, S)),   # +arm ONLY: strict >
    ))
    m6.run(slot_id)
    return m6._parse_ints(grid4.read(S).tokens)[0]
s_high = deal(400, 401, 7000)   # > 5000  -> promote
s_low  = deal(410, 411, 4000)   # < 5000  -> stay (the P1 bug, now dead)
s_edge = deal(420, 421, 5000)   # == 5000 -> stay (strict >)
assert (s_high, s_low, s_edge) == (2, 1, 1), (s_high, s_low, s_edge)
ok(f"7000 -> Qualified({s_high}); 4000 -> Lead({s_low}); 5000 -> Lead({s_edge}) — sign reached the branch")

print("I12 all six relations, zero new tokens")
def rel(a, b, arms):
    mm = Machine()
    mm.load(80, program(80, num(a), num(b), [MINUS], verb(CMD_IF), *arms, num(1), [EMIT]))
    mm.run(80)
    return len(mm.output) == 2       # emitted marker inside arm + trailing 1
GT  = lambda: [region(num(9), [EMIT])]
EQ_ = lambda: [region(), region(num(9), [EMIT])]
LT  = lambda: [region(), region(), region(num(9), [EMIT])]
GE  = lambda: [region(num(9), [EMIT]), region(num(9), [EMIT])]
LE  = lambda: [region(), region(num(9), [EMIT]), region(num(9), [EMIT])]
NE  = lambda: [region(num(9), [EMIT]), region(), region(num(9), [EMIT])]
checks = [
    (rel(7, 5, GT()), True),  (rel(5, 7, GT()), False), (rel(5, 5, GT()), False),
    (rel(5, 5, EQ_()), True), (rel(7, 5, EQ_()), False),
    (rel(3, 8, LT()), True),  (rel(8, 3, LT()), False),
    (rel(7, 5, GE()), True),  (rel(5, 5, GE()), True),  (rel(3, 5, GE()), False),
    (rel(3, 5, LE()), True),  (rel(5, 5, LE()), True),  (rel(7, 5, LE()), False),
    (rel(3, 5, NE()), True),  (rel(5, 5, NE()), False),
]
assert all(got == want for got, want in checks), checks
ok("> == < >= <= != all correct via ONE MINUS + region placement")

print(f"\nALL {PASS} CHECKS PASS — the walker walks, and the branch finally sees the sign.")
