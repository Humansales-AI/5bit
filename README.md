# 5bit

```
   █████████    ██████    ██    ████████
  ██       ██  ██    ██  ██      ██
  ██       ██  ██    ██  ██      ██
  ███████████  ████████  ██      ██
  ██       ██  ██    ██  ██      ██
  ██       ██  ██    ██  ██      ██
      5         B        I      T

  VM · DB · Language · Runtime · Compiler
    32 tokens. 5 bits. One substrate.
```

---

## What is 5bit?

5bit is a **self-hosting computational substrate**. It is a database, a virtual machine, a programming language, a compiler, and a protocol engine — all built from a single 32-token, 5-bit alphabet. Every layer speaks the same tongue. There are no seams.

| Layer | What it is |
|---|---|
| **Lexicon** | 32 five-bit tokens × 5 contexts (NUM, WORD, SPECIAL, SPECIAL2, SPECIAL3). 28 mappable codes + 4 controls. Deterministic encoding — same input produces same bytes, forever. |
| **Packing** | Width-agnostic transport. 5 × intN carries exactly N tokens at every rung. Wire format = storage format = compute format. |
| **5basm** | The notation layer. Text → tokens → packed bytes. Depth-checked, grant-refusing. Round-trips losslessly. |
| **Ownership** | Encode-time exclusive write grants. Conflicting GRANT_W is refused before producing tokens. The violating stream is unrepresentable — wrong programs cannot be written. |
| **Interpreter** | 8-verb stack machine (DEF, CALL, RET, three-way IF, LOOP, BREAK, STORE, READ). Programs are records at slots. Code and data share the grid. Three-way IF dispatches on sign(t) to three positional regions — all six comparisons from one subtraction + region placement. |
| **Host (Doorman)** | Slots ≥9000 are host capabilities. CALL to a reserved slot is trapped, grant-checked, and dispatched as a physical effect (clock, hash, socket I/O). The grant table is the ACL for reality. |
| **Self-Hosting** | The 5bit compiler is written in 5bit. C runs it once. The emitted native compiler then compiles 5bit — including itself. The ladder is kicked. |
| **Bootstrap** | C interpreter (10/10 test parity) runs on bare metal. No Python in the critical path. C is a historical artifact — the ladder that was climbed once. |
| **Protocols** | HTTP, WebRTC signaling, SMTP, raw TCP — all unified under one boundary. C handles sockets (the irreducible shell). 5bit handles routing, auth, and handlers natively. |
| **Native Comparator** | Two records on the grid are two rows of a matrix. Column-wise comparison reads divergence directly from aligned lanes. The grid structure provides the comparison geometry — no additional instructions needed. |

5bit is not a database with a query language. It is a **single substrate** where the token format IS the storage format IS the wire format IS the compute format. The bloat was never inside the layers — it was between them. 5bit deleted the seams.

---

## Project Status

```
✅ Atomicity        — Multi-write transactions via WAL + RECORD
✅ Consistency      — Application-enforced (schema-free by design)
✅ Isolation        — flock per-write + write_if CAS cross-process
✅ Durability       — WAL + fsync + SHA-256 chain, crash recovery verified
✅ Point reads      — O(1) at absolute bit offsets (AllocGrid)
✅ Indexes          — Hash (O(1) equality) + B-tree (O(log n) range)
✅ Replication      — Master/Replica over HTTP, WAL as oplog
✅ Transactions     — Begin/Commit/Rollback, WAL-backed, lock-spanned
✅ Change streams   — SSE + long-poll from WAL tail
✅ REST API         — Deterministic routes, content-addressed ETags
✅ Auth             — PBKDF2 + JWT sessions + OAuth (Google/GitHub)
✅ Storage          — Content-addressed file store, SHA-256 dedup
✅ Realtime         — WebSocket + presence + broadcast channels
✅ npm client       — fivebit-client@0.2.2
✅ Conformance      — 53/53 cross-language + canonical compaction
✅ Compaction       — Reclaim tombstone space, crash-atomic
✅ Page cache       — LRU read cache, 68× hit speedup (wired into AllocGrid)
✅ Multi-mode auth  — Zero-knowledge (server blind) or Managed (server can read)
✅ Interpreter      — 8-verb stack machine (DEF, CALL, RET, IF, LOOP, BREAK, STORE, READ)
✅ Assembler        — 5basm notation layer, depth-checked, grant-refusing
✅ Ownership        — Encode-time exclusive write grants, 14/14 test gauntlet
✅ Rung packing     — Width-agnostic transport (5×intN ladder), C + Python
```

---

## The 32‑Token Lexicon — Five Contexts

Four contexts, same 32 binary codes. 28 mappable slots each (00000–11011). Four controls (11100–11111) retain meaning across all contexts. SPECIAL3 maps the same 28 slots to control commands instead of characters — token-level permissions in the fabric itself.

| Binary  | NUM | WORD | SPECIAL | SPECIAL2 | SPECIAL3 |
|:-------:|:---:|:----:|:-------:|:--------:|:--------:|
| `00000` | `0` | `A`  | `a`     | `!`      | `AUTH`   |
| `00001` | `1` | `B`  | `b`     | `"`      | `GRANT_R`|
| `00010` | `2` | `C`  | `c`     | `#`      | `GRANT_W`|
| `00011` | `3` | `D`  | `d`     | `$`      | `REVOKE` |
| `00100` | `4` | `E`  | `e`     | `%`      | `ENCRYPT`|
| `00101` | `5` | `F`  | `f`     | `&`      | `LABEL`  |
| `00110` | `6` | `G`  | `g`     | `'`      | `DEF`    |
| `00111` | `7` | `H`  | `h`     | `(`      | `CALL`   |
| `01000` | `8` | `I`  | `i`     | `)`      | `RET`    |
| `01001` | `9` | `J`  | `j`     | `*`      | `IF`     |
| `01010` | `+` | `K`  | `k`     | `+`      | `LOOP`   |
| `01011` | `-` | `L`  | `l`     | `,`      | `BREAK`  |
| `01100` | `*` | `M`  | `m`     | `/`      | `STORE`  |
| `01101` | `/` | `N`  | `n`     | `:`      | `READ`   |
| `01110` | `=` | `O`  | `o`     | `;`      | `—`      |
| `01111` | `(` | `P`  | `p`     | `<`      | `—`      |
| `10000` | `)` | `Q`  | `q`     | `=`      | `—`      |
| `10001` | `-1`| `R`  | `r`     | `>`      | `—`      |
| `10010` | `-2`| `S`  | `s`     | `?`      | `—`      |
| `10011` | `-3`| `T`  | `t`     | `[`      | `—`      |
| `10100` | `-4`| `U`  | `u`     | `\`      | `—`      |
| `10101` | `-5`| `V`  | `v`     | `]`      | `—`      |
| `10110` | `-6`| `W`  | `w`     | `^`      | `—`      |
| `10111` | `-7`| `X`  | `x`     | `_`      | `—`      |
| `11000` | `-8`| `Y`  | `y`     | `` ` ``  | `—`      |
| `11001` | `-9`| `Z`  | `z`     | `{`      | `—`      |
| `11010` | `^` | `␣`  | `@`     | `\|`     | `—`      |
| `11011` | `S` | `.`  | `-`     | `}`      | `—`      |
| `11100` | **RECORD** | **RECORD** | **RECORD** | **RECORD** | **RECORD** |
| `11101` | **CHECKSUM** | **CHECKSUM** | **CHECKSUM** | **CHECKSUM** | **CHECKSUM** |
| `11110` | **END** | **END** | **END** | **END** | **END** |
| `11111` | **START** | **START** | **START** | **START** | **START** |

---

## Context Switching — How It Works

The parser starts in **NUM** state. Four control tokens navigate a 4-level stack:

| Token | Current state | Action |
|:-----:|:--------------|:-------|
| `START` (11111) | NUM | Enter WORD |
| `START` (11111) | WORD | Enter SPECIAL |
| `START` (11111) | SPECIAL | Enter SPECIAL2 |
| `START` (11111) | SPECIAL2 | Enter SPECIAL3 (control commands) |
| `END` (11110) | SPECIAL3 | Pop to SPECIAL2 |
| `END` (11110) | SPECIAL2 | Pop to SPECIAL |
| `END` (11110) | SPECIAL | Pop to WORD |
| `END` (11110) | WORD | Pop to NUM (finalize) |
| `RECORD` (11100) | Any | Finalize, emit record boundary, pop to NUM |
| `CHECKSUM` (11101) | Any | Emit integrity marker |

**Encoding examples:**

```
"HI"    → START  H  I  END                                     (2 letters, WORD)
"hi"    → START  START  h  i  END  END                          (2 lowercase, SPECIAL)
"Hi"    → START  H  START  i  END  END                          (mixed case)
"a!b"   → START  START  a  START  !  END  b  END  END           (SPECIAL2 punctuation)
"a@b"   → START  START  a  @  b  END  END                       (@ stays in SPECIAL)
"a.b"   → START  a  .  b  END                                   (. in WORD, no switch)
"AUTH 42" → START×4  CMD_AUTH  D4 D2 END  END×5                (SPECIAL3 command)
```

Digits (0-9) encode by temporarily popping to NUM: `END END D3 START` — pop SPECIAL→WORD→NUM, emit digit, re-enter WORD. A 64-char hex hash with digits and letters costs ~70-170 tokens depending on digit density.

The **RECORD** token (11100) terminates logical tuples. Everything between two RECORD tokens is one record. This is the boundary for geometric queries — Manhattan distance compares record vectors.

**SPECIAL3 Commands** map 28 slots to control primitives. Slots 0-5 are the security layer; slots 6-13 are the execution layer (verbs); 14 slots remain reserved.

**Security commands (slots 0-5):**

```
AUTH(user_id)     — declare record owner at token level
GRANT_R(user_id)  — grant read access to user
GRANT_W(user_id)  — grant write access to user
REVOKE(user_id)   — revoke access from user
ENCRYPT(key_id)   — mark record as encrypted with key
LABEL(pos, name)  — tag cell positions with metadata
```

**Execution verbs (slots 6-13) — the interpreter's instruction set:**

```
DEF(slot)   — "this record is a program"; CALL refuses non-DEF records
CALL(slot)  — run program at slot N; shared value stack carries args/returns
RET         — early return from function (end of record = implicit RET)
IF          — pop test value; nonzero → execute then-region, else skip to else-region
LOOP        — repeat a region until BREAK is hit
BREAK       — unwind to just past the innermost enclosing LOOP
STORE(slot) — pop value, append it as the new version of slot N (gated by GRANT_W)
READ(slot)  — push the current value of slot N onto the stack
```

These eight verbs are the entire execution model. Programs are records at slots. Code and data share the grid — DEF is the bit that tells the interpreter "this slot is executable." Control flow uses LPAREN/RPAREN regions (tokens already in the NUM lexicon) with balanced-paren counting — no offsets, no GOTO, no jump targets. Structure IS the address.

STORE is gated by ownership: a program running as holder H cannot append to a slot whose write grant H does not hold — `EncodeRefused` fires mid-program, execution halts, the grid is untouched. Deny-by-default execution. Effects are grants.

14 reserved slots remain for future commands (spawn, yield, effects, etc.).

```
LABEL 0 "user_id"    START×4 D5 D0 END×4  START u s e r _ i d END END
LABEL 1 "age"        START×4 D5 D1 END×4  START a g e END END
LABEL 2 "email"      START×4 D5 D2 END×4  START e m a i l END END
LABEL 3 "balance"    START×4 D5 D3 END×4  START b a l a n c e END END
LABEL 4 "name"       START×4 D5 D4 END×4  START n a m e END END
LABEL 5 "created"    START×4 D5 D5 END×4  START c r e a t e d END END

DATA:  D1 END  D2 D5 END  a l i c e @ d e m o . c o m END END  D5 D0 D0 D0 END  A l i c e END END  D8 END  RECORD
       uid=1   age=25       email="alice@demo.com"                  balance=5000          name="Alice"      created=8
```

B-tree reads labels → finds "age" at position 1 → indexes every record's position-1 value. No external schema config.

---

## Label-Driven Architecture

Labels replace schemas. The data describes itself.

**Without labels** (spec-driven):
```python
# Server needs external config
spec = {'fields': ['age', 'balance', 'name']}
# B-tree indexes position 0 because spec says "age is at 0"
```

**With labels** (data-driven):
```python
# Labels baked into the token stream
Encoder.encode_label(0, "age")       # cell 0 tagged "age"
Encoder.encode_label(1, "balance")   # cell 1 tagged "balance"
Encoder.encode_label(2, "name")      # cell 2 tagged "name"

# B-tree auto-discovers: position 0 = age, position 1 = balance
# Query ?filter=age:gt:21 — found via label lookup, no config
```

The server scans for CMD_LABEL tokens on startup, builds a field_name → position map, and auto-creates B-tree indexes. Labels travel with the data. Drop a labeled grid file on any server and it knows the schema instantly.

**Labels solve the HashIndex fragmentation bug.** The HashIndex stores multiple key-value pairs chained in one record — digits in keys fragment the chain, making it impossible to separate a key's own digits from the record_id that follows:

```
Old (chain, broken):   WORD("email42") NUM(7) RECORD → NUM fragments break the chain
Label (separate, fixed):  LABEL 0 "email42"           → stored as CMD_LABEL token
                           NUM(7) RECORD              → value at position 0
```

The label is the key. The value record sits at the labeled position. No chain. No ambiguity. Same per-field record pattern that fixed webhooks, DLQ, and storage owner fields.

**Labels preserve original characters losslessly.** The label join rule reconstructs from the original tokens, not parsed integer values:

```
"user 42" — space lives as WORD(' ') in the stream:
  Tokens:   START START u s e r END T_POW END D4 D2 END
  Parsed:   WORD('user')  WORD(' ')  NUM(4)  NUM(2)
  Label join at pos 0:  "user" + " " + "4" + "2" = "user 42" ✓

"id007" — leading zeros are NUM(0) tokens with value 0:
  Tokens:   START START i d END END D0 D0 D7 END
  Parsed:   WORD('id')  NUM(0)  NUM(0)  NUM(7)
  Label join at pos 0:  "id" + "0" + "0" + "7" = "id007" ✓
```

`str(NUM(0).value) = "0"` — the original digit text, not the integer 7. The token stores the digit, the parser stores the digit, the join concatenates the digits. Lossless. Labels solve field boundaries AND character preservation in the same mechanism.

**Labels-first vs data-first.** Both work. Labels-first is cleaner — the reader knows what it's reading as it reads:

```
LABEL 0 "age"       ← header: position 0 = age
LABEL 1 "balance"   ← header: position 1 = balance
LABEL 2 "name"      ← header: position 2 = name
══════════════      ← end of header, data starts
D2 D5 END           ← position 0: age=25
D5 D0 D0 D0 END     ← position 1: balance=5000
START A l i c e END END  ← position 2: name="Alice"
RECORD
```

Labels and data are interleaved — each label is immediately followed by its value. Self-contained per record. Labels as separate grid records is more space-efficient — store the schema once, every data record references positions from the label registry.

**Why NUM separates adjacent word fields.** `reassemble()` walks parsed tokens looking for consecutive WORDs. When it hits a NUM, it emits the accumulated word, keeps the NUM, and starts fresh:

```
Parsed:  WORD('ACME')  NUM(0)  WORD('CORP')
                                ↑
                  NUM stops the merge here.
                  "ACME" emitted. NUM kept. "CORP" starts new word.
```

Without the NUM, consecutive WORDs merge into `"ACMECORP"`. With it, they stay `"ACME"` and `"CORP"`. The value `0` is irrelevant — any NUM works. It just needs to be a `ParsedNumber`, not a `ParsedWord`.

**Label-aware Reassembly.** The reader uses labels to decide which positions to join:

```python
# Without labels:  WORD("user") NUM(4) NUM(2) → "user4" "2" (fragmented)
# With labels:     LABEL 0 "username"  WORD("user") NUM(4) NUM(2)
#                                        → position 0 = join → "user42"
```

`AllocGrid.reconstructByLabels(parsed)` walks the parsed tokens, finds all LABEL commands, builds a `position → name` map, then joins every token at labeled positions into a single string. Tokens at unlabeled positions are skipped. The result is `{'age': '25', 'balance': '5000', 'name': 'Alice'}`.

---

## The Interpreter — Tokens That DO Instead of MEAN

Everything before this layer is nouns (records, grants, values). The interpreter adds verbs — eight SPECIAL3 commands that turn the grid from a database into a runtime.

**The model**: a stack machine where parsed numbers push onto a data stack, arithmetic operators (tokens 10-13, already in the lexicon) work in postfix notation, and `=` (token 14) pops and emits to the output stream. Variables are slots — the grid itself is the register file.

```
Program:  DEF 42  ( D5 END  D3 END  +  = )  RECORD
          └── header ─┘ └── body: 5 3 + = ──┘

Execution:  D5 END → push 5
            D3 END → push 3
            +      → pop 3, pop 5, push 8
            =      → pop 8, emit to output

Output: [8]
```

**Control flow** uses the existing LPAREN/RPAREN tokens (15/16) as block delimiters. IF pops a test value and walks either the then-region or else-region by balanced-paren counting. LOOP repeats a region until BREAK unwinds to just past the enclosing LOOP's boundary. No offsets, no jump targets — structure IS the address.

```
IF  ( D3 END = )  ( D4 END = )     →  if true emit 3, else emit 4
LOOP ( ... BREAK )                  →  repeat until BREAK
CALL 99                             →  jump to program at slot 99
RET                                 →  early return (end of record = implicit RET)
```

**Why BREAK earns its slot.** Without it, every early exit from a loop becomes flag gymnastics: set found=1, let the iteration limp to its end, have LOOP's test check the flag. BREAK says it in one word: unwind the depth to just past the enclosing LOOP's region. Same balanced-skip machinery as a false IF branch — zero new mechanism.

**Safety**: a step budget (gas, default 100,000) bounds runaway loops deterministically. `OutOfGas` halts execution.

**Loading**: `Machine.load(slot, tokens)` verifies the DEF header. A record without DEF cannot be CALL'd — data is not executable. This is the single bit that keeps code and data honest while sharing the same grid.

```python
from griddb_interp import Machine, program, num, verb, region, CMD_DEF, CMD_CALL

m = Machine(grid=grid, holder=1)
m.load(42, program(42, num(100), verb('STORE', 5)))
m.run(42)  # stores 100 at slot 5 (if holder 1 owns GRANT_W on slot 5)
```

---

## 5basm — The Notation Layer

The assembler closes the gap between fingers and bits. It is not a traditional assembler (there is no semantic gap to bridge — the lexicon IS the language, the grid IS the layout, slots ARE the symbols). It is a literate notation whose backend is the existing Encoder, with two properties traditional assemblers never had:

1. **Depth is checked, not counted.** Humans cannot hand-count START/END nesting. The assembler tracks depth per token and refuses to assemble unbalanced or over-deep streams — with line numbers.
2. **It refuses like the encoder refuses.** Grant statements route through `OwnershipEncoder`: a program containing two live GRANT_Ws on one slot cannot be assembled. The invalid binary never exists.

```
; comments start with semicolon
auth 1
grant_w 12 holder 1
label 0 "score"
int -987
label 1 "tag"
word "HELLO WORLD"
record
```

Statements: `int <n>`, `word "<text>"`, `label <pos> "<name>"`, `grant_w <slot> holder <h>`, `grant_r <slot> holder <h> offset <o>`, `revoke <slot> holder <h>`, `auth <owner>`, `record`, `checksum`, `raw <TOK>...`

Disassembly round-trips: `disassemble(assemble(src))` produces identical tokens and bytes. The notation leaves no residue.

```bash
python3 griddb_asm.py program.5ba   # assemble → program.5b (packed bytes)
```

---

## Ownership Layer — Encode-Time Exclusive Write Grants

The borrow-checker replacement. Conflicting GRANT_W is not rejected by a checker after the fact — it **cannot be encoded**.

```
enc.grant_w(slot=7, holder=1)   →  15 tokens            (writer A holds slot 7)
enc.grant_w(slot=7, holder=2)   →  EncodeRefused         (writer B: ZERO tokens)
```

**The model**: slot = logical identity (stable forever), offset = physical version (changes per append). GRANT_W binds a slot exclusively — at most one live write grant. GRANT_R pins an offset — shared, any number of snapshot readers. REVOKE ends a grant — only the current holder can revoke its own.

**Guarantees, all test-enforced (14/14):**

1. **Zero-token refusal.** `EncodeRefused` raises before any token exists. The violating stream is unrepresentable on the trusted path.
2. **Atomic check+emit.** Twelve threads racing one slot: exactly one wins, eleven refuse. No TOCTOU window.
3. **Refusal is side-effect-free.** A refused call leaves the event log and grant table bit-identical to before.
4. **Deterministic replay.** `GrantTable.replay(event_log)` reproduces the live table exactly — the event stream IS the state.
5. **Cross-process.** Grant records are ordinary records; persist through the existing WAL; multi-writer races resolve via the existing `write_if` CAS.

**Guarded writes**: `write_with_grant(grid, slot, holder, tokens)` appends a new version iff the holder owns the live write grant. The grant check gates the append; readers pinned to old offsets are untouched (append-only). A stranger's append raises `EncodeRefused` mid-program.

**Trust model**: the encoder is the trusted path (check-free parse); `validate_ownership(tokens)` is the opt-in guard for untrusted bytes — replays grant events, raises `MalformedOwnership` on hand-assembled double GRANT_W, stranger revoke, or malformed records.

```python
from griddb_ownership import OwnershipEncoder, validate_ownership

enc = OwnershipEncoder()
enc.grant_w(slot=5, holder=1)
enc.write_with_grant(grid, 5, 1, tokens)   # ✓ holder appends
enc.write_with_grant(grid, 5, 2, tokens)   # ✗ EncodeRefused

# Untrusted stream guard
validate_ownership(untrusted_tokens)        # raises on violations
```

---

## Rung Packing — Width-Agnostic Transport

The packing layer separates the semantic token stream from how it's carried in memory or on the wire. The primitive identity: **five units of intN carry exactly N tokens.**

| Rung | Unit width | 5 units = bits | Tokens carried | Slack |
|---|---:|---:|---:|---:|
| 5 × int1  | 1 bit   | 5   | 1  | 0 |
| 5 × int2  | 2 bits  | 10  | 2  | 0 |
| 5 × int4  | 4 bits  | 20  | 4  | 0 |
| 5 × int8  | 8 bits  | 40  | 8  | 0 |
| 5 × int16 | 16 bits | 80  | 16 | 0 |
| 5 × int32 | 32 bits | 160 | 32 | 0 |
| 5 × int64 | 64 bits | 320 | 64 | 0 |

Every rung is exact — down to a single token (5 × int1) and up to SIMD width (5 × int64 = five u64 registers = 64 tokens). The int8 rung (40 bits = 5 bytes, LCM(5,8)) is where the ladder meets byte-addressable memory.

**Dynamic composition**: any token count decomposes in binary, so any stream packs exactly as a sum of rungs:

```
n = 13 tokens = 8 + 4 + 1
  → 5×int8 + 5×int4 + 5×int1 = 65 bits, zero slack
```

The parser is width-blind. A stream unpacked from int2-width transport and the same stream unpacked from GPU int64 words are indistinguishable — token-identical by construction. Conformance lives at the token level; transport is a parameter.

**Why this matters for the server**: conventional databases translate between wire protocol → parsed structs → page format on disk. The rung packing layer deletes that pipeline. Packed bytes from a socket are the same bytes appended to the WAL, the same bytes mmap'd back, the same bytes handed to a comparison kernel. Zero-copy ingest, direct mmap addressing, scatter-gather sends, deterministic replication — all from one representation end to end.

```python
from griddb_rungs import pack_composed, unpack_composed

segments = pack_composed(tokens)            # [(8, units), (4, units), (1, units)]
same_tokens = unpack_composed(segments)     # identical to input
assert same_tokens == tokens
```

---

## Arithmetic — Signed Digits + Shunting-Yard

All numbers are encoded as **signed-digit tokens**. Each digit carries its own sign. No floating-point. No IEEE 754.

**Integers**: `123` → `D1 D2 D3 END`. `-123` → `N1 N2 N3 END`. `0` → `D0 END`.

**Decimal scaling**: store as integer with an `S` (Scale) annotation. `12.50` → `D1 D2 D5 D0 END T_SCALE N2 END` = "1250 with 2 decimal places." Division-free — all arithmetic is integer. Rounding is explicit.

**Shunting-Yard expression parser**: `3 + 4 * 2` → `D3 END D4 END D2 END * +` (postfix). The parser evaluates in O(n) with a stack. Operators: `+ - * / ( ) = ^`. The `^` is token T_POW (11010 in NUM context).

```
Expression:  (1 + 2) * 3
Tokens:      T_LPAREN D1 END T_PLUS D2 END T_RPAREN T_MUL D3 END
Postfix:     1 2 + 3 *
Result:      9
```

**Geometric context**: Hamming distance compares raw token bits. Manhattan distance sums absolute differences of value vectors across records. Both are O(n) on the token stream. No index required.

---

## Delimiters — RECORD, END, START

Three structural tokens define the fabric:

| Token | Binary | Purpose |
|:-----:|:------:|:--------|
| `RECORD` | 11100 | Terminates a logical tuple. Everything between two RECORDs is one record. The boundary for geometric queries. |
| `END` | 11110 | Terminates a number or word. Also pops the context stack (SPECIAL3→SPECIAL2→SPECIAL→WORD→NUM). |
| `START` | 11111 | Pushes the context stack (NUM→WORD→SPECIAL→SPECIAL2→SPECIAL3). |

**Field separation**: Numbers self-terminate with END. Words self-terminate with END. So fields are naturally separated:

```
NUM(25) END  NUM(1000) END  START A l i c e END END  RECORD
   ↑              ↑                    ↑               ↑
  age=25      balance=1000         name="Alice"    end of record
```

No comma, no tab, no JSON delimiter. The token stream IS the format. A parser that understands END and RECORD can parse any 5bit data without a schema.

---

## Architecture Layers

```
┌──────────────────────────────────────────────┐
│ Application Layer                            │
│  REST API  │  Auth  │  Storage  │  Realtime  │
├──────────────────────────────────────────────┤
│ Language Layer (SPECIAL3 verbs)               │
│  Interpreter  │  5basm Notation  │  Ownership │
├──────────────────────────────────────────────┤
│ Fivebit Libraries (optional, zero core changes) │
│  RLS Engine  │  CryptoRLS  │  PerUserGrid    │
│  CommandRLS  │  MultiMode  │  TenantGrid     │
├──────────────────────────────────────────────┤
│ Index Layer                                  │
│  HashIndex (O(1))  │  BTreeIndex (O(log n)) │
├──────────────────────────────────────────────┤
│ Transaction Layer                            │
│  Begin/Commit/Rollback  │  WAL durability    │
├──────────────────────────────────────────────┤
│ Transport Layer                              │
│  Rung Packing (5×intN ladder)                │
├──────────────────────────────────────────────┤
│ Storage Layer                                │
│  AllocGrid (O(1) point)  │  PositionedGrid   │
│  LRU Page Cache          │  WAL + SHA-256    │
└──────────────────────────────────────────────┘
```

---

## ACID — How It Works

### Atomicity (Multi-Write)

```python
txn = grid.begin()
txn.put(0, alice_tokens)   # writes to WAL as PENDING
txn.put(1, bob_tokens)     # writes to WAL as PENDING
txn.commit()                # writes TXN_COMMIT → both visible
```

Lock spans entire transaction: `begin()` acquires flock → reads happen → `commit()` writes → lock releases. No process can interleave. Crash recovery: DIRTY marker ensures torn transactions are re-applied by the next survivor.

### Consistency

Schema-free by design. The grid stores tokens — the application enforces rules. Zero metadata overhead, maximum flexibility.

### Isolation

Two complementary primitives, both cross-process:

- **`flock` per-write** — prevents data corruption. Every write acquires an exclusive file lock, fsyncs, releases.
- **`write_if` (compare-and-swap)** — prevents double-spend. A write only commits if the record hasn't changed since you read it. Reentrant-lock with depth counter ensures nested CAS doesn't release prematurely.

Verified: 24 real OS processes, 1200 increments on one hot account, zero lost updates (`griddb_concurrency_cas.py` exits 0). Transaction lock spans full read→commit, serialized cross-process.

### Durability

Every write: WAL → `fsync()` → SHA-256 chain → eventual checkpoint. Crash recovery replays WAL, discards uncommitted transactions. DIRTY marker ensures committed-but-unapplied transactions are finished by survivors on `begin()`.

---

## Performance (Python)

| Operation | 5bit |
|---|---|
| Point read (cached, warm) | ~3µs |
| Point read (uncached) | ~44µs |
| Point read (thrash, exceeds cache) | ~95µs |
| Write (group commit, batched fsync) | ~48µs (~20,800/s) |
| Compaction | Manual O(n), crash-atomic |
| Deterministic encoding | ✓ (SHA-256 content-addressed) |
| Geometry queries | Native (Manhattan, Hamming) |
| Cross-language determinism | ✓ (53/53 Python≡TS) |
| Audit trail | Append-only, every write permanent |

*Cached reads hit ~3µs when the working set fits in the LRU cache. Enable with `AllocGrid("./data", cache_size=1000)`.*

---

## Correctness Suite

The strongest evidence 5bit works:

| Test | Proves | Result |
|---|---|---|
| Sum-N single-thread | RMW atomic | ✓ zero lost |
| Sum-N threaded | Serialized correct | ✓ zero lost |
| Crash recovery (SIGKILL) | Data survives hard kill | ✓ WAL replays |
| Group commit (batched fsync) | Throughput scaling | ✓ ~20,800/s |
| WAL checkpoint | Bounded disk | ✓ |
| Multi-process CAS (24 procs) | Cross-process atomic | ✓ zero lost |
| Canonical compaction | Deterministic lifecycle | ✓ Python≡TS |

```bash
python3 griddb_correctness.py     # Python
npx tsx tests/griddb_correctness.ts  # TypeScript
python3 griddb_concurrency_cas.py    # Multi-process CAS
./verify.sh                          # Conformance + compaction
```

---

## C Engine — Ground Truth

The C implementation is the canonical reference. If Python and TypeScript disagree on a packed byte, the C engine settles it. All three produce identical output.

```
c/
├── fivebit_codec.c     Full encode+decode (byte-identical to Python)
├── fivebit_encode.c    Encoder only
├── fivebit_write.c     Write operations
├── fivebit_grid.c/h    Grid storage engine
├── fivebit_lib.c       Shared library (ctypes / ffi-napi bindable)
├── fivebit_rungs.c     Rung packing (5×intN ladder, self-test)
└── Makefile            make all
```

```bash
cd c && make all

# Python binding
python3 -c "import ctypes; lib = ctypes.CDLL('./libfivebit.so')"

# TypeScript binding
npm install ffi-napi
# const lib = ffi.Library('./libfivebit', { ... })
```

Same binary. Three languages. Same bytes every time.

---

## Project Structure

```
5bit/
├── c/                              C engine (ground truth)
│   ├── fivebit_codec.c              Full encode+decode
│   ├── fivebit_encode.c             Encoder
│   ├── fivebit_grid.c/h             Grid storage engine
│   ├── fivebit_lib.c                Shared library (ctypes/ffi)
│   ├── fivebit_write.c              Write operations
│   ├── fivebit_rungs.c              Rung packing (5×intN ladder)
│   ├── test_grid.c                  Grid test suite
│   └── Makefile                     make all
├── python/                          Core engine
│   ├── binary_grid_db.py            Tokens, encoder, parser, 5 contexts
│   ├── griddb_alloc.py              AllocGrid (O(1) reads, LRU cache, compaction)
│   ├── griddb_wal.py                WAL + SHA-256 chaining
│   ├── griddb_positioned.py         PositionedGrid (O(1) by stride)
│   ├── griddb_index.py              HashIndex + BTreeIndex
│   ├── griddb_transactions.py       ACID transactions (lock-spanned, DIRTY recovery)
│   ├── griddb_replication.py        Master/Replica HTTP
│   ├── griddb_changestream.py       SSE + long-poll from WAL
│   ├── griddb_interp.py             Interpreter — 8-verb stack machine
│   ├── griddb_asm.py                5basm notation layer (assembler/disassembler)
│   ├── griddb_rungs.py              Rung packing (5×intN ladder)
│   ├── griddb_ownership.py          Ownership layer (encode-time exclusive grants)
│   ├── griddb_correctness.py        Correctness suite
│   ├── griddb_stress.py             Stress test harness
│   ├── griddb_concurrency_cas.py    Multi-process CAS regression
│   ├── test_binary_grid_db.py       168 unit tests
│   ├── test_griddb_asm.py           Assembler gauntlet (7 checks)
│   └── test_griddb_ownership.py     Ownership gauntlet (14 checks)
├── typescript/
│   ├── src/                         Full TS port
│   │   ├── types.ts                 32 Token enum, ParserState, ParsedToken types
│   │   ├── tokens.ts                5-bit mappings (NUM/WORD/SPECIAL/SPECIAL2 + control)
│   │   ├── encoder.ts               Signed-digit integers, words, expressions, records
│   │   ├── parser.ts                FSM parser (NUM→WORD→SPECIAL→SPECIAL2→SPECIAL3)
│   │   ├── serialization.ts         5-bit ↔ 8-bit pack/unpack
│   │   ├── arithmetic.ts            Shunting-Yard + decimal arithmetic
│   │   ├── geometry.ts              Hamming/Manhattan distance on token streams
│   │   ├── checksum.ts              Modulo-32 integrity checks
│   │   ├── grid.ts                  BinaryGrid (append-only)
│   │   ├── alloc.ts                 AllocGrid (O(1) reads, LRU cache, compaction, WAL, groups)
│   │   ├── positioned.ts            PositionedGrid (O(1) by stride)
│   │   ├── indexes.ts               HashIndex + BTreeIndex
│   │   ├── replication.ts           Master/Replica (cross-process)
│   │   ├── transactions.ts          ACID transactions (lock-spanned, DIRTY recovery)
│   │   ├── changestream.ts          SSE + long-poll event stream
│   │   ├── server.ts                Standalone REST API server (no Python needed)
│   │   └── fivebit/                 Optional libs (auth, RLS, crypto, per_user, tenant, cache, commands)
│   ├── client/                      npm package (fivebit-client@0.2.2)
│   └── tests/                       48 Jest + 5 correctness + conformance
├── fivebit/                         Optional libraries (zero core changes)
│   ├── auth/                        PBKDF2 + sessions + MultiMode (zero/managed)
│   ├── rls/                         RLSEngine, CryptoRLS, PerUserGrid, CommandRLS
│   ├── tenant/                      TenantGrid (stable SHA-256 hash)
│   ├── cache.py                     LRU page cache (wired into AllocGrid)
│   └── api/                         REST API, Auth server, OAuth, Storage, Realtime
├── crypto/                          Crypto modules
│   └── python/                      HD wallet, wallet store
├── cli/                             CLI tools
├── conformance.sh                   53/53 cross-language determinism
├── verify.sh                        Conformance + canonical compaction
├── OWNERSHIP.md                     Ownership layer specification
├── PACKING.md                       Rung packing specification
├── Dockerfile                       Docker build (C lib + Python API)
└── examples/                        Transformer, explorer, benchmarks
```

---

## Quick Start

```bash
git clone https://github.com/Humansales-AI/5bit && cd 5bit

# Verify
./verify.sh

# Start API server
python3 -c "
from fivebit.api.server import APIServer
APIServer('./data', {'name':'users','fields':['balance','name']}, port=8080).start(True)
"

# Client
npm install fivebit-client
```

---

## License

MIT
