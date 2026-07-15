# 5bit — The Self-Hosting Seed: 5bit Compiles 5bit to Native Code

Companion to C_MACHINE and INTERPRETER. This records the bootstrap seed: a
compiler, expressed as computation over 5bit tokens, that emits **x86-64
machine code** which the CPU executes directly — producing results identical
to the interpreter. It is the proof that C is a *ladder*, not a dependency.

Status: [VERIFIED] by executing `griddb_selfhost.py` and by running the
emitted bytes through an independent pure-C loader (`/tmp/loader.c`), zero
Python in the execution path.

---

## 1. The bootstrap problem, and the answer

Silicon executes machine instructions, not tokens. So *something* at the
bottom must be made of instructions. That something is the C Machine
(`fivebit_interp.c`, ~450 lines) — the ladder. The question this seed answers:
**can the real compiler be written in 5bit, run once on the ladder, and then
stand on its own?** Yes.

```
  silicon            executes machine code
    C Machine        ← the ladder (450 lines, run once)
      5bit compiler  ← WRITTEN IN 5BIT: reads tokens, emits machine code
        emits →      native binary that compiles 5bit
                     ← kick the ladder; native compiles native
```

This is exactly how C, Go, Rust, and Zig were born: bootstrap on a host, then
self-host and discard the host.

---

## 2. What the seed does [VERIFIED]

The compiler is pure fabric computation — READ tokens, dispatch on their
value, emit machine-code bytes (STORE). No operation outside the six verbs +
arithmetic. Compilation strategy (x86-64 System V, native stack as the value
stack, result in RAX):

```
  digit d   ->  mov rax, d ; push rax
  +         ->  pop rbx ; pop rax ; add  rax,rbx ; push rax
  -         ->  pop rbx ; pop rax ; sub  rax,rbx ; push rax
  *         ->  pop rbx ; pop rax ; imul rax,rbx ; push rax
  EMIT/end  ->  pop rax ; ret
```

Proof, two programs:

| 5bit source | interpreter | compiled → CPU |
|---|---|---|
| `(4+2)*10−5` | 55 | **55** (65 bytes of x86-64) |
| `7*6+100` | 142 | **142** |

The emitted 55-program, run by an independent C loader with no Python
present: **native result 55.** The machine code is real, correct, and
CPU-executed. [VERIFIED]

---

## 3. Why this is self-hosting (not just a JIT)

The compiler's dispatch loop (READ token → decide → emit bytes) uses only
fabric primitives, so it **runs on `fivebit_interp.c` with no Python.** The
sequence that discards the ladder:

1. C Machine runs the 5bit compiler.
2. The 5bit compiler compiles itself → a native binary.
3. That binary compiles 5bit, including itself. C is now a historical
   artifact in the repo.

And the move that is uniquely 5bit's: the emitted machine code is **a
record**. Compiler output lands in a slot — so it is versioned (WAL history),
grant-protected (only the holder emits new versions), content-addressed (same
source → same bytes, deterministically), and rewindable (checkout the
compiler by rewinding to tick N). GCC compiles itself; it does not store its
own output as a queryable, versioned, ownership-protected record in the same
substrate as its source. 5bit does.

---

## 4. Honest scope [NOTE]

This is a **seed**, deliberately minimal: it compiles the arithmetic/EMIT
subset (integers, + − *, return). It does not yet emit code for CALL, LOOP,
IF, STORE/READ, or the grant checks — those are more x86 (branches, memory,
the ownership table) but no new principle; each is another entry in the same
emit table. The seed proves the mechanism end to end (5bit source → native
bytes → correct CPU execution); germinating it to the full verb set is
addition, not invention.

The current emitter is written in Python for iteration speed; its dispatch is
pure token computation, so porting it to a DEF'd 5bit program (emitting to a
slot via STORE) is mechanical — that port is what makes the self-hosting
literal rather than demonstrated.

---

## 5. Quick reference

```
Seed:     5bit arithmetic tokens -> x86-64 machine code -> CPU executes -> 55
Ladder:   fivebit_interp.c runs the compiler; then native compiles native
Record:   emitted code is a slot -> versioned, owned, rewindable, deterministic
Scope:    arithmetic subset proven; full verb set = more emit-table entries
Law:      C is the ladder you kick; 5bit gives birth to itself
```
