# 5bit — The C Machine: Native Execution, No Python in the Critical Path

Companion to INTERPRETER, OWNERSHIP, HOST, and PROTOCOLS. This document
records the port of the Machine to C: the organ that lets the C binary
**execute** DEF'd 5bit records, not merely store them. With it, storage +
codec + Machine live in one binary, and the unified VM-DB has no Python
anywhere in the critical path.

Status legend follows LEXICON2. [VERIFIED] = confirmed by executing
`fivebit_interp.c --selftest` (10/10) and cross-checking against the Python
reference program-for-program. [DESIGN] = intended behavior. [NOTE] = caveat.

---

## 1. Why this organ was missing

Before this port, the C side had:

- `fivebit_codec.c` — encode / decode / pack
- `fivebit_grid.c` — the storage engine (the CRM runs on it)
- `fivebit_rungs.c` — the packing ladder

That is **storage and encoding — the DB half.** The VM half (Machine,
grants, doorman, routing) existed only in Python. A C server could hold a
5bit program as bytes and could not execute a single token of it; running
one meant calling out to Python — reintroducing precisely the seam the
fabric exists to delete.

`fivebit_interp.c` closes that gap. [VERIFIED]

---

## 2. What the C Machine is

A port, not a design. Every semantic decision was already made and
conformance-tested in the Python reference (`griddb_interp.py`, 12/12);
the C implementation reproduces them exactly.

Components, all in one file (~450 lines):

| Part | Implementation |
|---|---|
| token cursor | index into the DEF'd record's token array |
| value stack | `int64_t stack[STACK_MAX]` |
| call stack | native C recursion (`fb_exec` re-entry) |
| grant table | `int32_t write_holder[MAX_SLOTS]` (slot → holder+1) |
| verbs | `switch` on 8 commands (DEF CALL RET IF LOOP BREAK STORE READ) |
| regions | balanced-paren scan (`match_region`) — structure is the address |
| doorman trap | `CALL slot >= 9000` → `host_invoke`, grant-gated |
| safety | step budget (gas), DEF-header check on load |

Control signals (`RET`, `BREAK`) propagate as result codes through the
recursive walker — no `setjmp`, no unwinding machinery. [DESIGN]

---

## 3. The properties that survived the port [VERIFIED]

| Property | Result |
|---|---|
| Arithmetic, postfix over the value stack | `(4+2)*10−5` → 55 |
| DEF enforcement — data is not executable | record without DEF header refuses to load |
| **Three-way IF** — six relations, one MINUS | `> == < >= <= !=` all correct |
| LOOP + BREAK + slots-as-variables | `sum(1..5)` = 15, `i` ends at 6 |
| **Ownership refusal, mid-program** | out=[777], slot untouched at 1000, halted at STORE |
| **Grant is the switch** | same program, granted → out=[777,999], slot=666 |
| **Doorman deny-by-default** | ungranted capability refused; granted one performs |
| Native request loop (READ_IN → rule → EMIT_OUT) | outbox = [1, 0, 0] |
| Gas bounds an infinite LOOP | terminates deterministically |
| **Native comparator** (the grid IS the XOR) | hamming=3, manhattan=12 |

---

## 4. Two-implementation conformance [VERIFIED]

The same discipline that governs the encoding (LEXICON2 §11: Python ≡ C
across the vectors) now governs execution:

```
                identical program tokens
                          │
            ┌─────────────┴─────────────┐
            ▼                           ▼
   griddb_interp.py               fivebit_interp.c
   (reference)                    (production)
            │                           │
            └──────── same output ──────┘
```

Cross-check results, identical programs, both implementations:

| Program | Python | C | |
|---|---|---|---|
| arithmetic | 55 | 55 | MATCH |
| sum(1..5) | 15 | 15 | MATCH |
| ownership refusal | out=[777], slot=1000 | out=[777], slot=1000 | MATCH |

**Conformance obligation for any future Machine (Rust, Zig, WASM, FPGA):**
run the reference programs; produce identical output, identical refusal
points, identical emitted sequences. Divergence means the binding is wrong.
[DESIGN]

---

## 5. What this unlocks

With the Machine in C, the boundary described in PROTOCOLS becomes real
rather than designed:

```
  C binary (one file, no deps beyond libc):
    ├── sockets / accept / recv / send        (effects)
    ├── fivebit_codec + rungs                 (encoding)
    ├── fivebit_grid                          (storage, WAL, CAS)
    └── fivebit_interp     ← THIS             (execution: routing, auth,
                                               handlers, signaling — native)
```

- **Signaling natively in 5bit** (PROTOCOLS §3.2) is now executable: the C
  server can load a DEF'd signaling program and run it against session
  records under grants.
- **Routing and auth in the fabric** (PROTOCOLS §2) run inside the C binary;
  the host does bytes, tokens do decisions.
- **Python's role collapses to reference implementation and tooling.** It is
  the oracle the C is checked against, not a dependency of the running
  system. [DESIGN]

[NOTE] The C Machine's slot memory in the selftest is an in-process array; the
production build wires `V_STORE`/`V_READ` to `grid_write` / `grid_read_bytes`
from `fivebit_grid.c` so program variables are durable, versioned, WAL-backed
records — same semantics as the Python reference against AllocGrid. This is a
call-site substitution, not a semantic change.

---

## 6. Where 5basm fits (a clarification)

Three distinct organs, easy to conflate:

| Organ | Job | Analogy |
|---|---|---|
| **5basm** (`griddb_asm.py`) | text notation → tokens | the assembler (the pen) |
| **Machine** (`fivebit_interp.c`) | tokens → execution | the CPU (the engine) |
| **Doorman** (host caps) | effects: sockets, clock, hash | the I/O ports |

5basm has no runtime; it *writes* programs. Something must *walk* the tokens,
and that walker is the Machine. Assembling a signaling program does not run
it — which is exactly why the C Machine was the blocking organ.

---

## 7. Quick reference

```
Build:    cc -O2 -DFB_SELFTEST -o fivebit_interp fivebit_interp.c && ./fivebit_interp
Verbs:    DEF=6 CALL=7 RET=8 IF=9 LOOP=10 BREAK=11 STORE=12 READ=13
IF:       three-way sign dispatch, ( +arm )( 0arm )( -arm )
Regions:  balanced-paren scan; structure is the address
Grants:   write_holder[slot] gates STORE and every host capability
Doorman:  CALL slot >= 9000 → host_invoke (deny-by-default)
Law:      Python is the oracle; C is the runtime; the tokens are the truth
```
