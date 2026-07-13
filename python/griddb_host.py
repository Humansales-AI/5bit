#!/usr/bin/env python3
"""
The Doorman — 5bit host interface (effects as grant-gated capabilities)
========================================================================
The last organ. Everything below the fabric — sockets, clock, disk fsync,
hashing, random — is EFFECT, not computation: physical acts on hardware a
token stream cannot perform, because "receive a socket" is not a function,
it is the runtime reaching out and touching the world.

The doorman is the boundary where a 5bit program asks the host to touch the
world for it. In production this layer is a few hundred lines of C (your CRM
already proves it: the doorman there is a 50KB C binary, zero Python). This
module is the reference doorman in Python — the same contract, so the fabric
side is verified independently of which language mans the door.

MECHANISM — CALL into the reserved slot range = "ask the doorman":

    slots 0 .. HOST_BASE-1      normal DEF'd 5bit programs (compute)
    slots HOST_BASE ..          HOST CAPABILITIES (effects)

  A CALL to a host slot does NOT run a program. It:
    1. checks the running holder holds a GRANT over that capability slot
       (deny-by-default: no grant -> EncodeRefused, exactly like STORE),
    2. pops arguments from the value stack,
    3. asks the host to perform the physical effect,
    4. pushes results back onto the stack.

  So effects are capabilities: a program can only touch the world through
  doors it was granted. The fabric stays pure; the world stays behind the
  door; the grant table is the access-control list for reality.

CAPABILITY SLOTS (reserved, stable):
    9000 NOW        ()            -> push unix-epoch seconds
    9001 RANDOM     ()            -> push a random int in [0, 2^31)
    9002 HASH       (n)           -> push a stable 31-bit hash of n
    9003 EMIT_OUT   (n)           -> host output sink (e.g. socket/stdout)
    9004 LOG        (n)           -> host log sink
    9005 READ_IN    ()            -> push next host input value (e.g. request)

The contract is deliberately tiny and integer-in/integer-out: richer effects
(byte buffers, sockets) compose from these + the fabric's own record I/O.
"""
from __future__ import annotations

import hashlib
import time
from typing import Callable, Dict, List, Optional

from griddb_ownership import OwnershipEncoder, EncodeRefused

HOST_BASE = 9000
CAP_NOW, CAP_RANDOM, CAP_HASH = 9000, 9001, 9002
CAP_EMIT_OUT, CAP_LOG, CAP_READ_IN = 9003, 9004, 9005

CAP_NAMES = {CAP_NOW: 'NOW', CAP_RANDOM: 'RANDOM', CAP_HASH: 'HASH',
             CAP_EMIT_OUT: 'EMIT_OUT', CAP_LOG: 'LOG', CAP_READ_IN: 'READ_IN'}
CAP_ARITY = {CAP_NOW: 0, CAP_RANDOM: 0, CAP_HASH: 1,
             CAP_EMIT_OUT: 1, CAP_LOG: 1, CAP_READ_IN: 0}


class HostRefused(EncodeRefused):
    """A program invoked a capability it was not granted. Same family as
    STORE's refusal: effects are deny-by-default."""


class Doorman:
    """Reference host. Deterministic seams (clock, random, input) are
    injectable so the fabric side is testable byte-for-byte; the C doorman
    swaps these for real syscalls."""

    def __init__(self, ownership: OwnershipEncoder,
                 clock: Optional[Callable[[], int]] = None,
                 rng: Optional[Callable[[], int]] = None,
                 inbox: Optional[List[int]] = None):
        self.own = ownership
        self._clock = clock or (lambda: int(time.time()))
        self._rng = rng
        self.inbox: List[int] = list(inbox or [])
        self.outbox: List[int] = []      # what programs sent to the world
        self.log: List[int] = []         # what programs logged
        self.calls: List[str] = []       # audit: every door opened
        self._rng_state = 0x5b17

    # -- deny-by-default gate: identical discipline to STORE --
    def _require(self, cap: int, holder: int) -> None:
        if self.own.table.write_holder.get(cap) != holder:
            cur = self.own.table.write_holder.get(cap)
            raise HostRefused(
                f"CAP {CAP_NAMES.get(cap, cap)} refused: holder {holder} "
                f"lacks grant (held by {cur})")

    def _rand(self) -> int:
        if self._rng:
            return self._rng()
        self._rng_state = (self._rng_state * 1103515245 + 12345) & 0x7fffffff
        return self._rng_state

    def is_host_slot(self, slot: int) -> bool:
        return slot >= HOST_BASE

    def invoke(self, cap: int, holder: int, stack: List[int]) -> None:
        """Perform a granted effect. Pops arity args from `stack`, pushes
        result (if any). Refuses without a grant."""
        if cap not in CAP_NAMES:
            raise HostRefused(f"unknown capability slot {cap}")
        self._require(cap, holder)
        arity = CAP_ARITY[cap]
        args = [stack.pop() for _ in range(arity)][::-1]
        self.calls.append(f"{CAP_NAMES[cap]}({','.join(map(str, args))})")

        if cap == CAP_NOW:
            stack.append(self._clock())
        elif cap == CAP_RANDOM:
            stack.append(self._rand())
        elif cap == CAP_HASH:
            h = int.from_bytes(hashlib.sha256(str(args[0]).encode()).digest()[:4],
                               'big') & 0x7fffffff
            stack.append(h)
        elif cap == CAP_EMIT_OUT:
            self.outbox.append(args[0])
        elif cap == CAP_LOG:
            self.log.append(args[0])
        elif cap == CAP_READ_IN:
            stack.append(self.inbox.pop(0) if self.inbox else -1)


def attach_doorman(machine, doorman: Doorman) -> None:
    """Wire the doorman into a Machine: a CALL to a host slot is trapped and
    routed to the doorman instead of executed as a program. Pure monkey-
    patch of `call` — no interpreter core changes, mirroring how the C host
    dispatches on slot range before the program loader ever sees it."""
    inner_call = machine.call

    def call(slot: int) -> None:
        if doorman.is_host_slot(slot):
            machine.trace.append(f"HOST {CAP_NAMES.get(slot, slot)}")
            doorman.invoke(slot, machine.holder, machine.stack)
            return
        inner_call(slot)

    machine.call = call
