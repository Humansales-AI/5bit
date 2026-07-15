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

## 1. The verbs — SPECIAL3 budget (slots 6..13, 18..19)

The execution layer uses SPECIAL3 slots 6-13 and 18-19. Slots 14-17
and 20-27 remain banked (12 slots available). Verbs are encoded in the
exact `encode_command` shape (START×4 · cmd · END×4 [· NUM arg]) —
they pack, unpack, and parse with **zero new machinery** and survive
bytes round-trip. [VERIFIED]

| Slot | Verb | Arg | Meaning |
|---|---|---|---|
| 6 | **DEF** | slot | "this record is a program." CALL refuses non-DEF records. |
| 7 | **CALL** | slot | run the program at slot N; return here. |
| 8 | **RET** | — | early return. End of record = implicit RET. |
| 9 | **IF** | — | pop t; **three-way dispatch on sign(t)**: `( +arm )( 0arm )( −arm )`. |
| 10 | **LOOP** | — | repeat region until BREAK. |
| 11 | **BREAK** | — | unwind to just past the innermost LOOP region. |
| 12 | **STORE** | slot | pop value, write to slot — **gated by GRANT_W**. |
| 13 | **READ** | slot | push the slot's current value. |
| 18 | **LOADX** | — | pop index; push `slots[index]`. Array access over slot ranges. |
| 19 | **STOREX** | — | pop index, pop value; `slots[index] = value`. Array write. |

LOADX/STOREX enable programs to treat contiguous slot ranges as arrays —
the primitive behind the compiler's byte emission (STORE bytes to
`OUT_BASE + OP`) and the VLIW scheduler's dependency analysis.

### 1.1 Compiler opcode convention (NUM context, tokens 15-24)

When the 5bit compiler (`compiler.5b`) runs on this interpreter, it
maps NUM-context token values to x86-64 instruction sequences. These
are **application-level conventions**, not interpreter verbs — the
compiler's dispatch chain tests token values via three-way IF:

| Token | x86-64 emitted | Use |
|---|---|---|
| 15 | `pop rbx; pop rax; xchg rdi; pop rax; xchg rsi; pop rax; xchg rdx; syscall; push rax` | SYSCALL — kernel interface |
| 16 | `pop rbx; pop rax; and rax, rbx; push rax` | AND |
| 17 | `pop rbx; pop rax; or rax, rbx; push rax` | OR |
| 18 | `pop rbx; pop rax; xor rax, rbx; push rax` | XOR |
| 19 | `pop rcx; pop rax; shl rax, cl; push rax` | SHL |
| 20 | `pop rcx; pop rax; sar rax, cl; push rax` | SHR |
| 21 | `pop rax; not rax; push rax` | NOT |
| 22 | `pop rax; popcnt rax, rax; push rax` | POPCNT (Hamming distance) |
| 23 | `pop rax; movzx rax, byte [rax]; push rax` | MOVZX (byte load) |
| 24 | `pop rbx; pop rax; mov [rbx], al; push rax` | MOVB (byte store) |

These 10 opcodes + the existing digit/arithmetic/EMIT handlers give
the compiler 25 total emission targets — enough to emit the full scalar
x86-64 instruction set including syscall. [VERIFIED: XOR, AND tested]

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
Verbs:    DEF=6 CALL=7 RET=8 IF=9 LOOP=10 BREAK=11 STORE=12 READ=13
          LOADX=18 STOREX=19  (slots 14-17, 20-27 still banked)
Regions:  ( … ) via LPAREN/RPAREN — walk or skip balanced; no addresses
Branch:   IF = three-way sign dispatch ( +arm )( 0arm )( −arm );
          all six relations = one MINUS + arm placement
Values:   integers push; PLUS/MINUS/MUL/DIV postfix; '=' emits
Memory:   variables ARE slots (durable, versioned, grant-controlled)
Array:    LOADX/STOREX for indexed slot access (compiler back-end)
Compiler: tokens 15-24 map to x86-64 (SYSCALL, AND/OR/XOR/SHL/SHR/NOT/POPCNT/MOVZX/MOVB)
          25 total emission targets; scalar ISA complete
Host:     slots ≥9000 trap to doorman (NOW, HASH, EMIT_OUT, READ_IN, LOG)
Safety:   DEF-only execution; gas budget; full trace
Law:      programs are records; records are bytes; bytes are deterministic
```
