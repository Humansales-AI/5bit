# 5bit — Interpreter: Tokens That DO

Companion to LEXICON2, PACKING, and OWNERSHIP. This document specifies the
execution layer: the six control verbs plus two effect verbs, structured
control flow with zero addresses, slots as the variable model, and
grant-gated effects. It is the organ that turns the fabric from a place where
programs *live* into a place where programs *run*.

Status legend follows LEXICON2. [VERIFIED] = confirmed by executing the
gauntlet (`test_griddb_interp.py`, 10/10) against the reference Python
implementation and a real AllocGrid. [DESIGN] = intended behavior.
[NOTE] = honest engineering caveat.

---

## 1. The verbs — complete SPECIAL3 table (slots 0..24)

All assigned SPECIAL3 slots. Slots 25-27 remain banked (3 available).
Verbs are encoded as `START×4 · cmd · END×4 [· NUM arg]`.

| Slot | Verb | Arg | Meaning |
|---|---|---|---|
| 0 | **AUTH** | user | declare record owner |
| 1 | **GRANT_R** | user | grant read access |
| 2 | **GRANT_W** | user | grant write access |
| 3 | **REVOKE** | user | revoke access |
| 4 | **ENCRYPT** | key | mark record encrypted |
| 5 | **LABEL** | pos,name | tag cell with metadata |
| 6 | **DEF** | slot | mark record as executable program |
| 7 | **CALL** | slot | run program at slot N |
| 8 | **RET** | — | early return |
| 9 | **IF** | — | three-way sign dispatch `( + )( 0 )( − )` |
| 10 | **LOOP** | — | repeat region until BREAK |
| 11 | **BREAK** | — | unwind past innermost LOOP |
| 12 | **STORE** | slot | write value — gated by GRANT_W |
| 13 | **READ** | slot | push slot's value |
| 14 | **EMIT** | — | pop and return value |
| 15 | **SYSCALL** | — | pop 4 args, syscall, push result |
| 16 | **AND** | — | bitwise AND two values |
| 17 | **OR** | — | bitwise OR two values |
| 18 | **XOR** | — | bitwise XOR two values |
| 19 | **SHL** | — | shift left |
| 20 | **SHR** | — | arithmetic shift right |
| 21 | **NOT** | — | bitwise NOT |
| 22 | **POPCNT** | — | population count (Hamming distance) |
| 23 | **MOVZX** | — | byte load from arena |
| 24 | **MOVB** | — | byte store to arena |

25 total assigned slots. 3 banked. [VERIFIED: XOR, AND, arithmetic]

---

## 2. Regions: structured control flow, zero addresses [VERIFIED]

Branch and loop bodies are `( … )` regions using the LPAREN/RPAREN tokens
already in the NUM lexicon (15/16). The interpreter never jumps to an
address — it **walks or skips balanced regions**:

```
IF ( +arm ) ( 0arm ) ( −arm )           LOOP ( body … BREAK … )
```

Skipping a false branch is balanced-paren counting — the same depth
discipline the §3 stack machine already lives by. No offsets, no GOTO, no
jump targets, no relocation: **structure is the address.** CALL crosses
records → slot addresses; branching stays inside a record → nesting.
[DESIGN rationale; VERIFIED behavior: both IF arms, LOOP+BREAK]

[NOTE] Skipping a large false region is a token-walk, not an O(1) hop.
Acceptable now; cacheable later (region-length memo) without format change.

### 2.1 Three-way IF: comparison for zero tokens [VERIFIED]

**Architect's ruling.** The zero/nonzero collapse was an inherited C
convention, not a lexicon property — and the lexicon already encodes sign as
first-class tokens (N-digits). The branch now sees what the number system
encodes: IF dispatches on sign(t) ∈ {+, 0, −} to up to three positional
regions. (FORTRAN's 1957 arithmetic IF, reborn in a fabric whose numerals
actually carry the sign.)

Consequence: **all six relations fall out of ONE MINUS + region placement —
zero new SPECIAL3 tokens spent** (P2 answered: budget = 0):

| Relation | Program shape |
|---|---|
| `a > b`  | `a b − IF ( X )` |
| `a == b` | `a b − IF ( ) ( X )` |
| `a < b`  | `a b − IF ( ) ( ) ( X )` |
| `a >= b` | `a b − IF ( X ) ( X )` |
| `a <= b` | `a b − IF ( ) ( X ) ( X )` |
| `a != b` | `a b − IF ( X ) ( ) ( X )` |

Multi-sign relations duplicate an arm; shared arms may `CALL` a common slot
instead (shared code = shared slot, the fabric way). The equality trap (P4)
dissolves: `==` is simply the 0-arm — no inversion dance.

[NOTE] IF greedily binds up to three consecutive `( … )` regions following
it; a bare region intended as plain code must not sit immediately after an
IF with fewer arms.

---

## 3. Values, arithmetic, output [VERIFIED]

- **Integers**: native lexicon encoding (digits + END finalizer) push onto
  the value stack. Negative digits work unchanged.
- **Arithmetic**: NUM operator tokens applied postfix — PLUS/MINUS/MUL/DIV
  pop two, push one. `(4+2)*10-5` is `4 2 + 10 * 5 -`.
- **EMIT**: the `=` token (14) pops and appends to program output — the
  lexicon's equals sign repurposed as "produce the answer."

---

## 4. Variables are slots [VERIFIED]

There is no local-variable machinery. Programs READ and STORE **grid
slots**. Consequences, all structural:

1. Program state is records — **durable, versioned, rewindable** by the same
   WAL that rewinds everything else. A program's variables have history.
2. State is **access-controlled by the same grants** as all other data.
3. The fabric is the register file: `sum(1..5)` ran with `i` at slot 100 and
   `acc` at slot 101, verified by reading the slots after execution.

[NOTE] Slots-as-variables means recursion needs per-frame slots (a frame
convention or slot-region allocation) — deliberately deferred; nested
non-recursive CALLs are verified.

---

## 5. Effects = grants: deny-by-default execution [VERIFIED]

STORE routes through `OwnershipEncoder.write_with_grant`. A program running
as holder H **physically cannot** append to a slot whose write grant H does
not hold:

```
holder 1 owns slot 300 (value 1000)
holder 2 runs: EMIT 777 · STORE 666 -> 300 · EMIT 999
   -> EMIT 777                        (got exactly this far)
   -> EncodeRefused, MID-PROGRAM      (execution halts)
   -> slot 300 still 1000             (grid untouched)
   -> 999 never emitted               (nothing after the refusal ran)
```

And the grant is the switch: after REVOKE + re-grant to holder 2, the
**byte-identical program** completes and slot 300 becomes 666. [VERIFIED]

This is the sentence no mainstream runtime can say: a program's side effects
are bounded by a capability table the runtime cannot be talked out of,
because the refusal is the ownership layer's, not the interpreter's.

---

## 6. Determinism & safety

- **Gas**: a step budget bounds execution; an infinite LOOP terminates in
  OutOfGas deterministically — same input, same budget, same halt point.
  [VERIFIED]
- **DEF enforcement**: records without a DEF header refuse to load as
  programs. Code and data share the grid; DEF is how they stay honest.
  [VERIFIED]
- **Programs are bytes**: pack → unpack → load → run produces identical
  output; programs are records and inherit LEXICON2's determinism (same
  program = same bytes, forever). [VERIFIED]
- **Traced**: every CALL/RET/EMIT/STORE/READ/branch decision appends to an
  execution trace — replayable, auditable, and (with WAL ticks) time-travel
  debuggable as a *storage feature*, not tooling. [DESIGN; trace VERIFIED]

---

## 7. Gauntlet summary [VERIFIED — 12/12]

| # | Property |
|---|---|
| I1 | Arithmetic program emits 55 for (4+2)*10−5 |
| I2 | Record without DEF refuses to load — data is not executable |
| I3 | IF takes then-arm on nonzero, else-arm on zero |
| I4 | LOOP+BREAK+slot-variables: sum(1..5)=15, state readable in grid after |
| I5 | CALL/RET nesting; args and returns ride the shared value stack |
| I6 | Mid-program STORE refusal: halt, grid untouched, trace shows the cut |
| I7 | Infinite LOOP → OutOfGas at the budget, deterministically |
| I8 | Programs survive pack→bytes→unpack→load→run |
| I9 | Early RET: nothing after it executes |
| I10 | The refused program completes after re-grant — ownership is the switch |
| I11 | **P1 acceptance**: CRM rule `value > 5000 → promote` on a real grid — 7000 promotes, 4000 stays, 5000 stays (strict) |
| I12 | All six relations (`> == < >= <= !=`) correct via one MINUS + region placement |

---

## 8. Open rulings (for the architect)

1. **Comparison operators — RULED (§2.1).** Three-way IF; six relations,
   zero tokens. SPECIAL3 slots 14..27 remain fully banked.
2. **Calling convention.** Shared value stack (Forth-style) is verified and
   minimal. A frame convention (arg slots per call) becomes necessary for
   recursion — see §4 NOTE.
3. **DEF's arg.** Currently DEF carries the slot id (self-describing header).
   Alternative: DEF carries an arity or capability list. One token of
   headroom either way.

---

## 9. Quick reference

```
Slots 0-5:  AUTH GRANT_R GRANT_W REVOKE ENCRYPT LABEL    (security)
Slots 6-13: DEF CALL RET IF LOOP BREAK STORE READ         (execution)
Slots 14-24: EMIT SYSCALL AND OR XOR SHL SHR NOT POPCNT MOVZX MOVB  (compute)
Slots 25-27: banked (3 available)
Host:   slots ≥9000 trap to doorman (NOW, HASH, EMIT_OUT, READ_IN, LOG)
Regions: ( … ) via LPAREN/RPAREN — walk or skip balanced; no addresses
Branch:  IF = three-way sign dispatch ( +arm )( 0arm )( −arm )
Safety:  DEF-only execution; gas budget; full trace
Law:     programs are records; records are bytes; bytes are deterministic
```
