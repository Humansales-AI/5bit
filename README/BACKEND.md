# 5bit — Full Native Backend: Every Verb Compiled to x86-64

Companion to SELFHOST and C_MACHINE. The seed germinated to the whole tree.
The complete executable subset of 5bit now compiles to native x86-64 machine
code and runs on the bare CPU — arithmetic, three-way IF, LOOP/BREAK,
READ/STORE, grant-checked STORE, CALL/RET, EMIT.

Status: [VERIFIED] — `test_griddb_backend.py`, 8/8, machine code executed on
the CPU via ctypes and cross-checked against the interpreter.

---

## 1. What compiles now

| Construct | Lowering to x86-64 |
|---|---|
| integer literal | `mov rax,imm ; vpush` |
| `+ - *` | `vpop rbx ; vpop rax ; add/sub/imul ; vpush` |
| **IF (three-way)** | `vpop rax ; cmp rax,0 ; jg +arm ; je 0arm ; jmp -arm` — real branches |
| **LOOP / BREAK** | backward `jmp` to loop-top; BREAK = `jmp` to patched break label |
| **READ slot** | `mov rax,[r15+slots+slot*8] ; vpush` |
| **STORE slot** | **grant guard compiled inline** (below), then `mov [slots+slot],rax` |
| **CALL / RET** | native `call rel32` / `ret` over compiled program labels |
| EMIT | append to the output buffer in the arena |

Determinism is what makes the compiler a **table, not an analysis**: one
lowering per construct, balanced regions so jump targets are structural (no
address arithmetic), a flat grant array. No type inference, no aliasing
analysis, no GC. [VERIFIED]

---

## 2. The register model

```
  R15 = arena base       R14 = unwind rsp (entry)      R13 = value-stack ptr
```

The value stack lives in **R13**, dedicated and separate from the call stack
(RSP). This is the one non-obvious decision: if values and return addresses
shared RSP, a CALL'd program that pushed a value would corrupt `ret`. Splitting
them makes CALL/RET and the value stack independent — verified by B6. [VERIFIED]

Arena (one mmap'd int64 array): `slots | grant | out | out_count | holder |
vstack`. REFUSED sentinel = INT64_MIN.

---

## 3. Ownership enforced in machine code [VERIFIED]

The decisive result: the grant check is not a Python wrapper — it is **emitted
x86-64**. STORE compiles to:

```
    mov  rax, [r15 + grant + slot*8]     ; the slot's grant holder
    mov  rcx, [r15 + holder]             ; the running holder
    inc  rcx                             ; (+1 convention)
    cmp  rax, rcx
    jne  trap                            ; mismatch -> refuse
    ; --- allowed ---
    vpop rax ; mov [slots+slot], rax
    jmp  done
  trap:                                  ; --- refused, mid-program ---
    mov  rsp, r14                         ; unwind call stack
    lea  r13, [r15+vtop]                  ; reset value stack
    mov  rax, REFUSED                     ; sentinel
    jmp  epilogue                         ; abort the whole run, grid untouched
  done:
```

Verified (B5): holder 2 running a program that STOREs to a slot held by
holder 1 → the native code returns REFUSED, output stopped at the pre-STORE
emit, the slot's value **unchanged**. Grant the slot to holder 2 → the
identical machine code completes and writes. **Effects are grants — in
x86-64, not in a language runtime above it.**

---

## 4. Showcase: the CRM rule as native code [VERIFIED]

The real business rule — "if deal value > 5000, promote to Qualified" —
compiled to 225 bytes of x86-64, grant-gated:

```
  deal value 7000 -> stage 2   (promoted)
  deal value 4000 -> stage 1   (unchanged)
  deal value 5000 -> stage 1   (strict >, unchanged)
```

A CRM business rule, running as native machine code, with ownership enforced
by the CPU. No interpreter in the loop, no Python, no framework.

---

## 5. Gauntlet [VERIFIED — 8/8]

| # | Property |
|---|---|
| B1 | arithmetic `(4+2)*10−5` = 55 |
| B2 | three-way IF: all six relations as native jumps |
| B3 | READ/STORE against the slot arena |
| B4 | LOOP+BREAK+slots: sum(1..5)=15 |
| B5 | **grant check in machine code**: ungranted STORE → REFUSED, slot untouched; granted → completes |
| B6 | CALL/RET across compiled programs |
| B7 | interpreter == native, program for program |
| — | CRM rule (value>5000→promote) native, grant-gated, correct |

---

## 6. Scope & the self-hosting close

This backend is written in Python for iteration speed; its dispatch is pure
token→bytes computation (a table lookup per construct), so porting it to a
DEF'd 5bit program that emits to a slot is mechanical. When that port lands,
the loop closes literally: the 5bit compiler, written in 5bit, running on the
C Machine, emits native code — including its own. C becomes the ladder you
kick.

What remains for a production backend (not principle, just more table
entries): register allocation for speed (currently a memory value-stack),
multi-argument calling conventions, and wiring STORE/READ to grid_write/
grid_read_bytes so native program state is durable WAL-backed records.

```
Law: 5bit is compilable to bare metal; ownership survives the compile;
     the CPU itself enforces the grants.
```
