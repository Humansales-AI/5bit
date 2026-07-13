#!/usr/bin/env python3
"""
GridDB Rung Packing — the 5 x intN ladder
==========================================
Width-agnostic transport layer for 5-bit token streams.

Primitive identity:  5 units of intN carry exactly N tokens.
    5*int1  =  5 bits = 1 token      5*int8  = 40 bits =  8 tokens
    5*int2  = 10 bits = 2 tokens     5*int16 = 80 bits = 16 tokens
    5*int4  = 20 bits = 4 tokens     ...      5*int64 = 320 bits = 64 tokens

Composition: any token count n decomposes in binary, so any stream packs
EXACTLY as a sum of rungs (largest-first, canonical), with zero format slack:
    n=13 -> 5*int8 + 5*int4 + 5*int1 = 65 bits exact.

Invariants (enforced by tests):
  I1  Round-trip identity per rung:  unpack_units(pack_units(t,N),N) == t
  I2  Width equivalence:             all rungs decode to the same tokens
  I3  Bit-identity with reference:   concatenated rung bits == pack_to_bytes bits
      (the transport leaves no residue; conformance stays token-level)

The parser never sees any of this. Tokens in, tokens out.
"""
from typing import List, Tuple

RUNGS = (64, 32, 16, 8, 4, 2, 1)  # canonical order: largest first


# ---------------------------------------------------------------- single rung

def pack_units(tokens: List[int], n: int) -> List[int]:
    """Pack tokens into units of n bits: every N tokens -> 5 units of intN.

    len(tokens) must be a multiple of n. Units are emitted MSB-first,
    consistent with the reference byte packer.
    """
    if n not in RUNGS:
        raise ValueError(f"unsupported rung int{n}")
    if len(tokens) % n != 0:
        raise ValueError(f"token count {len(tokens)} not a multiple of rung {n}")
    mask = (1 << n) - 1
    units: List[int] = []
    acc = 0
    nbits = 0
    for t in tokens:
        acc = (acc << 5) | (int(t) & 0x1F)
        nbits += 5
        while nbits >= n:
            nbits -= n
            units.append((acc >> nbits) & mask)
    # exactness guaranteed: 5*len(tokens) bits, len % n == 0 -> nbits ends at 0
    assert nbits == 0
    return units


def unpack_units(units: List[int], n: int) -> List[int]:
    """Inverse of pack_units: every 5 units of intN -> N tokens."""
    if n not in RUNGS:
        raise ValueError(f"unsupported rung int{n}")
    if len(units) % 5 != 0:
        raise ValueError(f"unit count {len(units)} not a multiple of 5")
    mask_unit = (1 << n) - 1
    tokens: List[int] = []
    acc = 0
    nbits = 0
    for u in units:
        acc = (acc << n) | (int(u) & mask_unit)
        nbits += n
        while nbits >= 5:
            nbits -= 5
            tokens.append((acc >> nbits) & 0x1F)
    assert nbits == 0
    return tokens


# ---------------------------------------------------------------- composition

def compose(n_tokens: int) -> List[int]:
    """Binary decomposition of a token count into rung sizes, largest first.
    compose(13) -> [8, 4, 1]"""
    if n_tokens < 0:
        raise ValueError("negative count")
    return [r for r in sorted({1 << k for k in range(64)}, reverse=True)
            if r <= 64 and (n_tokens & r)] if n_tokens <= _MAX_COMPOSED else _compose_big(n_tokens)


_MAX_COMPOSED = 127  # counts above this repeat the int64 rung


def _compose_big(n_tokens: int) -> List[int]:
    """Counts beyond one pass of the ladder: repeat 5*int64 blocks, then
    decompose the remainder. 200 -> [64, 64, 64, 8]"""
    out = [64] * (n_tokens // 64)
    rem = n_tokens % 64
    out += [r for r in RUNGS if r & rem]
    return out


def pack_composed(tokens: List[int]) -> List[Tuple[int, List[int]]]:
    """Pack a stream of any length as exact rung segments, zero slack.
    Returns [(rung_width, units), ...] in canonical largest-first order."""
    segments: List[Tuple[int, List[int]]] = []
    idx = 0
    for n in _compose_big(len(tokens)) if len(tokens) > _MAX_COMPOSED else compose(len(tokens)):
        segments.append((n, pack_units(tokens[idx:idx + n], n)))
        idx += n
    return segments


def unpack_composed(segments: List[Tuple[int, List[int]]]) -> List[int]:
    """Inverse of pack_composed."""
    tokens: List[int] = []
    for n, units in segments:
        tokens.extend(unpack_units(units, n))
    return tokens


# ------------------------------------------------------- byte serialization

def composed_to_bytes(segments: List[Tuple[int, List[int]]]) -> Tuple[bytes, int]:
    """Serialize segments to a byte stream. The bitstream is exactly
    5*n_tokens bits; the only residue is the final byte-boundary fill,
    pad = (8 - (5n mod 8)) mod 8, identical to the reference packer.
    Bit-identical to pack_to_bytes(tokens) by construction (I3)."""
    acc = 0
    nbits = 0
    out = bytearray()
    for n, units in segments:
        for u in units:
            acc = (acc << n) | u
            nbits += n
            while nbits >= 8:
                nbits -= 8
                out.append((acc >> nbits) & 0xFF)
    pad = 0
    if nbits:
        pad = 8 - nbits
        out.append((acc << pad) & 0xFF)
    return bytes(out), pad


def bytes_to_composed(data: bytes, n_tokens: int) -> List[Tuple[int, List[int]]]:
    """Deserialize: the composition is derived from n_tokens alone
    (self-describing given the count; no stored pad needed)."""
    total_bits = n_tokens * 5
    if len(data) * 8 < total_bits:
        raise ValueError("byte stream shorter than 5*n_tokens bits")
    # stream tokens straight out of the bytes, then re-segment
    tokens: List[int] = []
    acc = 0
    nbits = 0
    it = iter(data)
    while len(tokens) < n_tokens:
        acc = (acc << 8) | next(it)
        nbits += 8
        while nbits >= 5 and len(tokens) < n_tokens:
            nbits -= 5
            tokens.append((acc >> nbits) & 0x1F)
    return pack_composed(tokens)
