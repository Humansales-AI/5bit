#!/usr/bin/env python3
"""
5bit Interpreter — tokens that DO instead of MEAN
==================================================
The walker. Everything before this layer is nouns (records, grants, values);
this module adds the verbs. It is the existing §3 stack machine given one more
responsibility: on CALL, execute the record at a slot instead of merely
reading it.

THE VERBS (SPECIAL3 free slots 6..13 — first spend of the 22-slot budget):

    DEF   = 6    "this record is a program"; CALL refuses non-DEF records
    CALL  = 7    run program at slot N; shared value stack carries args/returns
    RET   = 8    early return (end of record = implicit RET)
    IF    = 9    pop t; THREE-WAY dispatch on sign(t): (+arm)(0arm)(-arm);
                 absent arms skip; booleans are the degenerate case
    LOOP  = 10   repeat region until BREAK (all loops = LOOP + IF + BREAK)
    BREAK = 11   unwind to just past the innermost LOOP region
    STORE = 12   pop value, append it as the new version of slot N
                 — GATED BY OWNERSHIP: refuses mid-program without GRANT_W
    READ  = 13   push the current value of slot N
    LOADX = 14   pop index; push slots[index]        — INDEXED read
    STOREX= 15   pop index, pop value; slots[index]=v — INDEXED write (grant-gated)

REGIONS (structured control flow, zero addresses):
    Branch/loop bodies are LPAREN ( ... ) RPAREN regions — the parens already
    in the NUM lexicon (tokens 15/16). Skipping a false branch is balanced-
    paren counting, the same depth discipline the parser lives by. No offsets,
    no GOTO, no jump targets: structure IS the address.

VALUES & MEMORY:
    - Integers: native lexicon encoding (digits + END finalizer) push onto the
      value stack.
    - Arithmetic: NUM operator tokens applied postfix — PLUS MINUS MUL DIV pop
      two, push one. EQ (=) pops and EMITS to program output.
    - Variables are SLOTS. No local-variable machinery: programs READ/STORE
      grid slots, so program state is records — durable, versioned, rewindable
      by the same WAL that rewinds everything else, and access-controlled by
      the same grants. The fabric is the register file.

EFFECTS = GRANTS:
    STORE routes through OwnershipEncoder.write_with_grant. A program running
    as holder H physically cannot append to a slot whose write grant H does
    not hold — EncodeRefused fires MID-PROGRAM, execution halts, the grid is
    untouched. Deny-by-default execution.

Safety: step budget (gas) bounds runaway LOOPs deterministically.
"""
from __future__ import annotations

from typing import Dict, List, Optional

from binary_grid_db import Token, Encoder, pack_to_bytes, unpack_from_bytes
from griddb_ownership import OwnershipEncoder, EncodeRefused

# ---------------------------------------------------------------- verb tokens
CMD_DEF   = Token(6)
CMD_CALL  = Token(7)
CMD_RET   = Token(8)
CMD_IF    = Token(9)
CMD_LOOP  = Token(10)
CMD_BREAK = Token(11)
CMD_STORE = Token(12)
CMD_READ  = Token(13)
CMD_LOADX = Token(18)   # pop index -> push slots[index]        (indexed read)
CMD_STOREX= Token(19)   # pop index, pop value -> slots[index]  (indexed write, grant-checked)

VERB_NAMES = {CMD_DEF: 'DEF', CMD_CALL: 'CALL', CMD_RET: 'RET', CMD_IF: 'IF',
              CMD_LOOP: 'LOOP', CMD_BREAK: 'BREAK', CMD_STORE: 'STORE',
              CMD_READ: 'READ', CMD_LOADX: 'LOADX', CMD_STOREX: 'STOREX'}
_HAS_ARG = {CMD_DEF, CMD_CALL, CMD_STORE, CMD_READ}

LPAREN, RPAREN = Token(15), Token(16)
_OPS = {Token(10): lambda a, b: a + b, Token(11): lambda a, b: a - b,
        Token(12): lambda a, b: a * b, Token(13): lambda a, b: a // b}
EMIT = Token(14)  # '=' pops and emits
_DIGITS = {Token(i): i for i in range(10)}
_NEGS = {Token(16 + i): i for i in range(1, 10)}


# ------------------------------------------------------------ program builder
def verb(cmd: Token, arg: Optional[int] = None) -> List[Token]:
    """Encode a verb in the exact encode_command shape (START×4 cmd END×4
    [+ NUM arg]) so verbs pack/unpack/parse with zero new machinery."""
    toks = [Token.START] * 4 + [cmd] + [Token.END] * 4
    if arg is not None:
        toks += list(Encoder.encode_integer(arg))
    return toks


def num(n: int) -> List[Token]:
    return list(Encoder.encode_integer(n))


def region(*parts: List[Token]) -> List[Token]:
    out: List[Token] = [LPAREN]
    for p in parts:
        out.extend(p)
    out.append(RPAREN)
    return out


def program(slot: int, *parts: List[Token]) -> List[Token]:
    """DEF header + body + RECORD: a complete executable record."""
    out = verb(CMD_DEF, slot)
    for p in parts:
        out.extend(p)
    out.append(Token.RECORD)
    return out


# ------------------------------------------------------------ control signals
class _Break(Exception): pass
class _Return(Exception): pass


class InterpreterError(Exception): pass


class OutOfGas(InterpreterError): pass


# ------------------------------------------------------------------ machine
class Machine:
    """The walker. Programs are records at slots; running as `holder`
    determines what STORE may touch (effects = grants)."""

    def __init__(self, ownership: Optional[OwnershipEncoder] = None,
                 grid=None, holder: int = 0, max_steps: int = 100_000):
        self.own = ownership or OwnershipEncoder()
        self.grid = grid
        self.holder = holder
        self.max_steps = max_steps
        self.programs: Dict[int, List[Token]] = {}
        self.stack: List[int] = []
        self.output: List[int] = []
        self.trace: List[str] = []
        self._steps = 0

    # -- loading: DEF is the executable bit --
    def load(self, slot: int, tokens: List[Token]) -> None:
        """A record is loadable as a program iff it opens with DEF <slot>."""
        toks = [Token(t) for t in tokens]
        head = verb(CMD_DEF, slot)
        if toks[:len(head)] != head:
            raise InterpreterError(
                f"slot {slot}: record has no DEF header — data is not executable")
        self.programs[slot] = toks

    def load_bytes(self, slot: int, data: bytes, pad: int) -> None:
        self.load(slot, unpack_from_bytes(data, pad))

    # -- running --
    def call(self, slot: int) -> None:
        if slot not in self.programs:
            raise InterpreterError(f"CALL {slot}: no DEF'd program at slot")
        toks = self.programs[slot]
        self.trace.append(f"CALL {slot}")
        try:
            self._exec(toks, 0, len(toks))
        except _Return:
            pass
        self.trace.append(f"RET  {slot}")

    def run(self, slot: int) -> List[int]:
        self._steps = 0
        self.call(slot)
        return self.output

    # -- core walk --
    def _gas(self):
        self._steps += 1
        if self._steps > self.max_steps:
            raise OutOfGas(f"step budget {self.max_steps} exhausted")

    def _exec(self, toks: List[Token], pos: int, end: int) -> int:
        while pos < end:
            self._gas()
            t = toks[pos]

            # SPECIAL3 verb: START×4 cmd END×4 [arg]
            if t == Token.START and toks[pos:pos + 4] == [Token.START] * 4:
                cmd = toks[pos + 4]
                pos += 9  # 4 START + cmd + 4 END
                arg = None
                if cmd in _HAS_ARG:
                    arg, pos = self._read_int(toks, pos, end)
                pos = self._dispatch(cmd, arg, toks, pos, end)
                continue

            if t in _DIGITS or t in _NEGS:
                val, pos = self._read_int(toks, pos, end)
                self.stack.append(val)
                continue

            if t in _OPS:
                b, a = self.stack.pop(), self.stack.pop()
                self.stack.append(_OPS[t](a, b))
                pos += 1
                continue

            if t == EMIT:
                v = self.stack.pop()
                self.output.append(v)
                self.trace.append(f"EMIT {v}")
                pos += 1
                continue

            if t == LPAREN:           # bare region: just execute it
                close = self._match(toks, pos, end)
                self._exec(toks, pos + 1, close)
                pos = close + 1
                continue

            if t == Token.END:        # stray finalizer at depth 0: no-op (§4)
                pos += 1
                continue

            if t == Token.RECORD:     # end of program: implicit RET
                raise _Return()

            if t == Token.CHECKSUM:
                pos += 1
                continue

            raise InterpreterError(f"unexpected token {Token(t).name} at {pos}")
        return pos

    # -- verb dispatch --
    def _dispatch(self, cmd: Token, arg: Optional[int],
                  toks: List[Token], pos: int, end: int) -> int:
        name = VERB_NAMES.get(cmd, f'CMD{int(cmd)}')
        if cmd == CMD_DEF:
            return pos                      # header; no runtime effect
        if cmd == CMD_CALL:
            self.call(arg)
            return pos
        if cmd == CMD_RET:
            raise _Return()
        if cmd == CMD_BREAK:
            raise _Break()
        if cmd == CMD_IF:
            # THREE-WAY IF (architect's ruling): the branch sees the full
            # sign of the number — the answer subtraction already computed.
            # IF binds up to three consecutive regions, positionally:
            #     ( +arm ) ( 0arm ) ( -arm )
            # sign(test) picks the arm; an absent arm is a skip. The old
            # boolean style is the degenerate case (1 -> +arm, 0 -> 0arm),
            # so flag-tests keep working unchanged. All six relations fall
            # out of ONE MINUS + region placement, zero new tokens:
            #     a >  b :  a b -  IF (X)            a == b :  a b -  IF ()(X)
            #     a <  b :  a b -  IF ()()(X)        a >= b :  a b -  IF (X)(X)
            #     a <= b :  a b -  IF ()(X)(X)       a != b :  a b -  IF (X)()(X)
            # (shared arms may CALL a common slot instead of duplicating)
            test = self.stack.pop()
            sign = (test > 0) - (test < 0)
            regions = []
            p = pos
            while len(regions) < 3 and p < end and toks[p] == LPAREN:
                close = self._match(toks, p, end)
                regions.append((p + 1, close))
                p = close + 1
            if not regions:
                raise InterpreterError('IF needs at least one ( region )')
            idx = {1: 0, 0: 1, -1: 2}[sign]
            arm = ('+', '0', '-')[idx]
            if idx < len(regions):
                self.trace.append(f"IF {test:+d} -> {arm}-arm")
                self._exec(toks, *regions[idx])
            else:
                self.trace.append(f"IF {test:+d} -> {arm}-arm absent, skip")
            return p
        if cmd == CMD_LOOP:
            open_ = self._expect(toks, pos, LPAREN, 'LOOP needs ( region )')
            close = self._match(toks, open_, end)
            while True:
                self._gas()
                try:
                    self._exec(toks, open_ + 1, close)
                except _Break:
                    self.trace.append("BREAK")
                    break
            return close + 1
        if cmd == CMD_STORE:
            v = self.stack.pop()
            if self.grid is None:
                raise InterpreterError("STORE with no grid attached")
            # THE GATE: effects are grants. Refuses mid-program.
            self.own.write_with_grant(self.grid, arg, self.holder,
                                      [*Encoder.encode_integer(v), Token.RECORD])
            self.trace.append(f"STORE {v} -> slot {arg}")
            return pos
        if cmd == CMD_LOADX:                       # indexed read: slot from stack
            idx = self.stack.pop()
            if self.grid is None:
                raise InterpreterError("LOADX with no grid attached")
            rec = self.grid.read(idx)
            if rec is None:
                self.stack.append(0)               # unset slot reads as 0
            else:
                vals = self._parse_ints(rec.tokens)
                self.stack.append(vals[0] if vals else 0)
            self.trace.append(f"LOADX slot {idx}")
            return pos
        if cmd == CMD_STOREX:                      # indexed write: slot from stack
            idx = self.stack.pop()
            v = self.stack.pop()
            if self.grid is None:
                raise InterpreterError("STOREX with no grid attached")
            self.own.write_with_grant(self.grid, idx, self.holder,
                                      [*Encoder.encode_integer(v), Token.RECORD])
            self.trace.append(f"STOREX {v} -> slot {idx}")
            return pos
        if cmd == CMD_READ:
            if self.grid is None:
                raise InterpreterError("READ with no grid attached")
            rec = self.grid.read(arg)
            if rec is None:
                raise InterpreterError(f"READ slot {arg}: empty")
            vals = [v for v in self._parse_ints(rec.tokens)]
            self.stack.append(vals[0])
            self.trace.append(f"READ slot {arg} -> {vals[0]}")
            return pos
        raise InterpreterError(f"unknown verb {name}")

    # -- helpers --
    @staticmethod
    def _read_int(toks: List[Token], pos: int, end: int):
        digits: List[int] = []
        neg = False
        while pos < end:
            t = toks[pos]
            if t in _DIGITS:
                digits.append(_DIGITS[t]); pos += 1
            elif t in _NEGS:
                digits.append(_NEGS[t]); neg = True; pos += 1
            elif t == Token.END:
                pos += 1; break
            else:
                break
        val = 0
        for d in digits:
            val = val * 10 + d
        return (-val if neg else val), pos

    @staticmethod
    def _parse_ints(tokens: List[Token]) -> List[int]:
        vals, digits, neg = [], [], False
        for t in tokens:
            t = Token(t)
            if t in _DIGITS: digits.append(_DIGITS[t])
            elif t in _NEGS: digits.append(_NEGS[t]); neg = True
            elif t in (Token.END, Token.RECORD) and digits:
                v = 0
                for d in digits: v = v * 10 + d
                vals.append(-v if neg else v); digits, neg = [], False
        return vals

    @staticmethod
    def _expect(toks, pos, want, why) -> int:
        if pos >= len(toks) or toks[pos] != want:
            raise InterpreterError(why)
        return pos

    @staticmethod
    def _match(toks: List[Token], open_pos: int, end: int) -> int:
        """Balanced-paren skip: structure is the address."""
        depth = 0
        for i in range(open_pos, end):
            if toks[i] == LPAREN: depth += 1
            elif toks[i] == RPAREN:
                depth -= 1
                if depth == 0: return i
        raise InterpreterError("unbalanced region ( ... )")
