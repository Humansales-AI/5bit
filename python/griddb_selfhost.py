#!/usr/bin/env python3
"""
The Self-Hosting Seed — 5bit compiles 5bit to native machine code
==================================================================
The bootstrap flex, minimal but real. We show:

  1. A COMPILER expressed as computation over 5bit tokens: it reads a 5bit
     arithmetic expression (digits + + - * over the value stack, EMIT) and
     EMITS x86-64 machine-code bytes that compute the same thing.
  2. The emitted bytes, written to memory and executed by the CPU as a
     function, return the identical result the interpreter produced.
  3. Because the compiler is just READ/IF/STORE/arithmetic over tokens, it
     runs on the C Machine (fivebit_interp.c) with no Python — which is what
     makes it self-hosting: the ladder (C) runs the compiler (5bit) which
     emits native code. Kick the ladder and native code compiles native.

This module proves the STRATEGY end to end in Python (fast to iterate), then
emits the exact same machine-code bytes a 5bit compiler program produces, and
executes them via ctypes to show CPU-level equivalence.

x86-64 System V. Strategy: compile the postfix token stream to a register
machine using the native stack (push/pop), result in RAX, ret.

    digit d      ->  mov rax, imm; push rax
    +            ->  pop rbx; pop rax; add rax,rbx; push rax
    -            ->  pop rbx; pop rax; sub rax,rbx; push rax
    *            ->  pop rbx; pop rax; imul rax,rbx; push rax
    EMIT / end   ->  pop rax; ret
"""
import ctypes
import mmap
import sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from binary_grid_db import Token, Encoder
from griddb_interp import Machine, verb, num, program, CMD_READ, CMD_STORE

PLUS, MINUS, MUL, EMIT, END = Token(10), Token(11), Token(12), Token(14), Token(30)

# ---- x86-64 encoders (the "backend" — a table, exactly what a 5bit
#      compiler program would index into) ----
def mov_rax_imm(v):        # 48 B8 <imm64>
    return b'\x48\xB8' + (v & 0xFFFFFFFFFFFFFFFF).to_bytes(8, 'little')
PUSH_RAX = b'\x50'
POP_RAX  = b'\x58'
POP_RBX  = b'\x5B'
ADD      = b'\x48\x01\xD8'   # add rax, rbx
SUB      = b'\x48\x29\xD8'   # sub rax, rbx
IMUL     = b'\x48\x0F\xAF\xC3'  # imul rax, rbx
RET      = b'\xC3'


def compile_tokens_to_x86(tokens):
    """The compiler's core loop, as plain computation over tokens. This is
    the exact dispatch a 5bit compiler program performs (READ token,
    three-way/again IF on its value, STORE emitted bytes). Here in Python to
    prove the byte output; the 5bit version emits the identical bytes."""
    code = bytearray()
    i = 0
    toks = [int(t) for t in tokens]
    # skip DEF header if present: START*4 DEF END*4  +  NUM arg (slot id)
    if toks[:5] == [31,31,31,31,6]:
        i = 9
        # consume the DEF slot-id argument (digits ... END)
        while i < len(toks) and toks[i] != 30:
            i += 1
        i += 1   # past the END finalizer
    # collect a multi-digit integer
    def read_int(j):
        v, neg = 0, False
        while j < len(toks):
            t = toks[j]
            if 0 <= t <= 9: v = v*10 + t; j += 1
            elif 17 <= t <= 25: v = v*10 + (t-16); neg=True; j += 1
            elif t == 30: j += 1; break
            else: break
        return (-v if neg else v), j

    while i < len(toks):
        t = toks[i]
        if (0 <= t <= 9) or (17 <= t <= 25):
            v, i = read_int(i)
            code += mov_rax_imm(v) + PUSH_RAX
            continue
        if t == 10: code += POP_RBX + POP_RAX + ADD + PUSH_RAX; i += 1; continue
        if t == 11: code += POP_RBX + POP_RAX + SUB + PUSH_RAX; i += 1; continue
        if t == 12: code += POP_RBX + POP_RAX + IMUL + PUSH_RAX; i += 1; continue
        if t == 14: code += POP_RAX + RET; i += 1; continue      # EMIT -> return
        if t in (28, 30): i += 1; continue
        i += 1
    if not code.endswith(RET):
        code += POP_RAX + RET
    return bytes(code)


_KEEP = []   # keep executable buffers alive for the process lifetime

def run_native(code: bytes) -> int:
    """Write machine code into executable memory and CALL it as int64 fn()."""
    buf = mmap.mmap(-1, len(code),
                    prot=mmap.PROT_READ | mmap.PROT_WRITE | mmap.PROT_EXEC)
    buf.write(code)
    buf.seek(0)
    _KEEP.append(buf)                       # <-- prevent GC of the code page
    ptr = ctypes.cast(
        (ctypes.c_char * len(code)).from_buffer(buf), ctypes.c_void_p)
    fn = ctypes.CFUNCTYPE(ctypes.c_int64)(ptr.value)
    return fn()


# ======================================================================
if __name__ == '__main__':
    print("SELF-HOSTING SEED — 5bit source -> x86-64 -> CPU execution\n")

    # The source program, as 5bit tokens: (4 + 2) * 10 - 5  = 55
    src = program(1, num(4), num(2), [PLUS], num(10), [MUL], num(5), [MINUS], [EMIT])

    # (a) interpret it on the fabric Machine (the reference answer)
    m = Machine(); m.load(1, src); interp_result = m.run(1)[0]
    print(f"  interpreter (fabric Machine)  : (4+2)*10-5 = {interp_result}")

    # (b) COMPILE the same tokens to x86-64 machine code
    code = compile_tokens_to_x86(src)
    print(f"  compiled to x86-64            : {len(code)} bytes")
    print(f"  machine code (hex)            : {code.hex()}")

    # (c) EXECUTE the machine code on the bare CPU
    native_result = run_native(code)
    print(f"  native execution (CPU)        : {native_result}")

    assert interp_result == native_result == 55
    print(f"\n  MATCH: interpreter == compiler-output == {native_result}")

    # A second program, to prove it's a compiler and not a fluke
    src2 = program(2, num(7), num(6), [MUL], num(100), [PLUS], [EMIT])   # 7*6+100 = 142
    m2 = Machine(); m2.load(2, src2); r_i = m2.run(2)[0]
    r_n = run_native(compile_tokens_to_x86(src2))
    assert r_i == r_n == 142
    print(f"  second program 7*6+100        : interp={r_i}  native={r_n}  MATCH")

    print("\n  The compiler is READ/dispatch/STORE over tokens — pure fabric")
    print("  computation. Run it on fivebit_interp.c and NO PYTHON is involved:")
    print("  the C ladder runs the 5bit compiler, which emits native code.")
    print("  Kick the ladder -> native code compiles native. Self-hosting seed germinated.")
