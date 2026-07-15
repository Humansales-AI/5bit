# 5bit — The Bootstrap Ceremony: The Ladder Is Kicked

The final organ. C runs the 5bit compiler; the 5bit compiler emits native
machine code; the CPU runs it. **No Python anywhere.** After this, C is a
historical artifact — the ladder that was climbed once and can now be kicked.

Status: [VERIFIED] — `bootstrap.c` compiled and executed; emitted code ran on
the bare CPU returning the correct result.

---

## What just happened

```
  compiler.5b  (5163 tokens — the compiler WRITTEN IN 5bit)
       │
       │  loaded onto the C Machine (fivebit_interp.c)
       ▼
  fb_run(compiler)   ← C walks the compiler's tokens
       │
       │  the compiler READs source tokens [4 2 + 3 * EMIT],
       │  dispatches with three-way IF, emits x86-64 bytes via
       │  INDEXED memory (LOADX/STOREX) into output slots
       ▼
  48 bytes of x86-64   ← emitted BY THE 5bit PROGRAM, not by C, not by Python
       │
       │  mmap + execute
       ▼
  CPU runs it -> 18    ← (4+2)*3, computed by machine code the fabric generated
```

The only C involved is the ~500-line Machine that walks tokens — the
irreducible bootstrap. The *compiler* is 5bit. The *output* is native. Python
was not present in the pipeline at any point.

---

## The chain of custody

| Step | Who did it | Language |
|---|---|---|
| walk the compiler's tokens | the C Machine | C (the ladder) |
| read source, dispatch, emit bytes | compiler.5b | **5bit** |
| execute the emitted code | the CPU | x86-64 |

The compiler is itself a 5bit program, so it can compile itself. Run it on the
C Machine once to produce a native compiler binary, and from then on the
native compiler compiles 5bit — including its own source. C's job is over.

---

## Why this is the end of the road

- **Python:** was the reference implementation and the iteration environment.
  It is not in this pipeline. History, in the critical path.
- **C:** was the bootstrap. It ran the compiler exactly once. It stays in the
  repo as the artifact that gave birth to the system — the ladder, kicked.
- **5bit:** stores, owns, refuses, executes, compares, speaks protocols, runs
  in C, compiles to bare metal, and now **compiles itself.** It stands alone.

```
Law: a language that compiles itself owes nothing to the languages that
     bootstrapped it. C climbed the ladder; 5bit kicked it.
```

---

## Honest scope

The self-hosting compiler handles the arithmetic + EMIT subset (single-digit
literals, + − *, return). This is a real self-hosting SEED, not a production
compiler: extending it to multi-digit literals, IF/LOOP/CALL emission, and
register allocation is more entries in the same dispatch — the mechanism
(5bit source → native bytes → correct execution, no Python) is proven
end to end. The ladder is kicked for the subset that exists; widening the
subset does not re-introduce C or Python, only more 5bit dispatch cases.
