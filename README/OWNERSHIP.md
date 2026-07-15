# 5bit — Ownership Layer: Encode-Time Exclusive Write Grants

Companion to LEXICON2 and PACKING. This document specifies the ownership model:
slot-exclusive write grants enforced at **encode time**, shared snapshot reads
pinned to offsets, and the trusted-writer / opt-in-guard contract. It is the
first executable piece of the language layer: the checker you do not argue
with, because the violating stream is never produced.

Status legend follows LEXICON2. [VERIFIED] = confirmed by executing the test
gauntlet (`test_griddb_ownership.py`, 14/14) against the reference Python
implementation and a real AllocGrid. [DESIGN] = intended behavior / convention.
[NOTE] = honest engineering caveat.

---

## 1. The model: slot / offset split

Identity and version are different addresses, and ownership binds them
differently: [DESIGN]

| Concept | Address | Stability | Grant kind |
|---|---|---|---|
| **Identity** | slot (alloc-table entry) | stable forever | GRANT_W binds a slot |
| **Version**  | offset (physical position) | changes per append | GRANT_R pins an offset |

- **GRANT_W** — exclusive. At most one live write grant per slot. Holding it is
  the *right to append the next version* of that slot.
- **GRANT_R** — shared. Any number of readers may pin any offsets. Append-only
  storage (LEXICON2 invariant 5) makes a pinned offset immutable **by
  construction**: no future write can disturb it, because nothing is ever
  overwritten. Readers and the writer cannot conflict — they address different
  kinds of thing.
- **REVOKE** — ends a write grant. Only the current holder revokes its own
  grant.

This is the MVCC guarantee expressed structurally: snapshot readers at pinned
versions, one appender per identity, no lifetime annotations anywhere — the
storage layout is the lifetime system.

---

## 2. THE LAW: refusal happens at encode time

A conflicting GRANT_W is not detected by a checker after the fact. It
**cannot be encoded.** [VERIFIED]

```
enc.grant_w(slot=7, holder=1)   ->  15 tokens            (writer A holds slot 7)
enc.grant_w(slot=7, holder=2)   ->  EncodeRefused        (writer B: ZERO tokens)
```

Guarantees, all test-enforced: [VERIFIED]

1. **Zero-token refusal.** `EncodeRefused` is raised before any token exists.
   The violating stream is unrepresentable on the trusted path — there is
   nothing to write, replicate, or parse.
2. **Atomic check+emit.** The ledger check and token emission are one critical
   section; there is no window where two racing grants both pass. Twelve
   threads racing one slot: exactly one wins, eleven refuse.
3. **Refusal is side-effect-free.** A refused call leaves the event log and
   grant table bit-identical to before.

This is the LEXICON2 §7 contract model applied to ownership: the contract
lives in the writer; a correct encoder cannot emit the malformed state; the
default parser does not pay to re-check it.

[NOTE] Relation to Rust: the borrow checker rejects programs that *were
written*; this encoder makes the conflicting program *unwritable*. The
exclusivity rule enforced is the same one-writer-XOR-many-readers aliasing
discipline, but positionally (slot occupancy) rather than by proof over
names and lifetimes. There is nothing to annotate and nothing to argue with.

---

## 3. Wire format — no new tokens, no parser changes

Grant records are ordinary records built from existing LEXICON2 primitives
(the SPECIAL3 commands already in the lexicon and `encode_command`): [VERIFIED]

```
GRANT_W  slot S to holder H:    encode_command('GRANT_W', H) · NUM(S) · RECORD
GRANT_R  slot S offset O by H:  encode_command('GRANT_R', H) · NUM(S) · NUM(O) · RECORD
REVOKE   slot S by holder H:    encode_command('REVOKE',  H) · NUM(S) · RECORD
```

The existing Parser yields, per record:
`{'type':'command','cmd':...}` · `ParsedNumber(slot)` [· `ParsedNumber(offset)`]
— grant records survive `pack -> bytes -> unpack -> parse` losslessly.
[VERIFIED]

[NOTE] The SPECIAL3 tokens remain *representations*, exactly as LEXICON2 §10
states: enforcement is performed by the OwnershipEncoder (encode-time) and the
opt-in validator (parse-time), not by the tokens alone. This layer is the
slot-exclusivity counterpart to RLSEngine's per-record owner/grantee checks;
the two compose and do not replace each other.

---

## 4. The ledger: deterministic, replayable, append-only

The grant table is derivable purely by replaying grant records in stream
order — no hidden state: [VERIFIED]

```
GrantTable.replay(event_log)  ==  live table     (bit-exact reproduction)
```

Consequences: [DESIGN]

- **Durability for free.** Grant records are ordinary records; persist them
  through the existing WAL and the ownership state survives crash recovery by
  the same replay that recovers everything else.
- **Audit-complete.** The full history of who held what, when, is the stream
  itself.
- **Cross-process.** In-process races are lock-serialized [VERIFIED,
  12 threads]. Across processes, the ledger append is the CAS target: route
  grant-record appends through the existing verified `write_if`
  (expected_offset/expected_bitlen) and multi-writer races resolve with the
  machinery LEXICON2 §9 already proved (12 processes, zero lost updates).
  [DESIGN — integration point; not re-benchmarked here]

---

## 5. Guarded writes

`write_with_grant(grid, slot, holder, tokens)` appends a new version of the
slot iff the holder owns the live write grant: [VERIFIED]

```
holder appends v1  -> offset 12
stranger append    -> EncodeRefused (WRITE refused: held by 1, not 2)
holder appends v2  -> offset 16
reader pinned @12  -> untouched, immutable by construction (append-only)
```

The slot/offset split in action: the writer moved the slot's current version;
the reader's pinned universe did not move.

---

## 6. Untrusted streams: the opt-in guard

The trusted path never re-checks (the encoder cannot produce violations). For
untrusted or hand-assembled streams, `validate_ownership(tokens)` replays the
stream's grant events and raises `MalformedOwnership` on: [VERIFIED]

- a second live GRANT_W on a held slot (the hand-built attack the encoder
  refused to build — caught at parse time instead),
- REVOKE by a non-holder or on an unheld slot,
- structurally malformed grant records (missing holder/slot/offset).

Same double-choice as LEXICON2 §7's label guard: the hot path is check-free by
contract; ingestion of untrusted bytes switches the guard on.

---

## 7. Conformance obligations for bindings

A conformant ownership implementation MUST: [DESIGN]

1. Refuse conflicting grants **before** producing any token (zero-token
   refusal, side-effect-free).
2. Make check+emit atomic under its concurrency model (no TOCTOU).
3. Emit grant records byte-identical to the reference wire format (§3) —
   verified through the existing pack conformance vectors.
4. Reproduce `GrantTable.replay` semantics exactly: same event stream, same
   final table, same refusals at the same events.
5. Keep GRANT_R unconditional (shared pins never block) and REVOKE
   holder-only.

---

## 8. Test gauntlet summary [VERIFIED — 14/14]

| # | Property |
|---|---|
| T1 | Two GRANT_Ws race one slot; second refused at encode time |
| T2 | Refusal produces zero tokens, zero events, ledger unchanged |
| T3 | Stranger revoke refused; holder revoke frees; re-grant succeeds |
| T4 | 12-thread race, one slot: exactly 1 winner, 11 refused |
| T5 | Multiple GRANT_R pins coexist with a live GRANT_W |
| T6 | Guarded append: holder writes v1,v2; stranger refused; pinned offset untouched |
| T7 | Replay determinism: event log reproduces live table exactly |
| T8 | Grant records survive pack→bytes→unpack→parse |
| T9 | Hand-assembled double GRANT_W caught by opt-in validator |
| T10 | Malformed grant record (missing slot) caught by validator |

---

## 9. Quick reference

```
Identity:  slot (stable)      Version: offset (per append)
GRANT_W:   binds slot, exclusive — the right to append the next version
GRANT_R:   pins offset, shared — immutable by construction (append-only)
REVOKE:    holder-only
THE LAW:   conflicting GRANT_W raises EncodeRefused with ZERO tokens produced
Trust:     encoder = trusted path (check-free parse);
           validate_ownership = opt-in guard for untrusted bytes
Ledger:    grant records are ordinary records; state = replay(stream);
           cross-process = write_if CAS on the ledger append
```
