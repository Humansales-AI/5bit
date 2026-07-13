#!/usr/bin/env python3
"""
5basm — the 5bit notation layer ("assembler")
==============================================
Closes the one gap the lexicon leaves open: fingers vs bits.

There is no semantic gap to bridge (the lexicon IS the language; the grid IS
the layout; slots ARE the symbols), so this is not a traditional assembler —
it is a literate notation whose backend is the existing Encoder, with two
properties trad assemblers never had:

  1. DEPTH IS CHECKED, NOT COUNTED.  Humans cannot hand-count START/END
     nesting; raw token lines are depth-tracked and refuse to assemble
     unbalanced or over-deep streams — with line numbers.
  2. IT REFUSES LIKE THE ENCODER REFUSES.  Grant statements route through
     OwnershipEncoder: a program containing two live GRANT_Ws on one slot
     CANNOT BE ASSEMBLED. The invalid binary never exists (encode-time law,
     OWNERSHIP.md §2, lifted to the text layer).

Grammar (one statement per line; ';' comments):

  int  <n>                       signed integer            -> encode_integer
  word "<TEXT>"                  string (mixed case ok)    -> encode_word
  label <pos> "<name>"           field label               -> encode_label
  grant_w <slot> holder <h>      exclusive write grant     (ownership-checked)
  grant_r <slot> holder <h> offset <o>   shared snapshot pin
  revoke  <slot> holder <h>      end write grant           (holder-only)
  auth <owner>                   declare record owner      -> encode_command
  record                         RECORD boundary
  checksum                       CHECKSUM marker
  raw <TOK> <TOK> ...            raw tokens (depth-checked escape hatch)
                                 names: D0..D9 N1..N9 START END RECORD
                                        CHECKSUM PLUS MINUS MUL DIV EQ
                                        LPAREN RPAREN POW SCALE

Outputs: token stream, packed bytes + pad (byte-identical to pack_to_bytes),
and a disassembler for round-trip verification.
"""
from __future__ import annotations

import re
import sys
from typing import List, Optional, Tuple

from binary_grid_db import (
    Token, Encoder, Parser, ParsedNumber, ParsedWord,
    pack_to_bytes, unpack_from_bytes,
)
from griddb_ownership import OwnershipEncoder, EncodeRefused


class AssembleRefused(Exception):
    """Assembly refused. Carries the line number: nothing was emitted."""
    def __init__(self, lineno: int, line: str, why: str):
        self.lineno = lineno
        super().__init__(f"line {lineno}: {why}\n    | {line.strip()}")


# ---- raw-token name table (NUM-context mnemonics + controls) ----
_RAW = {
    **{f'D{i}': Token(i) for i in range(10)},
    **{f'N{i}': Token(16 + i) for i in range(1, 10)},
    'PLUS': Token(10), 'MINUS': Token(11), 'MUL': Token(12), 'DIV': Token(13),
    'EQ': Token(14), 'LPAREN': Token(15), 'RPAREN': Token(16),
    'POW': Token(26), 'SCALE': Token(27),
    'RECORD': Token.RECORD, 'CHECKSUM': Token.CHECKSUM,
    'END': Token.END, 'START': Token.START,
}
_RAW_INV = {v: k for k, v in _RAW.items()}

_MAX_DEPTH = 4  # NUM=0 .. SPECIAL3=4


class Assembler:
    """Text -> tokens -> bytes. Statement backend is the existing Encoder;
    grants go through a real OwnershipEncoder (assemble-time refusal)."""

    def __init__(self, ownership: Optional[OwnershipEncoder] = None):
        self.own = ownership or OwnershipEncoder()

    # ------------------------------------------------------------ statements
    def _stmt_tokens(self, op: str, rest: str, lineno: int, line: str) -> List[Token]:
        try:
            if op == 'int':
                return list(Encoder.encode_integer(int(rest.strip())))
            if op == 'word':
                m = re.fullmatch(r'\s*"(.*)"\s*', rest)
                if not m:
                    raise ValueError('word needs a quoted string')
                return list(Encoder.encode_word(m.group(1)))
            if op == 'label':
                m = re.fullmatch(r'\s*(\d+)\s+"(.*)"\s*', rest)
                if not m:
                    raise ValueError('label needs: <pos> "<name>"')
                return list(Encoder.encode_label(int(m.group(1)), m.group(2)))
            if op == 'auth':
                return list(Encoder.encode_command('AUTH', int(rest.strip())))
            if op == 'grant_w':
                m = re.fullmatch(r'\s*(\d+)\s+holder\s+(\d+)\s*', rest)
                if not m:
                    raise ValueError('grant_w needs: <slot> holder <h>')
                return self.own.grant_w(slot=int(m.group(1)), holder=int(m.group(2)))
            if op == 'grant_r':
                m = re.fullmatch(r'\s*(\d+)\s+holder\s+(\d+)\s+offset\s+(\d+)\s*', rest)
                if not m:
                    raise ValueError('grant_r needs: <slot> holder <h> offset <o>')
                return self.own.grant_r(slot=int(m.group(1)),
                                        holder=int(m.group(2)),
                                        offset=int(m.group(3)))
            if op == 'revoke':
                m = re.fullmatch(r'\s*(\d+)\s+holder\s+(\d+)\s*', rest)
                if not m:
                    raise ValueError('revoke needs: <slot> holder <h>')
                return self.own.revoke(slot=int(m.group(1)), holder=int(m.group(2)))
            if op == 'record':
                return [Token.RECORD]
            if op == 'checksum':
                return [Token.CHECKSUM]
            if op == 'raw':
                toks: List[Token] = []
                for name in rest.split():
                    if name.upper() not in _RAW:
                        raise ValueError(f'unknown raw token {name!r}')
                    toks.append(_RAW[name.upper()])
                return toks
            raise ValueError(f'unknown statement {op!r}')
        except EncodeRefused as e:
            raise AssembleRefused(lineno, line, f'OWNERSHIP REFUSED: {e}') from e
        except ValueError as e:
            raise AssembleRefused(lineno, line, str(e)) from e

    # ------------------------------------------------------------ assembly
    def assemble(self, source: str) -> List[Token]:
        """Full program -> token stream. Depth-checked; refusals carry line
        numbers; a refused program emits NOTHING."""
        tokens: List[Token] = []
        depth = 0
        for lineno, rawline in enumerate(source.splitlines(), 1):
            line = rawline.split(';', 1)[0].strip()
            if not line:
                continue
            op, _, rest = line.partition(' ')
            stmt = self._stmt_tokens(op.lower(), rest, lineno, rawline)

            # depth check every token (raw lines are where humans die)
            for t in stmt:
                if t == Token.START:
                    depth += 1
                    if depth > _MAX_DEPTH:
                        raise AssembleRefused(lineno, rawline,
                            f'depth {depth} exceeds SPECIAL3 ({_MAX_DEPTH})')
                elif t == Token.END:
                    # Per §3/§4: END above NUM pops one context; END *at* NUM
                    # is a value finalizer (integer terminator) — legal, no pop.
                    if depth > 0:
                        depth -= 1
                elif t == Token.RECORD:
                    depth = 0   # RECORD finalizes and resets to NUM (§3)
            tokens.extend(stmt)

        if depth != 0:
            raise AssembleRefused(len(source.splitlines()), '<eof>',
                f'program ends at depth {depth}, not NUM (missing {depth} END)')
        return tokens

    def assemble_bytes(self, source: str) -> Tuple[bytes, int, List[Token]]:
        toks = self.assemble(source)
        data, pad = pack_to_bytes(toks)
        return data, pad, toks


# ---------------------------------------------------------------- disassembly
def disassemble(tokens: List[Token]) -> str:
    """tokens -> notation (literate form). Uses the existing Parser. Empty
    ParsedWord items (context-transition artifacts of START/END descents)
    are filtered; commands re-absorb their arguments so statements
    round-trip: assemble(disassemble(t)) == t for encoder-produced streams."""
    parser = Parser()
    parser.feed_tokens(list(tokens))
    items = [it for it in parser.output
             if not (isinstance(it, ParsedWord) and it.text == '')]
    out: List[str] = []
    i = 0

    def take_nums(j: int, k: int) -> Tuple[List[int], int]:
        nums: List[int] = []
        while j < len(items) and len(nums) < k and isinstance(items[j], ParsedNumber):
            nums.append(items[j].value); j += 1
        return nums, j

    while i < len(items):
        it = items[i]
        if isinstance(it, dict) and it.get('type') == 'command':
            cmd = it['cmd']
            if cmd == 'LABEL':
                # shape: LABEL, word(name), int(position)   (encode_label)
                name, pos = None, None
                j = i + 1
                if j < len(items) and isinstance(items[j], ParsedWord):
                    name = items[j].text; j += 1
                if j < len(items) and isinstance(items[j], ParsedNumber):
                    pos = items[j].value; j += 1
                if name is not None and pos is not None:
                    out.append(f'label {pos} "{name}"'); i = j; continue
            elif cmd == 'AUTH':
                nums, j = take_nums(i + 1, 1)
                if len(nums) == 1:
                    out.append(f'auth {nums[0]}'); i = j; continue
            elif cmd in ('GRANT_W', 'REVOKE'):
                nums, j = take_nums(i + 1, 2)
                if len(nums) == 2:
                    kw = 'grant_w' if cmd == 'GRANT_W' else 'revoke'
                    # grant statements re-emit their trailing RECORD: swallow it
                    if j < len(items) and items[j] is Token.RECORD:
                        j += 1
                    out.append(f'{kw} {nums[1]} holder {nums[0]}'); i = j; continue
            elif cmd == 'GRANT_R':
                nums, j = take_nums(i + 1, 3)
                if len(nums) == 3:
                    if j < len(items) and items[j] is Token.RECORD:
                        j += 1
                    out.append(f'grant_r {nums[1]} holder {nums[0]} offset {nums[2]}')
                    i = j; continue
            out.append(f'; unrecognized command {cmd}')
        elif isinstance(it, ParsedNumber):
            out.append(f'int {it.value}')
        elif isinstance(it, ParsedWord):
            out.append(f'word "{it.text}"')
        elif it is Token.RECORD:
            out.append('record')
        elif it is Token.CHECKSUM:
            out.append('checksum')
        i += 1
    return '\n'.join(out) + '\n'


# ---------------------------------------------------------------- CLI
def main(argv: List[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2
    src_path = argv[1]
    with open(src_path) as f:
        source = f.read()
    asm = Assembler()
    try:
        data, pad, toks = asm.assemble_bytes(source)
    except AssembleRefused as e:
        print(f'REFUSED: {e}', file=sys.stderr)
        return 1
    out_path = src_path.rsplit('.', 1)[0] + '.5b'
    with open(out_path, 'wb') as f:
        f.write(bytes([pad]) + data)
    print(f'{len(toks)} tokens -> {len(data)} bytes (pad {pad}) -> {out_path}')
    print('hex:', data.hex())
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv))
