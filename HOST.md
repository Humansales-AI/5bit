# 5bit — The Doorman: Host Interface (Effects as Grant-Gated Capabilities)

Companion to LEXICON2, PACKING, OWNERSHIP, and INTERPRETER. This document
specifies the boundary between the fabric and the world: how a 5bit program
performs effects it cannot compute — socket I/O, clock, disk fsync, hashing,
randomness — by asking a host to touch the world on its behalf, gated by the
same grant discipline that governs storage.

Status legend follows LEXICON2. [VERIFIED] = confirmed by executing the
gauntlet (`test_griddb_host.py`, 7/7) against the reference Python doorman.
[DESIGN] = intended behavior. [NOTE] = honest caveat.

---

## 1. Why a host exists at all

The six verbs are Turing-complete: they compute any function. But a function
is `input → output` — pure. **Effects are not functions.** "Receive a
socket," "read the clock," "fsync to disk" are physical acts on hardware; they
are the runtime *reaching out and touching the world*, which no token stream
can express. Every language faces this: Python delegates to C, C to syscalls,
syscalls to silicon. Turtles down to the voltage.

So the irreducible non-fabric layer is not "Python" and not "C" — it is **a
host**: something that turns a program's request into an interrupt. The
host can be written in anything; the 5bit CRM proves it can be a 50KB C
binary with zero Python. This layer is the reference host ("the doorman") in
Python, defined by a contract so the fabric side is verified independently of
which language mans the door. [DESIGN]

---

## 2. Mechanism: CALL into the reserved slot range

Slots split into two ranges: [VERIFIED]

```
  0 .. HOST_BASE-1     normal DEF'd 5bit programs   (compute)
  HOST_BASE ..         host capabilities            (effects)   [HOST_BASE = 9000]
```

A CALL to a host slot is **trapped before the program loader sees it** and
routed to the doorman instead of executed. The doorman:

1. **checks the running holder holds a grant over that capability slot** —
   deny-by-default, the identical discipline as STORE (OWNERSHIP §5);
2. pops the capability's arguments from the value stack;
3. asks the host to perform the physical effect;
4. pushes results (if any) back onto the stack.

Effects are therefore **capabilities**: a program can only touch the world
through doors it was granted. The fabric stays pure, the world stays behind
the door, and **the grant table is the access-control list for reality.**
[VERIFIED — H1, H2, H3]

[NOTE] In the reference, the trap is a wrapper around `Machine.call` — no
interpreter-core change. This mirrors a C host dispatching on slot range
before its program loader runs; the interpreter never needs to know the
door exists.

---

## 3. Capability slots (reference set)

| Slot | Cap | Args → Result | Effect |
|---|---|---|---|
| 9000 | NOW | () → t | unix-epoch seconds |
| 9001 | RANDOM | () → r | random int [0, 2³¹) |
| 9002 | HASH | (n) → h | stable 31-bit hash of n |
| 9003 | EMIT_OUT | (n) → — | host output sink (socket/stdout) |
| 9004 | LOG | (n) → — | host log sink |
| 9005 | READ_IN | () → n | next host input value (request) |

The contract is deliberately tiny and integer-in / integer-out. Richer
effects (byte buffers, full sockets) compose from these plus the fabric's own
record I/O — the doorman moves scalars and records; structure stays in 5bit.
[DESIGN]

---

## 4. Deny-by-default: effects are capabilities [VERIFIED]

```
program: EMIT_OUT(42)   with NO grant  -> HostRefused, mid-program, outbox empty   (H2)
grant EMIT_OUT to holder, same program -> outbox = [42]                            (H3)
```

The grant is the switch, exactly as for STORE: a program's reach into the
world is bounded by a capability table the runtime cannot be talked out of,
because the refusal is the ownership layer's, not the program's. A program
that was never granted the socket cannot open the socket — not by policy, by
construction.

---

## 5. A full request loop in pure 5bit [VERIFIED — H4]

The endgame, demonstrated: a request handler with **no host-side logic** —
the doorman only moves bytes through the door; every decision is tokens.

```
READ_IN            ; pull request value from the world
100 MINUS          ; compare to threshold
IF ( +arm: 1 EMIT_OUT )    ; > 100  -> approve
   ( 0arm: 0 EMIT_OUT )    ; == 100 -> deny
   ( -arm: 0 EMIT_OUT )    ; < 100  -> deny
```

Requests `[250, 30, 100]` → outbox `[1, 0, 0]`. The rule lived in the fabric;
the host provided only the door. This is the shape of an entire application:
compute native, effects granted.

---

## 6. Determinism & audit

- **Injected seams.** Clock, randomness, and input are injectable, so the
  fabric side is byte-for-byte testable; the C doorman swaps these for real
  syscalls without changing the contract. Same seams → identical output,
  twice. [VERIFIED — H7]
- **Hashing is stable.** HASH(n) is deterministic across independent runs.
  [VERIFIED — H5]
- **Every door is logged.** The doorman records each capability invocation;
  ungranted attempts perform no effect and leave the world untouched. The
  audit trail is the complete list of the program's contact with reality.
  [VERIFIED — H6]

---

## 7. What this closes

With the doorman, the layering is complete and the roles are exact:

```
  5bit programs   — ALL logic: rules, comparison, similarity, control flow
  the doorman     — the only path to effects; a few hundred lines, any language
  the kernel      — performs the physical act
```

Python/TypeScript are now needed for **nothing in the critical path**. They
remain welcome as *hosts* (a doorman can be written in them) and as
*embedders* (5bit-as-a-library inside an existing app) — both choices, not
dependencies. The systems layer that must exist is the doorman, and the
doorman is already demonstrably a small C binary. [DESIGN — architecture;
VERIFIED — the fabric-side contract]

---

## 8. Gauntlet summary [VERIFIED — 7/7]

| # | Property |
|---|---|
| H1 | Granted program reads host clock via NOW and emits it |
| H2 | Deny-by-default: ungranted capability refused mid-program, world untouched |
| H3 | Grant is the switch: same program performs the effect once granted |
| H4 | Full request loop in pure 5bit: READ_IN → rule → EMIT_OUT, no host logic |
| H5 | HASH deterministic across independent runs |
| H6 | Every capability invocation audited; ungranted attempts leave no effect |
| H7 | Injected seams → byte-identical output across runs |

---

## 9. Open rulings (for the architect)

1. **Byte/buffer effects.** The scalar contract composes to bytes via records,
   but a first-class buffer capability (socket read → record) may earn a slot.
2. **Capability granularity.** One grant per capability slot today. Finer
   scoping (e.g. "EMIT_OUT only to connection N") could ride the grant's
   argument. Architect's call.
3. **Host slot range.** HOST_BASE = 9000 is a convention; a reserved SPECIAL3
   marker could distinguish host-CALL from program-CALL structurally instead
   of by range, if you prefer structure over convention.

---

## 10. Quick reference

```
Range:    slots >= 9000 are host capabilities; a CALL there asks the doorman
Gate:     capability requires a grant (deny-by-default, like STORE)
Caps:     NOW RANDOM HASH EMIT_OUT LOG READ_IN  (integer in/out; compose for bytes)
Loop:     READ_IN -> native rule -> EMIT_OUT   = a whole app, logic in fabric
Law:      the grant table is the access-control list for reality;
          Python/TS are hosts or embedders — choices, never critical-path
```
