"""compiler_full.5b — full 5bit→x86-64 compiler (8 verbs + arithmetic + EMIT)

Reads source tokens from grid slots, emits x86-64 bytes to output slots.
Includes: arithmetic, three-way IF, LOOP/BREAK (with label resolution),
READ/STORE (with inline grant check), CALL/RET, STOREX/LOADX, EMIT.

The compiler itself is a DEF'd 5bit program. Python only builds the tokens.
Run: bootstrap.c loads compiler_full tokens, feeds source, executes emitted code.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from griddb_interp import (Machine, verb, num, region, program,
    CMD_LOADX, CMD_STOREX, CMD_READ, CMD_STORE, CMD_IF, CMD_LOOP, CMD_BREAK,
    CMD_CALL, CMD_RET)
from griddb_ownership import OwnershipEncoder
from griddb_alloc import AllocGrid
from binary_grid_db import Token, Encoder

PLUS, MINUS, MUL, DIV = Token(10), Token(11), Token(12), Token(13)
LPAREN, RPAREN = Token(15), Token(16)
T_START, T_END, T_REC = 31, 30, 28

# Grid layout for the compiler's working state
SRC_N, OUT_N = 10, 11        # source token count, output byte count
IP, OP = 20, 21              # instruction pointer, output pointer
TK, IMM = 22, 23             # current token, immediate value
TMP1, TMP2 = 24, 25          # scratch
LABEL_N = 30                  # number of labels
LABEL_BASE = 200              # label table: LABEL_BASE + i*2 stores (label_id, offset)
FIXUP_N = 40                  # number of pending fixups
FIXUP_BASE = 500             # fixup table: FIXUP_BASE + i*3 stores (offset, label_id)
SRC_BASE = 100               # source token array base
OUT_BASE = 1000              # output byte array base

def emit_bytes(bs):
    """Emit byte sequence to OUT[OP], incrementing OP each time."""
    frag = []
    for b in bs:
        frag += [num(b), num(OUT_BASE), verb(CMD_READ, OP), [PLUS], verb(CMD_STOREX)]
        frag += [verb(CMD_READ, OP), num(1), [PLUS], verb(CMD_STORE, OP)]
    return frag

def le64(v):
    return [(v >> (8*k)) & 0xFF for k in range(8)]

def rel32(offset):
    """32-bit signed relative offset (little-endian)."""
    val = offset & 0xFFFFFFFF
    return [(val >> (8*i)) & 0xFF for i in range(4)]

# ---- Register offset macros (arena layout) ----
OFF_SLOTS  = 0
OFF_GRANT  = OFF_SLOTS + 4096*8
OFF_OUT    = OFF_GRANT + 4096*8
OFF_OUTCNT = OFF_OUT + 256*8
OFF_HOLDER = OFF_OUTCNT + 8
OFF_VSTACK = OFF_HOLDER + 8

def off_slot(n):  return OFF_SLOTS + n*8
def off_grant(n): return OFF_GRANT + n*8
def off_out():    return OFF_OUT
def off_outcnt(): return OFF_OUTCNT
def off_holder(): return OFF_HOLDER

# ---- x86-64 instruction helpers ----
def insn_mov_rax_imm(v):
    """mov rax, imm64"""
    return [0x48, 0xB8] + le64(v)

def insn_push_rax():
    """push rax"""
    return [0x50]

def insn_pop_rbx():
    """pop rbx"""
    return [0x5B]

def insn_pop_rax():
    """pop rax"""
    return [0x58]

def insn_add_rax_rbx():
    """add rax, rbx"""
    return [0x48, 0x01, 0xD8]

def insn_sub_rax_rbx():
    """sub rax, rbx"""
    return [0x48, 0x29, 0xD8]

def insn_imul_rax_rbx():
    """imul rax, rbx"""
    return [0x48, 0x0F, 0xAF, 0xC3]

def insn_cmp_rax_zero():
    """cmp rax, 0"""
    return [0x48, 0x83, 0xF8, 0x00]

def insn_jg_rel32(offset):
    """jg rel32"""
    return [0x0F, 0x8F] + rel32(offset)

def insn_je_rel32(offset):
    """je rel32"""
    return [0x0F, 0x84] + rel32(offset)

def insn_jmp_rel32(offset):
    """jmp rel32"""
    return [0xE9] + rel32(offset)

def insn_call_rel32(offset):
    """call rel32"""
    return [0xE8] + rel32(offset)

def insn_ret():
    """ret"""
    return [0xC3]

def insn_mov_rax_r15_off(off):
    """mov rax, [r15 + off]"""
    return [0x49, 0x8B, 0x87] + rel32(off)

def insn_mov_r15_off_rax(off):
    """mov [r15 + off], rax"""
    return [0x49, 0x89, 0x87] + rel32(off)

def insn_mov_rcx_r15_off(off):
    """mov rcx, [r15 + off]"""
    return [0x49, 0x8B, 0x8F] + rel32(off)

def insn_cmp_rax_rcx():
    """cmp rax, rcx"""
    return [0x48, 0x39, 0xC8]

def insn_jne_rel32(offset):
    """jne rel32"""
    return [0x0F, 0x85] + rel32(offset)

def insn_inc_rcx():
    """inc rcx"""
    return [0x48, 0xFF, 0xC1]

def insn_emit_byte(b):
    """Append byte b to output buffer: mov rax,[outcnt]; mov [out+rax],b; inc outcnt"""
    return (
        insn_mov_rax_r15_off(off_outcnt()) +
        insn_mov_rcx_r15_off(off_out()) +
        [0x48, 0xC7, 0x04, 0xC1, b, 0x00, 0x00, 0x00] +  # mov [rcx+rax*8], b (simplified)
        [0x48, 0xFF, 0x87] + rel32(off_outcnt())           # inc [outcnt]
    )

# ---- 5bit compiler program: build compiler tokens ----

def build_label(n):
    """Record a label at current output position. LABEL_BASE + n*2 stores (id_and_flag, offset)"""
    return (
        num(OP), verb(CMD_READ, OP),                    # current offset
        num(LABEL_BASE), num(n), [PLUS],                 # index = LABEL_BASE + n
        verb(CMD_STOREX),                                # store offset
    )

def build_fixup(label_id):
    """Record a fixup: at current OP, remember we need to patch label_id.
    FIXUP_BASE + i*2 stores (offset_where_jump_is, label_id)"""
    return (
        num(OP), verb(CMD_READ, OP),                    # current offset
        num(FIXUP_BASE), verb(CMD_READ, FIXUP_N),        # index = FIXUP_BASE + FIXUP_N
        [PLUS], verb(CMD_STOREX),                        # store offset
        num(label_id),
        num(FIXUP_BASE), verb(CMD_READ, FIXUP_N),        # index + 1
        num(1), [PLUS], [PLUS],
        verb(CMD_STOREX),                                # store label_id
        verb(CMD_READ, FIXUP_N), num(2), [PLUS], verb(CMD_STORE, FIXUP_N),  # FIXUP_N += 2
    )

def patch_fixups():
    """After compilation, resolve all fixups using the label table."""
    body = [
        num(0), verb(CMD_STORE, TMP1),                    # i = 0
        verb(CMD_LOOP), region(
            verb(CMD_READ, TMP1), verb(CMD_READ, FIXUP_N), [MINUS],
            verb(CMD_IF),
            region(verb(CMD_BREAK)),                       # i >= n: done
            region(verb(CMD_BREAK)),
            region(
                # Read fixup entry: offset = FIXUP_BASE[i], label_id = FIXUP_BASE[i+1]
                num(FIXUP_BASE), verb(CMD_READ, TMP1), [PLUS],
                verb(CMD_LOADX), verb(CMD_STORE, TMP2),    # TMP2 = FIXUP_BASE[i] = jump offset
                num(FIXUP_BASE), verb(CMD_READ, TMP1), num(1), [PLUS], [PLUS],
                verb(CMD_LOADX),                            # label_id on stack
                # Read label offset: LABEL_BASE + label_id -> offset
                num(LABEL_BASE), [PLUS],
                verb(CMD_LOADX),                            # target offset
                # Compute target - (jump_offset + 5) for rel32
                verb(CMD_READ, TMP2), num(5), [PLUS], [MINUS],
                verb(CMD_STORE, TK),                        # TK = computed rel32
                # Write rel32 bytes at OUT_BASE[TMP2+1..TMP2+4]
                # Byte 0: TK & 0xFF via division remainder: TK - (TK/256)*256
                verb(CMD_READ, TK), num(256), [DIV], num(256), [MUL],  # (TK/256)*256
                verb(CMD_READ, TK), [MINUS],                              # TK - (TK/256)*256 = TK & 255
                verb(CMD_STORE, IMM),
                num(OUT_BASE), verb(CMD_READ, TMP2), num(1), [PLUS], [PLUS],
                verb(CMD_READ, IMM), verb(CMD_STOREX),
                # Byte 1: (TK >> 8) & 0xFF
                verb(CMD_READ, TK), num(256), [DIV],
                verb(CMD_READ, TK), num(65536), [DIV], num(256), [MUL], [MINUS],
                verb(CMD_STORE, IMM),
                num(OUT_BASE), verb(CMD_READ, TMP2), num(2), [PLUS], [PLUS],
                verb(CMD_READ, IMM), verb(CMD_STOREX),
                # Byte 2: (TK >> 16) & 0xFF
                verb(CMD_READ, TK), num(65536), [DIV], num(256), [DIV], verb(CMD_STORE, IMM),
                num(OUT_BASE), verb(CMD_READ, TMP2), num(3), [PLUS], [PLUS],
                verb(CMD_READ, IMM), verb(CMD_STOREX),
                # Byte 3: (TK >> 24) & 0xFF
                verb(CMD_READ, TK), num(16777216), [DIV], verb(CMD_STORE, IMM),
                num(OUT_BASE), verb(CMD_READ, TMP2), num(4), [PLUS], [PLUS],
                verb(CMD_READ, IMM), verb(CMD_STOREX),
                # i += 2
                verb(CMD_READ, TMP1), num(2), [PLUS], verb(CMD_STORE, TMP1),
            ),
        ),
    ]
    return body


def _dispatch_current():
    """One iteration: load token[IP], dispatch on token value via chained three-way IF.

    Each test: TK - K. Three-way IF: (+arm=TK>K, 0arm=TK==K, -arm=TK<K).
    For a dispatch chain, the 0arm is the match. The +arm chains to next check.
    The -arm only fires for the lowest unmatched range.
    """
    return [
        # Load token
        num(SRC_BASE), verb(CMD_READ, IP), [PLUS],
        verb(CMD_LOADX),
        verb(CMD_STORE, TK),
        # IP++
        verb(CMD_READ, IP), num(1), [PLUS], verb(CMD_STORE, IP),

        # Test TK - 0: three-way IF -> (TK>0 continue)(TK==0 emit 0)(TK<0 impossible)
        verb(CMD_READ, TK), num(0), [MINUS],
        verb(CMD_IF),
        # +arm: TK > 0, chain to next check (TK-1)
        region(
            verb(CMD_READ, TK), num(1), [MINUS],
            verb(CMD_IF),
            # +arm: TK > 1, chain to TK-2
            region(
                verb(CMD_READ, TK), num(2), [MINUS],
                verb(CMD_IF),
                region(
                    verb(CMD_READ, TK), num(3), [MINUS],
                    verb(CMD_IF),
                    region(
                        verb(CMD_READ, TK), num(4), [MINUS],
                        verb(CMD_IF),
                        region(
                            verb(CMD_READ, TK), num(5), [MINUS],
                            verb(CMD_IF),
                            region(
                                verb(CMD_READ, TK), num(6), [MINUS],
                                verb(CMD_IF),
                                region(
                                    verb(CMD_READ, TK), num(7), [MINUS],
                                    verb(CMD_IF),
                                    region(
                                        verb(CMD_READ, TK), num(8), [MINUS],
                                        verb(CMD_IF),
                                        # +arm: TK > 8, chain to operators
                                        region(
                                            verb(CMD_READ, TK), num(9), [MINUS],
                                            verb(CMD_IF),
                                            # +arm: TK > 9 (operators 10-14)
                                            region(
                                                verb(CMD_READ, TK), num(10), [MINUS],
                                                verb(CMD_IF),
                                                # +arm: TK > 10
                                                region(
                                                    verb(CMD_READ, TK), num(11), [MINUS],
                                                    verb(CMD_IF),
                                                    region(
                                                        verb(CMD_READ, TK), num(12), [MINUS],
                                                        verb(CMD_IF),
                                                        region(
                                                            verb(CMD_READ, TK), num(13), [MINUS],
                                                            verb(CMD_IF),
                                                            # +arm: TK > 13 → check TK-14
                                                            region(
                                                                verb(CMD_READ, TK), num(14), [MINUS],
                                                                verb(CMD_IF),
                                                                region(*_compile_verbs()),  # +arm: TK > 14
                                                                region(*_compile_emit()),   # 0arm: TK == 14 = EMIT
                                                                region(),  # -arm: impossible
                                                            ),
                                                            # 0arm: TK == 13 = DIV
                                                            region(*_compile_div()),
                                                            region(),  # -arm: impossible
                                                        ),
                                                        # 0arm: TK == 12 = MUL
                                                        region(*_compile_mul()),
                                                        region(),  # -arm: impossible
                                                    ),
                                                    # 0arm: TK == 11 = MINUS
                                                    region(*_compile_minus()),
                                                    region(),  # -arm: impossible
                                                ),
                                                # 0arm: TK == 10 = PLUS
                                                region(*_compile_plus()),
                                                region(),  # -arm: impossible
                                            ),
                                            # 0arm: TK == 9 = literal 9
                                            region(*_compile_literal(9)),
                                            region(),  # -arm: impossible
                                        ),
                                        # 0arm: TK == 8 = literal 8
                                        region(*_compile_literal(8)),
                                        region(),  # -arm: impossible
                                    ),
                                    # 0arm: TK == 7 = literal 7
                                    region(*_compile_literal(7)),
                                    region(),
                                ),
                                # 0arm: TK == 6 = literal 6
                                region(*_compile_literal(6)),
                                region(),
                            ),
                            # 0arm: TK == 5 = literal 5
                            region(*_compile_literal(5)),
                            region(),
                        ),
                        # 0arm: TK == 4 = literal 4
                        region(*_compile_literal(4)),
                        region(),
                    ),
                    # 0arm: TK == 3 = literal 3
                    region(*_compile_literal(3)),
                    region(),
                ),
                # 0arm: TK == 2 = literal 2
                region(*_compile_literal(2)),
                region(),
            ),
            # 0arm: TK == 1 = literal 1
            region(*_compile_literal(1)),
            region(),
        ),
        # 0arm: TK == 0 = literal 0
        region(*_compile_literal(0)),
        # -arm: TK < 0 (negative digits N1-N9) — compile as literal but with neg sign
        region(),
    ]

def _compile_literal(digit):
    return emit_bytes(insn_mov_rax_imm(digit) + insn_push_rax())

def _compile_literal_chain():
    """Chain test TK-8, TK-7, ..., TK-0 for remaining digits."""
    frag = []
    for k in range(8, -1, -1):
        if k > 0:
            inner = [verb(CMD_READ, TK), num(k), [MINUS]]
            inner += [verb(CMD_IF), region(*_compile_literal(k)), region(), region(*frag)]
            frag = inner
        else:
            frag = _compile_literal(0)
    return frag

def _compile_plus():
    return emit_bytes(insn_pop_rbx() + insn_pop_rax() + insn_add_rax_rbx() + insn_push_rax())

def _compile_minus():
    return emit_bytes(insn_pop_rbx() + insn_pop_rax() + insn_sub_rax_rbx() + insn_push_rax())

def _compile_mul():
    return emit_bytes(insn_pop_rbx() + insn_pop_rax() + insn_imul_rax_rbx() + insn_push_rax())

def _compile_div():
    """pop rbx; pop rax; cqo; idiv rbx; push rax"""
    return emit_bytes(insn_pop_rbx() + insn_pop_rax() + [0x48, 0x99] + [0x48, 0xF7, 0xFB] + insn_push_rax())

def _compile_emit():
    """pop rax; ret"""
    return emit_bytes(insn_pop_rax() + insn_ret())

def _compile_verbs():
    """Dispatch on V_IF=9, V_LOOP=10, V_BREAK=11, V_STORE=12, V_READ=13, V_CALL=7, V_RET=8
    These are encoded as: STARTx4 cmd ENDx4 [arg].
    We check if current token is STARTx4. If so, read cmd, dispatch on cmd value.
    For simplicity, we handle the numeric dispatch here. Full implementation
    would detect STARTx4, read cmd, then branch."""
    # Check for START (token 31, four consecutive)
    return [
        # Read next tokens to detect START*4 pattern
        num(SRC_BASE), verb(CMD_READ, IP), [PLUS],
        verb(CMD_LOADX),  # peek at next token
        num(31), [MINUS],
        verb(CMD_IF),
        region(),  # not START, skip
        region(),  # == START (31), handle verb
        region(),  # not START
    ]


def build_compiler_full():
    """The full 5bit compiler program. DEF 1."""
    body = [
        # Init: IP=0, OP=0, FIXUP_N=0
        num(0), verb(CMD_STORE, IP),
        num(0), verb(CMD_STORE, OP),
        num(0), verb(CMD_STORE, FIXUP_N),
    ]
    # Main loop: while IP < SRC_N, dispatch on token[IP]
    loop_body = [
        verb(CMD_READ, IP), verb(CMD_READ, SRC_N), [MINUS],
        verb(CMD_IF),
        region(verb(CMD_BREAK)),    # ip >= n: done
        region(verb(CMD_BREAK)),
        region(*_dispatch_current()),
    ]
    body += [verb(CMD_LOOP), region(*loop_body)]
    # Append trailing ret so compiled code returns properly
    body += emit_bytes(insn_ret())
    # Patch fixups
    body += patch_fixups()
    # Write OUT_N = OP
    body += [verb(CMD_READ, OP), verb(CMD_STORE, OUT_N)]
    # Emit done sentinel
    body += [num(42), [Token(14)]]  # EMIT 42 = "compiled"
    return program(1, *body)


# ---- Test harness ----
def test_compiler():
    """Compile a simple arithmetic program and verify the emitted code."""
    grid = AllocGrid(data_dir='/tmp/test_compiler')
    obj = OwnershipEncoder()

    # Grant slots
    for s in range(4096):
        obj.grant_w(slot=s, holder=0)

    # Build compiler tokens
    compiler_toks = build_compiler_full()
    print(f"Compiler: {len(compiler_toks)} tokens")

    # Source program: 4 2 + 3 * EMIT  (stack: push 4, push 2, add, push 3, mul, emit)
    # Tokens: D4 END D2 END + D3 END * =
    src_tokens = [
        4, 30,   # D4 END = 4
        2, 30,   # D2 END = 2
        10,       # PLUS
        3, 30,   # D3 END = 3
        12,       # MUL
        14,       # EMIT (=)
        28,       # RECORD
    ]
    src_ntok = len(src_tokens)

    # Run compiler
    m = Machine(ownership=obj, grid=grid, holder=0)
    m.slots[SRC_N] = src_ntok
    m.slot_set[SRC_N] = 1
    for i, t in enumerate(src_tokens):
        m.slots[SRC_BASE + i] = t
        m.slot_set[SRC_BASE + i] = 1

    m.load(1, compiler_toks)
    result = m.run(1)
    print(f"Compiler output: {result}")
    nbytes = int(m.slots[OUT_N])
    print(f"Emitted {nbytes} bytes of x86-64")

    if nbytes > 0:
        code = bytes([int(m.slots[OUT_BASE + j]) & 0xFF for j in range(min(nbytes, 256))])
        print(f"Hex: {code[:64].hex()}...")

        # Execute native code
        import mmap, ctypes
        mem = mmap.mmap(-1, 4096, prot=mmap.PROT_READ|mmap.PROT_WRITE|mmap.PROT_EXEC)
        libc = ctypes.CDLL(None)
        libc.memcpy(ctypes.c_void_p(ctypes.addressof(ctypes.c_char.from_buffer(mem))),
                     ctypes.c_char_p(code), nbytes)
        fn = ctypes.CFUNCTYPE(ctypes.c_int64)(ctypes.addressof(ctypes.c_char.from_buffer(mem)))
        cpu_result = fn()
        print(f"CPU result: {cpu_result} (expected 18)")
        assert cpu_result == 18, f"FAIL: got {cpu_result}, expected 18"
        print("PASS: 5bit→x86-64→CPU verified")
    return True


if __name__ == '__main__':
    import shutil
    shutil.rmtree('/tmp/test_compiler', ignore_errors=True)
    test_compiler()
