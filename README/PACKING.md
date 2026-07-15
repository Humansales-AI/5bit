# 5bit — Width-Agnostic Packing & Transport Layer

Companion to LEXICON2. This document specifies the packing architecture: how a 5-bit
token stream maps onto integer transport words of selectable width, why the token
layer and the packing layer are strictly separated, and what this separation buys the
server path.

Status legend follows LEXICON2: [VERIFIED] = confirmed against the reference
implementation. [DESIGN] = intended behavior / convention. [NOTE] = honest
engineering caveat.

---

## 1. The core principle: two layers, one contract

**The token stream is pure.** [DESIGN]

A 5bit stream is a single flat sequence of 5-bit tokens. The parser (the §3 stack
machine of LEXICON2) consumes tokens one at a time. It has **no concept** of blocks,
lanes, words, or packing width. Nothing in the lexicon, the context stack, or the
record structure refers to how tokens are carried in memory or on the wire.

**Packing width is a transport parameter.** [DESIGN]

How the flat token sequence is serialized into machine integers is chosen at
pack/unpack time, per use case. The same logical stream may be packed into different
word widths for different destinations, and `unpack` always returns the identical
flat token sequence regardless of which width carried it.

```
        SEMANTIC LAYER                      TRANSPORT LAYER
  ┌─────────────────────────┐        ┌──────────────────────────┐
  │  tokens · contexts ·    │        │  pack(tokens, width) →   │
  │  stack machine · records│  ←──→  │  words; unpack(words) →  │
  │  (width-blind)          │        │  the same tokens          │
  └─────────────────────────┘        └──────────────────────────┘
```

Consequences:

1. **Conformance is defined at the token level.** Two bindings agree iff they produce
   the same token sequence; the packed bytes for a given width then follow
   mechanically from the packing rule. (LEXICON2 §6 / §11.) [VERIFIED for the
   byte-packed path: Python ≡ C, Python ≡ TS.]
2. **The parser never pays for transport.** No re-encoding, no translation step, no
   block bookkeeping on the hot path.
3. **The packer is swappable.** New transport widths can be added without touching
   the lexicon, the parser, or any stored data semantics.

---

## 2. The packing ladder

The primitive identity: **five units of intN carry exactly N tokens.** [DESIGN]

`5 × N bits = N × 5 bits` — the factor of five makes every rung of the integer
ladder an exact fit, with no misaligned width anywhere:

| Rung | Unit width | 5 units = bits | Tokens carried | Slack |
|---|---|---|---|---|
| 5 × int1  | 1 bit   | 5   | 1  | 0 |
| 5 × int2  | 2 bits  | 10  | 2  | 0 |
| 5 × int4  | 4 bits  | 20  | 4  | 0 |
| 5 × int8  | 8 bits  | 40  | 8  | 0 |
| 5 × int16 | 16 bits | 80  | 16 | 0 |
| 5 × int32 | 32 bits | 160 | 32 | 0 |
| 5 × int64 | 64 bits | 320 | 64 | 0 |

Every rung is exact — down to a single token (5 × int1) and up to the SIMD end
(5 × int64 = five u64 registers = 64 tokens). The int8 rung (40 bits = 5 bytes,
LCM(5,8)) is where the ladder meets byte-addressable memory, but alignment is a
property of that rung, not a privilege the others lack: each rung is a perfect
container at its own unit width. [DESIGN]

### 2.1 Dynamic composition — exact packing for any count

Because the rungs are powers of two, any token count decomposes in binary, so any
stream packs **exactly** as a sum of rungs: [DESIGN]

```
n = 13 tokens = 8 + 4 + 1
  → 5×int8 + 5×int4 + 5×int1 = 40 + 20 + 5 = 65 bits, zero slack

n = 21 tokens = 16 + 4 + 1
  → 5×int16 + 5×int4 + 5×int1 = 80 + 20 + 5 = 105 bits, zero slack
```

The packer selects rungs greedily from the binary representation of the count;
the unpacker reverses it. Padding is therefore not a property of the format at
all: the bitstream is always exactly `5·n` bits, composed of exact containers.
The LEXICON2 §6 tail rule survives only as the final byte-boundary residue —
at most 7 zero bits once per stream, where the bit-exact stream meets
byte-addressable storage — never as per-width or per-record slack.

[NOTE] Role of the small rungs: int1/int2/int4 are never whole-payload sizes —
no realistic stream is that small — they are the *remainder digits* of the
composition. Any stream whose token count is not a multiple of 8 terminates
through them (n=13 → [8,4,1]; n=1001 → 15×[64] + [32,8,1]). Removing them
reintroduces per-stream slack, defeating the scheme's purpose. They appear
constantly; they never appear alone.

[NOTE] Honest hardware caveat, for the reviewer who reads "int1" literally:
unit widths are exact *accounting* (framing, offsets, and wire layout are
computable to the bit), not machine types — the physical load path reads
byte-aligned words and applies shift/mask, as LEXICON2 already mandates. The
claim is zero *format* slack, not bit-addressable RAM.

### 2.2 Choosing a width

Width — single-rung or composed — is chosen by destination, not by the format:

- **Wire framing / small messages** — composed rungs for exact-fit frames with
  bit-precise, self-describing sizes.
- **Disk, WAL, mmap, bulk transfer** — int8-rung multiples, so every unit
  boundary is a byte address.
- **SIMD / GPU kernels** — 5×int64 blocks: 320 bits filling five u64 registers
  with zero slack, matching the load widths compute hardware already fetches.

The stream does not know or care which was used. [DESIGN]

### 2.3 Tail behavior

Unchanged from LEXICON2 §6 where byte packing is used directly: a stream whose
bit length is not a byte multiple zero-fills the final byte,
`pad = (8 − (5n mod 8)) mod 8`, recorded alongside the bytes. Under rung
composition this residue appears at most once, at the very end of the physical
stream. [VERIFIED for byte packing; DESIGN for rung composition]

[NOTE] In practice, well-formed constructions rarely sit at tiny counts: real
streams carry START/END wrappers, scale markers, and RECORD terminators
alongside digits, so wrapper tokens organically grow streams toward the larger
rungs.

### 2.4 The alignment law — where the 5 must come from

A container of **k units of intN is token-aligned iff 5 | k·N** (its bit length
k·N is a multiple of 5, so it holds only whole tokens). Because 5 is prime, the
factor of 5 must come from exactly one of two places — the unit count or the
unit width — giving two families of legal containers: [DESIGN]

**Family A — hardware widths (5 ∤ N):** int1, int2, int4, int8, int16, int32,
int64. The 5 must come from the count: k must be a multiple of 5. Five units is
the *minimum* legal block (10, 15, … units are also aligned); fewer than 5
units of these widths can never land on a token boundary. This is the family of
the §2 ladder, and why its rungs are stated as 5 × intN.

**Family B — token-native widths (5 | N):** int5, int10, int20, int40. Here a
*single* unit is already whole tokens (int5 = 1 token, int40 = 8 tokens), so
any k is aligned — the 5 lives inside the unit width itself.

The families meet at one block:

```
5 × int8  =  1 × int40  =  40 bits  =  8 tokens  =  5 bytes
```

— the same resonance point (LCM(5,8)) described from the hardware side and the
token side respectively.

Layer summary, stated precisely:

1. **Bitstream layer — bit-transparent.** The stream is exactly 5n bits;
   any slicing of those bits decodes to the same tokens. Misaligned
   segmentations (e.g. 1×int1 + 11×int4 for 45 bits) are not *undecodable* —
   a serial reader that concatenates all bits recovers the stream.
2. **Container layer — token-aligned (the ×5 law).** A segmentation that
   splits tokens across container boundaries produces containers that are
   individually meaningless: they cannot be independently decoded, addressed,
   or dispatched to a compute lane. Alignment (5 | k·N) is what makes a
   container a unit of work rather than a pen-stroke on a bitstream. The
   zero-copy, mmap-addressing, and SIMD properties of §4 exist only at this
   layer.
3. **Composition layer — canonical (determinism).** Among token-aligned
   compositions, exactly one is canonical: binary decomposition of the token
   count, largest rung first, Family A widths. It is unique, and it is
   derivable from the token count alone — the receiver reconstructs the
   entire composition from n_tokens with zero descriptor metadata. Any
   non-canonical composition would require transmitted framing data AND
   create multiple byte encodings of one input, violating LEXICON2
   invariant 3 (same input → same bytes). Per the §7 contract model, a
   correct encoder cannot emit a non-canonical composition; the default
   parser does not pay to re-check it.

[NOTE] Family B widths are logical: no shipping CPU loads int5 or int40
natively — they are shift-masked out of byte-aligned words like everything
else, which is why the canonical composition uses Family A. The law is stated
in full so that future substrates (custom lanes, FPGA) inherit it rather than
rediscover it.

---

## 3. What this design is NOT

Stated explicitly to prevent misreading:

- **Not stream quantization.** Streams are not padded, rounded, or structured into
  8-token blocks. There is no block header, no block boundary token, no alignment
  requirement on the logical stream. The ladder describes the *container*, never the
  *content*.
- **Not a parser concept.** The stack machine cannot observe packing width. A stream
  unpacked from int2-width transport and the same stream unpacked from 64-token GPU
  words are indistinguishable — token-identical by construction.
- **Not the §12 comparison-lane rule.** Fixed-width value lanes within an S-bucket
  (padding mantissas to equal length so XOR/Hamming compares aligned lanes) is a
  separate, downstream convention of the vector-fingerprint layer. It operates on
  values before encoding, not on the packed transport. The two mechanisms compose but
  are independent.

---

## 4. Why this is the server's performance thesis

Conventional database servers spend significant CPU translating between
representations: wire protocol → parsed structs → page format on disk, and the
reverse for every read. The width-agnostic packing layer deletes that pipeline.
[DESIGN, building on VERIFIED components]

**One representation, end to end.** The packed bytes received from a socket are the
same bytes appended to the WAL, the same bytes fsync'd, the same bytes mmap'd back,
and the same bytes handed to a comparison kernel. The data never changes costume at a
boundary:

```
socket → verify → append (WAL) → mmap → compute
              (identical bytes throughout)
```

Specific capabilities this unlocks:

1. **Zero-copy ingest.** A received frame is appended as-is. No decode/re-encode
   cycle between wire format and storage format, because they are the same format.
2. **Direct mmap addressing.** With byte-aligned units, records in a mapped file are
   addressable at integer byte offsets — point reads are pointer arithmetic plus a
   bounded shift/mask, never a reassembly across representations. This is the
   transport-level foundation under the "address is the key" property (LEXICON2 §7).
3. **Scatter-gather sends.** A response can be assembled as
   `writev(header, packed_region)` straight out of the mapped file — the kernel moves
   the data plane; the server never stages a translated copy.
4. **Deterministic replication.** Because bytes are identical across hosts for the
   same token stream (LEXICON2 invariant 3), replicas can verify by byte comparison,
   and the WAL-as-oplog ships the canonical representation itself.
5. **Compute without inflation.** 5-bit packed data is ~37.5% smaller than
   byte-per-character encodings *and* requires no decompression step before use: the
   XOR/popcount/SAD kernels of §12 operate on packed lanes directly (expanding to
   int8/int16/int32 lanes in VRAM on load, the Arrow pattern). Density where it
   counts — memory-bus traffic — with none of a codec's CPU tax.

[NOTE] The honest comparison: schemes like Cap'n Proto achieve "no serialization
step" between memory and wire. The 5bit packing layer extends the same property
through the storage engine and into the comparison kernels — wire, disk, and compute
share one representation. That end-to-end reach, not raw density, is the claim.

---

## 5. Binding requirements

A conformant binding's packing layer MUST: [DESIGN]

1. Expose packing width — single rung or rung composition — as an explicit
   parameter; never bake a width into encode logic.
2. Guarantee `unpack(pack(tokens, W)) == tokens` for every supported width `W`
   (round-trip identity per width).
3. Guarantee `unpack(pack(tokens, W1)) == unpack(pack(tokens, W2))` for all
   supported `W1, W2` (width equivalence — the transport leaves no residue).
4. Produce byte-identical output to the reference for the byte-packed path
   (LEXICON2 §11 conformance vectors) before any wider unit is trusted.
5. Use fixed-width unsigned integer types and mask every shift.

---

## 6. Quick reference

```
Layers:    tokens (semantic, width-blind)  |  packing (transport, width-parametric)
Identity:  5 × intN = N tokens, exact at every rung (incl. int1)
Compose:   n tokens → binary decomposition → sum of exact rungs, zero slack
Byte meet: int8 rung = 40 bits = 5 bytes (LCM(5,8))
SIMD:      5 × int64 = 320 bits = 64 tokens
Tail:      zero-fill final word; pad recorded; unchanged from LEXICON2 §6
Align:     k units of intN token-aligned  ⇔  5 | k·N   (5 is prime:
           the 5 comes from the count [5×intN, hardware widths] or
           the width [int5/int40, token-native]; families meet at
           5×int8 = 1×int40 = 40 bits)
Canon:     binary decomposition, largest rung first, Family A —
           unique, derivable from n_tokens, zero metadata
Law:       packing width is a transport parameter;
           the token stream is width-agnostic
```
