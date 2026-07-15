#!/usr/bin/env python3
"""Generate compiler.5ba — 5basm source for the 5bit→x86-64 compiler.

This is a MACRO ASSEMBLER. It reads the lowering table (token→x86-64 bytes)
and emits 5basm notation. The output is assembled by griddb_asm.py into
compiler.5b tokens. Python is the toolchain, not the runtime.

Usage: python3 gen_compiler_5ba.py > compiler.5ba
       python3 griddb_asm.py compiler.5ba   # -> compiler.5b
"""

VERB = {
    'DEF': 'D6', 'CALL': 'D7', 'RET': 'D8', 'IF': 'D9',
    'LOOP': 'PLUS', 'BREAK': 'MINUS',
    'STORE': 'MUL', 'READ': 'DIV',
    'LOADX': 'N2', 'STOREX': 'N3',
}

def S(cmd, arg=None):
    """Emit a SPECIAL3 verb: raw STARTx4 cmd ENDx4 [arg]. arg is a 5basm 'int N' line."""
    v = VERB[cmd]
    s = f"raw START START START START {v} END END END END"
    if arg is not None:
        s += f"\nint {arg}"
    return s

def N(n):
    """Emit an integer literal."""
    return f"int {n}"

def byte_emit(b):
    """Emit one byte to OUT[OP] using STOREX, then OP++."""
    lines = []
    lines.append(N(b))
    lines.append(N(1000))              # OUT_BASE
    lines.append(S('READ', 21))        # READ OP (slot 21)
    lines.append("raw PLUS")
    lines.append(S('STOREX'))          # STOREX: store byte at OUT_BASE+OP
    lines.append(S('READ', 21))        # READ OP
    lines.append(N(1))
    lines.append("raw PLUS")
    lines.append(S('STORE', 21))       # STORE OP: OP++
    return lines

def emit_bytes(bs):
    """Emit a list of bytes."""
    lines = []
    for b in bs:
        lines.extend(byte_emit(b))
    return lines

def chain(remaining, handler, indent=0):
    """Build a three-way IF dispatch chain.
    remaining: list of (token_value, handler_label)
    Returns 5basm lines for the chain."""
    if not remaining:
        return []
    val, name = remaining[0]
    rest = remaining[1:]
    pad = "  " * indent

    lines = []
    lines.append(f"{pad}; -- test TK == {val} ({name}) --")
    lines.append(S('READ', 22))         # READ TK (slot 22)
    lines.append(N(val))
    lines.append("raw MINUS")           # TK - val
    lines.append(S('IF'))
    lines.append("raw LPAREN")          # +arm: TK > val, try next
    if rest:
        lines.extend(chain(rest, None, indent + 1))
    else:
        lines.append("  ; TK > max — ignore (END, START, RECORD, verbs)")
    lines.append("raw RPAREN")
    lines.append("raw LPAREN")          # 0arm: TK == val — compile
    lines.extend(handler())
    lines.append("raw RPAREN")
    lines.append("raw LPAREN")          # -arm: impossible at this level
    lines.append("raw RPAREN")
    return lines

def digit_handler(d):
    """Handler for literal digit d: mov rax, d; push rax."""
    def h():
        return emit_bytes([0x48, 0xB8] + [(d >> (8*k)) & 0xFF for k in range(8)] + [0x50])
    return h

def op_handler(op_bytes):
    """Handler for an operator."""
    def h():
        return emit_bytes(op_bytes)
    return h


def generate():
    lines = []
    lines.append("; compiler.5ba — 5bit→x86-64 compiler in 5basm")
    lines.append("; Assembled by griddb_asm.py → compiler.5b")
    lines.append("; Loaded by bootstrap.c → emits native x86-64 → CPU executes")
    lines.append("")
    lines.append("; ---- Init ----")
    lines.append(N(0) + " " + S('STORE', 20))  # IP=0
    lines.append(N(0) + " " + S('STORE', 21))  # OP=0
    lines.append("")

    lines.append("; ---- Main loop ----")
    lines.append(S('LOOP'))
    lines.append("raw LPAREN")
    lines.append("  ; Load token[IP] → TK")
    lines.append("  " + N(100))               # SRC_BASE
    lines.append("  " + S('READ', 20))         # READ IP
    lines.append("  raw PLUS")
    lines.append("  " + S('LOADX'))
    lines.append("  " + S('STORE', 22))        # STORE TK

    lines.append("  ; IP++")
    lines.append("  " + S('READ', 20))
    lines.append("  " + N(1))
    lines.append("  raw PLUS")
    lines.append("  " + S('STORE', 20))

    lines.append("  ; Check IP >= N → break")
    lines.append("  " + S('READ', 20))         # READ IP
    lines.append("  " + S('READ', 10))         # READ SRC_N
    lines.append("  raw MINUS")
    lines.append("  " + S('IF'))
    lines.append("  raw LPAREN")              # +arm: IP > N → break
    lines.append("    " + S('BREAK'))
    lines.append("  raw RPAREN")
    lines.append("  raw LPAREN")              # 0arm: IP == N → break
    lines.append("    " + S('BREAK'))
    lines.append("  raw RPAREN")
    lines.append("  raw LPAREN")              # -arm: IP < N → dispatch
    lines.append("")

    # ---- Dispatch chain for tokens 0-14 ----
    tokens = [
        (0, "0", digit_handler(0)),
        (1, "1", digit_handler(1)),
        (2, "2", digit_handler(2)),
        (3, "3", digit_handler(3)),
        (4, "4", digit_handler(4)),
        (5, "5", digit_handler(5)),
        (6, "6", digit_handler(6)),
        (7, "7", digit_handler(7)),
        (8, "8", digit_handler(8)),
        (9, "9", digit_handler(9)),
        (10, "PLUS", op_handler([0x5B, 0x58, 0x48, 0x01, 0xD8, 0x50])),
        (11, "MINUS", op_handler([0x5B, 0x58, 0x48, 0x29, 0xD8, 0x50])),
        (12, "MUL", op_handler([0x5B, 0x58, 0x48, 0x0F, 0xAF, 0xC3, 0x50])),
        (13, "DIV", op_handler([0x5B, 0x58, 0x48, 0x99, 0x48, 0xF7, 0xFB, 0x50])),
        (14, "EMIT", op_handler([0x58, 0xC3])),
    ]

    # Build chain: test each token in order
    chain_lines = []
    for i, (val, name, handler) in enumerate(tokens):
        chain_lines.append(f"    ; --- TK == {val} ({name}) ---")
        chain_lines.append(f"    {S('READ', 22)}")
        chain_lines.append(f"    {N(val)}")
        chain_lines.append(f"    raw MINUS")
        chain_lines.append(f"    {S('IF')}")
        if i < len(tokens) - 1:
            chain_lines.append(f"    raw LPAREN")
            chain_lines.append(f"      ; TK > {val} — try next")
            # Remaining tokens will be chained in the +arm of the NEXT level
            chain_lines.append(f"    raw RPAREN")
        else:
            chain_lines.append(f"    raw LPAREN")
            chain_lines.append(f"      ; TK > 14 — ignore (END, START, RECORD, verbs)")
            chain_lines.append(f"    raw RPAREN")
        chain_lines.append(f"    raw LPAREN")
        for hl in handler():
            chain_lines.append(f"      {hl}")
        chain_lines.append(f"    raw RPAREN")
        chain_lines.append(f"    raw LPAREN")
        chain_lines.append(f"    raw RPAREN")

    lines.extend(chain_lines)

    lines.append("  raw RPAREN")  # close -arm region of IP<N check
    lines.append("raw RPAREN")    # close LOOP body

    # Trailing ret
    lines.append("")
    lines.append("; ---- Trailing RET ----")
    lines.append(N(195))  # ret = 0xC3
    lines.append(N(1000))
    lines.append(S('READ', 21))
    lines.append("raw PLUS")
    lines.append(S('STOREX'))
    lines.append(S('READ', 21))
    lines.append(N(1))
    lines.append("raw PLUS")
    lines.append(S('STORE', 21))

    # Write OUT_N
    lines.append("")
    lines.append("; ---- Write OUT_N ----")
    lines.append(S('READ', 21))
    lines.append(S('STORE', 11))

    # EMIT 42
    lines.append(N(42))
    lines.append("raw EQ")
    lines.append("")
    lines.append("record")

    return "\n".join(lines)


if __name__ == '__main__':
    import sys
    src = generate()
    if len(sys.argv) > 1:
        with open(sys.argv[1], 'w') as f:
            f.write(src)
        print(f"Written {len(src.splitlines())} lines to {sys.argv[1]}", file=sys.stderr)
    else:
        print(src)
