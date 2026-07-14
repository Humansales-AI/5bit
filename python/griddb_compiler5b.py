"""compiler.5b — the compiler WRITTEN IN 5bit, emitting x86-64 bytes.

Reads source tokens from SRC[0..n-1], emits machine-code bytes to OUT[0..m-1],
using INDEXED memory (LOADX/STOREX) and a LOOP. Pure verbs. No Python logic.

Compiles the arithmetic+EMIT subset:
   literal d : mov rax,d ; push        -> bytes 48 B8 <d,8> 50   (d single-digit here)
   +         : 5B 58 48 01 D8 50
   -         : 5B 58 48 29 D8 50
   *         : 5B 58 48 0F AF C3 50
   EMIT/end  : 58 C3

Memory:
  SRC_N slot, SRC_BASE+i source tokens
  OUT_N slot, OUT_BASE+j emitted bytes
  IP source cursor, OP output cursor, TK current token
"""
import sys, shutil, mmap, ctypes
sys.path.insert(0, '.')
from griddb_interp import (Machine, verb, num, region, program,
    CMD_LOADX, CMD_STOREX, CMD_READ, CMD_STORE, CMD_IF, CMD_LOOP, CMD_BREAK)
from griddb_ownership import OwnershipEncoder
from griddb_alloc import AllocGrid
from binary_grid_db import Token

PLUS,MINUS,MUL,EMIT = Token(10),Token(11),Token(12),Token(14)

SRC_N, OUT_N, IP, OP, TK = 10, 11, 20, 21, 22
SRC_BASE, OUT_BASE = 100, 1000

def emit_byte_seq(byte_list):
    """Program fragment: append each byte in byte_list to OUT[OP], OP++ ."""
    frag = []
    for b in byte_list:
        # OUT[OP] = b   (value b, index OUT_BASE+OP)
        frag += [num(b), num(OUT_BASE), verb(CMD_READ, OP), [PLUS], verb(CMD_STOREX)]
        # OP = OP + 1
        frag += [verb(CMD_READ, OP), num(1), [PLUS], verb(CMD_STORE, OP)]
    return frag

def le64(v):
    return [(v >> (8*k)) & 0xFF for k in range(8)]

# The compiler program: for each source token, dispatch on its value and emit.
# We dispatch via a chain of three-way IFs on (token - K) for each opcode K.
# Subset tokens: digits 0-9 (emit literal), 10=+, 11=-, 12=*, 14=EMIT.
def build_compiler():
    body = [num(0), verb(CMD_STORE, IP), num(0), verb(CMD_STORE, OP)]
    loop_body = [
        verb(CMD_READ, IP), verb(CMD_READ, SRC_N), [MINUS], verb(CMD_IF),
        region(verb(CMD_BREAK)),           # ip > n : done
        region(verb(CMD_BREAK)),           # ip == n : done
        region(*_dispatch_current()),      # ip < n : compile token
    ]
    body += [verb(CMD_LOOP), region(*loop_body)]
    # write OUT_N = OP
    body += [verb(CMD_READ, OP), verb(CMD_STORE, OUT_N)]
    return program(1, *body)

def _dispatch_current():
    # TK = SRC[SRC_BASE+ip]
    frag = [num(SRC_BASE), verb(CMD_READ, IP), [PLUS], verb(CMD_LOADX), verb(CMD_STORE, TK)]
    # advance ip early (so BREAKs in emit fragments aren't needed)
    frag += [verb(CMD_READ, IP), num(1), [PLUS], verb(CMD_STORE, IP)]
    # if TK <= 9 -> literal.  test (TK - 10): negative => digit
    frag += [
        verb(CMD_READ, TK), num(10), [MINUS], verb(CMD_IF),
        region(*_op_dispatch()),           # +arm: TK >= 10 -> operator/emit
        region(*_op_dispatch()),           # 0arm: TK==10 -> '+'
        region(*_emit_literal()),          # -arm: TK < 10 -> digit literal
    ]
    return frag

def _emit_literal():
    # mov rax, TK ; push  = 48 B8 <TK as 8 bytes LE> 50
    # TK is 0..9 so low byte = TK, rest 0.
    seq = []
    # 48 B8
    seq += emit_byte_seq([0x48, 0xB8])
    # low byte = TK (dynamic): OUT[OP]=TK ; OP++
    seq += [verb(CMD_READ, TK), num(OUT_BASE), verb(CMD_READ, OP), [PLUS], verb(CMD_STOREX),
            verb(CMD_READ, OP), num(1), [PLUS], verb(CMD_STORE, OP)]
    # 7 zero bytes
    seq += emit_byte_seq([0,0,0,0,0,0,0])
    # 50 (push rax)
    seq += emit_byte_seq([0x50])
    return seq

def _op_dispatch():
    # TK is 10,11,12, or 14. Dispatch by (TK-11): 10->neg,11->zero,12->pos,14->pos
    # Simplive: nested tests.
    # test TK-10==0 -> '+'
    plus  = emit_byte_seq([0x5B,0x58,0x48,0x01,0xD8,0x50])
    minus = emit_byte_seq([0x5B,0x58,0x48,0x29,0xD8,0x50])
    mul   = emit_byte_seq([0x5B,0x58,0x48,0x0F,0xAF,0xC3,0x50])
    emit_ = emit_byte_seq([0x58,0xC3])
    # chain: (TK-10): 0 -> plus ; else (TK-11):0->minus; else (TK-12):0->mul; else emit
    return [
        verb(CMD_READ,TK), num(10), [MINUS], verb(CMD_IF),
        region(   # TK>10
            verb(CMD_READ,TK), num(11),[MINUS], verb(CMD_IF),
            region(  # TK>11
                verb(CMD_READ,TK), num(12),[MINUS], verb(CMD_IF),
                region(*emit_),      # TK>12 -> assume EMIT(14)
                region(*mul),        # TK==12 -> *
                region(*emit_)),     # TK<12 (shouldn't happen) -> emit
            region(*minus),          # TK==11 -> -
            region(*mul)),           # TK<11 (i.e. would be caught above) 
        region(*plus),               # TK==10 -> +
        region(*plus),               # TK<10 (unreached; digit path handles)
    ]

def run_compiler(src_tokens):
    shutil.rmtree('/tmp/c5b', ignore_errors=True)
    g = AllocGrid(data_dir='/tmp/c5b'); own = OwnershipEncoder()
    slots = [SRC_N, OUT_N, IP, OP, TK] + [SRC_BASE+i for i in range(len(src_tokens))] + [OUT_BASE+j for j in range(400)]
    for s in slots: own.grant_w(slot=s, holder=0)
    m = Machine(ownership=own, grid=g, holder=0, max_steps=2_000_000)
    # write source length + tokens
    def wr(slot, v):
        from binary_grid_db import Encoder
        own.write_with_grant(g, slot, 0, [*Encoder.encode_integer(v), Token.RECORD])
    wr(SRC_N, len(src_tokens))
    for i,t in enumerate(src_tokens): wr(SRC_BASE+i, int(t))
    m.load(1, build_compiler())
    m.run(1)
    # collect OUT bytes
    from binary_grid_db import Encoder
    n = m._parse_ints(g.read(OUT_N).tokens)[0]
    out = []
    for j in range(n):
        rec = g.read(OUT_BASE+j)
        out.append(m._parse_ints(rec.tokens)[0] & 0xFF)
    return bytes(out)

def exec_native(code):
    buf = mmap.mmap(-1, max(len(code),64), prot=mmap.PROT_READ|mmap.PROT_WRITE|mmap.PROT_EXEC)
    buf.write(code)
    ptr = ctypes.cast((ctypes.c_char*len(code)).from_buffer(buf), ctypes.c_void_p)
    return ctypes.CFUNCTYPE(ctypes.c_int64)(ptr.value)()

if __name__ == '__main__':
    # source: 4 2 + 3 *  = (4+2)*3 = 18   (single-digit operands)
    src = [Token(4), Token(2), PLUS, Token(3), MUL, EMIT]
    print("compiling 5bit source [4 2 + 3 * EMIT] with the 5bit compiler...")
    code = run_compiler(src)
    print("  emitted", len(code), "bytes of x86-64:", code.hex())
    result = exec_native(code)
    print("  native CPU result:", result, "(expect 18)")
    assert result == 18
    print("\n  *** THE 5BIT COMPILER, WRITTEN IN 5BIT, EMITTED NATIVE CODE")
    print("      THAT THE CPU RAN. No Python logic in the compile — only the")
    print("      Machine walking tokens. On fivebit_interp.c: zero Python.")
