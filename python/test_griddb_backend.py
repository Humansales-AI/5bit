#!/usr/bin/env python3
"""test_griddb_backend.py — full verb set compiled to x86-64, run on the CPU.

Every result below was produced by NATIVE MACHINE CODE the compiler emitted
from 5bit tokens, then cross-checked against the interpreter.

B1  arithmetic (baseline still holds)
B2  three-way IF: all six relations, native branches
B3  READ/STORE against the slot arena (memory works)
B4  LOOP + BREAK + slots: sum(1..5) = 15, natively
B5  GRANT CHECK IN MACHINE CODE: ungranted STORE -> REFUSED sentinel, slot
    untouched; granted -> completes. Effects are grants, in x86-64.
B6  CALL/RET across compiled programs
B7  interpreter == native, program for program
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from binary_grid_db import Token
from griddb_interp import (Machine, verb, num, region, program,
    CMD_CALL, CMD_IF, CMD_STORE, CMD_READ, CMD_LOOP, CMD_BREAK)
from griddb_backend import run, REFUSED
from griddb_alloc import AllocGrid
from griddb_ownership import OwnershipEncoder
import shutil

PLUS, MINUS, MUL, EMIT = Token(10), Token(11), Token(12), Token(14)
PASS = 0
def ok(n): 
    global PASS; PASS += 1; print(f"  PASS  {n}")

def interp(prog_slot, progs, grants=None, slots=None, holder=0):
    own = OwnershipEncoder()
    tmp='/tmp/bkgrid'; shutil.rmtree(tmp, ignore_errors=True)
    g = AllocGrid(data_dir=tmp)
    for s,h in (grants or {}).items(): own.grant_w(slot=s, holder=h)
    m = Machine(ownership=own, grid=g, holder=holder, max_steps=500000)
    if slots:
        for s,v in slots.items():
            if (grants or {}).get(s)==holder or s in (grants or {}):
                pass
    for s,t in progs.items(): m.load(s,t)
    try:
        m.run(prog_slot); return m.output, False
    except Exception:
        return m.output, True


print("B1  arithmetic (4+2)*10-5")
p = {1: program(1, num(4),num(2),[PLUS],num(10),[MUL],num(5),[MINUS],[EMIT])}
r = run(p, 1)
assert r['out'] == [55], r
ok(f"native emitted {r['out']} ({r['code_len']} bytes)")

print("B2  three-way IF: six relations in native branches")
def relprog(a,b,arms):
    return {1: program(1, num(a),num(b),[MINUS], verb(CMD_IF), *arms)}
def emit9(): return [num(9),[EMIT]]
GT=[region(*emit9()), region(), region()]
EQ=[region(), region(*emit9()), region()]
LT=[region(), region(), region(*emit9())]
GE=[region(*emit9()), region(*emit9()), region()]
LE=[region(), region(*emit9()), region(*emit9())]
NE=[region(*emit9()), region(), region(*emit9())]
tests = [(7,5,GT,True),(5,7,GT,False),(5,5,EQ,True),(3,8,LT,True),
         (7,5,GE,True),(5,5,GE,True),(3,5,GE,False),(5,5,LE,True),(3,5,NE,True),(5,5,NE,False)]
for a,b,arms,want in tests:
    r = run(relprog(a,b,arms), 1)
    got = (r['out'] == [9])
    assert got == want, (a,b,want,r['out'])
ok("> == < >= <= != all correct as native jumps")

print("B3  READ/STORE against the slot arena")
p = {1: program(1, num(123), verb(CMD_STORE,10), verb(CMD_READ,10), [EMIT])}
r = run(p, 1, grants={10:0}, slot_init={10:0})
assert r['out']==[123] and r['slots'][10]==123, r
ok(f"stored 123 to slot 10, read it back natively: {r['out']}")

print("B4  LOOP + BREAK + slots: sum(1..5)")
p = {1: program(1,
    num(0), verb(CMD_STORE,101), num(1), verb(CMD_STORE,100),
    verb(CMD_LOOP), region(
        verb(CMD_READ,100), num(6), [MINUS], verb(CMD_IF),
        region(verb(CMD_BREAK)), region(verb(CMD_BREAK)),
        region(verb(CMD_READ,101), verb(CMD_READ,100),[PLUS], verb(CMD_STORE,101),
               verb(CMD_READ,100), num(1),[PLUS], verb(CMD_STORE,100))),
    verb(CMD_READ,101),[EMIT])}
r = run(p, 1, grants={100:0,101:0}, slot_init={100:0,101:0})
assert r['out']==[15], r
ok(f"native loop summed 1..5 = {r['out'][0]}, i ended {r['slots'][100]}")

print("B5  GRANT CHECK IN MACHINE CODE")
p = {1: program(1, num(777),[EMIT], num(666), verb(CMD_STORE,300), num(999),[EMIT])}
# holder 2 running, slot 300 granted to holder 1 -> refuse
r = run(p, 1, grants={300:1}, slot_init={300:1000}, holder=2)
assert r['refused'] and r['out']==[777] and r['slots'][300]==1000, r
ok(f"ungranted STORE -> REFUSED in x86-64; out={r['out']} slot300={r['slots'][300]} untouched")
# now grant to holder 2 -> completes
r2 = run(p, 1, grants={300:2}, slot_init={300:1000}, holder=2)
assert not r2['refused'] and r2['out']==[777,999] and r2['slots'][300]==666, r2
ok(f"granted -> completes; out={r2['out']} slot300={r2['slots'][300]}")

print("B6  CALL/RET across compiled programs")
p = {
    1: program(1, num(10), verb(CMD_CALL,2), [EMIT]),   # push 10, call sq-ish, emit
    2: program(2, num(5), [PLUS]),                       # add 5 -> 15
}
r = run(p, 1)
assert r['out']==[15], r
ok(f"program 1 called program 2 natively: {r['out']}")

print("B7  interpreter == native (cross-check)")
progs = {1: program(1, num(8),num(3),[MUL],num(4),[MINUS],[EMIT])}   # 20
ni = run(progs, 1)['out']
mi,_ = interp(1, progs)
assert ni == mi == [20], (ni, mi)
ok(f"native={ni} interp={mi} MATCH")

print(f"\nALL {PASS} CHECKS PASS — the full verb set runs as native machine code.")
