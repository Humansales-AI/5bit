#!/usr/bin/env python3
"""
Native 5bit column comparison — the grid IS the comparator
============================================================
P5 verdict, executable. Two records on the grid are two rows of a matrix.
"XOR" is a VERTICAL READ: walk the columns, mark where the rows diverge.
"Popcount" is COUNTING THE MARKS. No bitwise opcode, no special context,
no new tokens — divergence is a property you observe at a coordinate.

Lanes-as-slots (§12 fixed-width lane vectors): a fingerprint of D lanes
lives at slots base..base+D-1, one integer per lane. The comparator is a
straight-line 5bit program (unrolled column walk — the sequential twin of
the SIMD kernel: same columns, one at a time instead of 64 at once).

Per column i, pure existing verbs:

    READ a_i · READ b_i · MINUS · STORE T        difference
    READ T · IF
       ( +arm:  H+=1        C += T          )    diverged (positive)
       ( 0arm:                              )    identical column
       ( −arm:  H+=1        C += (0 − T)    )    diverged (negative; the
                                                  −arm IS the ABS — the
                                                  three-way IF paying rent)

Then:  READ H = (Hamming / popcount)   READ C = (Manhattan / L1)

The exact-match program is one more three-way IF on H:
    H == 0  → 0-arm → emit 1 (identical)   H > 0 → +arm → emit 0

This module only BUILDS token programs. It never compares anything.
"""
from typing import List

from binary_grid_db import Token
from griddb_interp import (
    verb, num, region, program,
    CMD_IF, CMD_STORE, CMD_READ,
)

PLUS, MINUS = Token(10), Token(11)
EMIT = Token(14)


def build_column_comparator(prog_slot: int, base_a: int, base_b: int,
                            lanes: int, slot_T: int, slot_H: int,
                            slot_C: int) -> List[Token]:
    """A complete DEF'd 5bit program: compares two D-lane fingerprints at
    base_a/base_b, emits [hamming, manhattan]. Pure verbs, zero Python
    at run time."""
    body: List[List[Token]] = [
        num(0), verb(CMD_STORE, slot_H),                 # H = 0  (popcount)
        num(0), verb(CMD_STORE, slot_C),                 # C = 0  (manhattan)
    ]
    for i in range(lanes):                               # the vertical walk
        body += [
            verb(CMD_READ, base_a + i),
            verb(CMD_READ, base_b + i),
            [MINUS], verb(CMD_STORE, slot_T),            # T = a_i - b_i
            verb(CMD_READ, slot_T),
            verb(CMD_IF),
            region(                                      # +arm: diverged
                verb(CMD_READ, slot_H), num(1), [PLUS], verb(CMD_STORE, slot_H),
                verb(CMD_READ, slot_C), verb(CMD_READ, slot_T), [PLUS],
                verb(CMD_STORE, slot_C),
            ),
            region(),                                    # 0arm: same column
            region(                                      # −arm: diverged, ABS
                verb(CMD_READ, slot_H), num(1), [PLUS], verb(CMD_STORE, slot_H),
                verb(CMD_READ, slot_C),
                num(0), verb(CMD_READ, slot_T), [MINUS], [PLUS],
                verb(CMD_STORE, slot_C),
            ),
        ]
    body += [
        verb(CMD_READ, slot_H), [EMIT],                  # emit popcount
        verb(CMD_READ, slot_C), [EMIT],                  # emit manhattan
    ]
    return program(prog_slot, *body)


def build_exact_match(prog_slot: int, slot_H: int) -> List[Token]:
    """Emit 1 iff the last comparison found zero divergent columns."""
    return program(prog_slot,
        verb(CMD_READ, slot_H),
        verb(CMD_IF),
        region(num(0), [EMIT]),        # +arm: divergence exists -> 0
        region(num(1), [EMIT]),        # 0arm: identical -> 1
    )
