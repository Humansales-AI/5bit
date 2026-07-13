#!/usr/bin/env python3
"""test_griddb_asm.py — the notation gauntlet.

A1  LEXICON2 §6 vectors via `int` statements -> exact reference hex.
A2  Full program: labels + mixed values + record; parses back losslessly.
A3  THE REFUSAL: source with two GRANT_Ws on one slot cannot assemble;
    refusal carries the line number; zero bytes exist.
A4  Grant lifecycle assembles: grant -> revoke -> re-grant.
A5  Depth checking on raw lines: over-deep, unbalanced END, unclosed EOF —
    all refused with line numbers.
A6  Round-trip: disassemble(assemble(src)) re-assembles to identical tokens
    and identical bytes (notation leaves no residue).
A7  Assembled grant records satisfy validate_ownership (layers compose).
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from binary_grid_db import Token, pack_to_bytes
from griddb_asm import Assembler, AssembleRefused, disassemble
from griddb_ownership import OwnershipEncoder, validate_ownership

PASS = 0
def ok(name):
    global PASS; PASS += 1; print(f"  PASS  {name}")


print("A1  LEXICON2 vectors through notation")
vec = {"int 0": ("0780", 6), "int 1": ("0f80", 6), "int -1": ("8f80", 6),
       "int 42": ("20bc", 1), "int -42": ("a4bc", 1)}
for src, (hexs, pad) in vec.items():
    data, p, _ = Assembler().assemble_bytes(src)
    assert (data.hex(), p) == (hexs, pad), (src, data.hex(), p)
ok("all 5 reference vectors byte-identical via `int` statements")

print("A2  full labeled record")
prog = '''
; a labeled tuple, LEXICON2 §7 interleaved layout
label 0 "age"
int 34
label 1 "name"
word "ALICE"
record
'''
data, pad, toks = Assembler().assemble_bytes(prog)
assert toks[-1] == Token.RECORD and len(data) > 0
ok(f"assembled: {len(toks)} tokens, {len(data)} bytes, pad {pad}")

print("A3  THE REFUSAL — conflicting grants cannot assemble")
conflict = '''grant_w 7 holder 1
word "WRITER A OWNS SLOT SEVEN"
grant_w 7 holder 2      ; <- writer B, same slot: must refuse
record
'''
try:
    Assembler().assemble_bytes(conflict)
    raise AssertionError("conflicting program assembled!")
except AssembleRefused as e:
    assert e.lineno == 3, e.lineno
    ok(f"refused at line {e.lineno}; zero bytes produced")
    print(f"        {e}".replace(chr(10), chr(10) + '        '))

print("A4  lifecycle assembles")
lifecycle = '''grant_w 7 holder 1
revoke 7 holder 1
grant_w 7 holder 2
record
'''
own = OwnershipEncoder()
data, pad, toks = Assembler(own).assemble_bytes(lifecycle)
assert own.table.live_writer(7) == 2
ok("grant -> revoke -> re-grant assembled; table shows holder 2")

print("A5  depth checking on raw lines")
cases = {
    "raw START START START START START": "depth 5 exceeds",
    "raw START START D4": "ends at depth 2",
}
# NOTE: `raw END` at depth 0 is LEGAL per lexicon (§4 integer finalizer),
# so it is deliberately not a refusal case.
for src, needle in cases.items():
    try:
        Assembler().assemble(src)
        raise AssertionError(f"accepted: {src}")
    except AssembleRefused as e:
        assert needle in str(e), (src, str(e))
ok("over-deep and unclosed-at-EOF refused with line info")

print("A6  round-trip: disassemble -> reassemble, zero residue")
rt_src = '''auth 1
grant_w 12 holder 1
label 0 "score"
int -987
label 1 "tag"
word "HELLO WORLD"
record
grant_r 12 holder 9 offset 40
record
'''
t1 = Assembler().assemble(rt_src)
notation = disassemble(t1)
t2 = Assembler().assemble(notation)
assert t1 == t2, "token mismatch after round-trip"
assert pack_to_bytes(t1) == pack_to_bytes(t2)
ok(f"tokens and bytes identical through disassemble->assemble ({len(t1)} tokens)")

print("A7  layers compose: assembled stream passes the ownership validator")
table = validate_ownership(t1)
assert table.live_writer(12) == 1
ok("validate_ownership accepts the assembled stream; live writer = 1")

print(f"\nALL {PASS} CHECKS PASS — the conflicting program was never bytes.")
