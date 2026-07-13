#!/usr/bin/env python3
"""
GridDB Ownership Layer — encode-time exclusive write grants
=============================================================
The borrow-checker replacement, first breath.

Model (slot/offset split):
  slot   = logical identity (alloc-table entry, stable forever)
  offset = physical version (changes per append; append-only)

  GRANT_W binds a SLOT   — exclusive: at most ONE live write grant per slot.
  GRANT_R pins an OFFSET — shared: any number of snapshot readers.
  REVOKE  ends a grant   — only the current holder can revoke its own.

THE LAW (encode-time refusal):
  A conflicting GRANT_W is not rejected by a checker after the fact —
  it CANNOT BE ENCODED. OwnershipEncoder.grant_w() raises EncodeRefused
  before a single token exists. Invalid ownership states are
  unrepresentable on the trusted path, in the spirit of LEXICON2 §7
  (the contract lives in the writer).

Trust model (mirrors LEXICON2 §7 exactly):
  - Trusted path: OwnershipEncoder is the only token source; the parser
    pays nothing.
  - Untrusted path: validate_ownership() is the opt-in guard that replays
    a stream's grant records and raises on a violation (hand-assembled
    double GRANT_W, revoke-by-stranger, etc.).

Wire format (built ONLY from existing LEXICON2 primitives — no new tokens,
no parser changes; a grant record is an ordinary record):

  GRANT_W slot S to holder H:   encode_command('GRANT_W', H) · encode_integer(S) · RECORD
  GRANT_R slot S offset O by H: encode_command('GRANT_R', H) · encode_integer(S) · encode_integer(O) · RECORD
  REVOKE  slot S by holder H:   encode_command('REVOKE',  H) · encode_integer(S) · RECORD

  Parses with the existing Parser as:
    {'type':'command','cmd':...}, ParsedNumber(slot) [, ParsedNumber(offset)], RECORD

Concurrency: the in-process ledger is lock-serialized (threads race, one
wins). Cross-process, the ledger record itself is the CAS target: append
grant records through AllocGrid.write_if so the existing verified CAS
resolves multi-writer races — no new machinery.

Determinism: the ledger IS the grant stream. GrantTable.replay(events)
rebuilds the exact table from the records alone; state is derivable,
append-only, and audit-complete.
"""
from __future__ import annotations

import threading
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Set, Tuple

from binary_grid_db import (
    Token, Encoder, Parser, ParsedNumber,
)


# ────────────────────────────────────────────────────────────────────────────
# Errors
# ────────────────────────────────────────────────────────────────────────────

class EncodeRefused(Exception):
    """Raised BEFORE any token is produced. The stream that would violate
    ownership never exists."""


class MalformedOwnership(ValueError):
    """Raised by the opt-in validator on an untrusted stream that encodes
    an ownership violation."""


# ────────────────────────────────────────────────────────────────────────────
# Grant ledger (deterministic, replayable)
# ────────────────────────────────────────────────────────────────────────────

@dataclass
class GrantTable:
    """Authoritative live-grant state. Derivable purely by replaying grant
    records (append-only) — no hidden state."""
    write_holder: Dict[int, int] = field(default_factory=dict)   # slot -> holder
    read_pins: Dict[int, Set[Tuple[int, int]]] = field(default_factory=dict)
    # slot -> {(holder, offset)}  (shared snapshot pins; informational)

    def live_writer(self, slot: int) -> Optional[int]:
        return self.write_holder.get(slot)

    # -- transition rules (pure; raise on violation) --
    def apply_grant_w(self, slot: int, holder: int) -> None:
        cur = self.write_holder.get(slot)
        if cur is not None:
            raise EncodeRefused(
                f"GRANT_W refused: slot {slot} already write-held by holder "
                f"{cur} (requested by {holder}); REVOKE first")
        self.write_holder[slot] = holder

    def apply_grant_r(self, slot: int, holder: int, offset: int) -> None:
        self.read_pins.setdefault(slot, set()).add((holder, offset))

    def apply_revoke(self, slot: int, holder: int) -> None:
        cur = self.write_holder.get(slot)
        if cur is None:
            raise EncodeRefused(f"REVOKE refused: slot {slot} has no live write grant")
        if cur != holder:
            raise EncodeRefused(
                f"REVOKE refused: slot {slot} held by {cur}, not {holder} "
                f"(only the holder revokes its own grant)")
        del self.write_holder[slot]

    # -- determinism: rebuild from events --
    @classmethod
    def replay(cls, events: List[Tuple[str, int, int, Optional[int]]]) -> "GrantTable":
        """events: (cmd, holder, slot, offset|None) in stream order."""
        t = cls()
        for cmd, holder, slot, offset in events:
            if cmd == 'GRANT_W':
                t.apply_grant_w(slot, holder)
            elif cmd == 'GRANT_R':
                t.apply_grant_r(slot, holder, offset if offset is not None else -1)
            elif cmd == 'REVOKE':
                t.apply_revoke(slot, holder)
        return t


# ────────────────────────────────────────────────────────────────────────────
# The encoder — the only token source on the trusted path
# ────────────────────────────────────────────────────────────────────────────

class OwnershipEncoder:
    """Encode-time enforcement. Conflicting grants raise EncodeRefused
    before any token exists; successful calls return the grant record's
    tokens AND atomically update the ledger under one lock (check+emit is
    a single critical section — no TOCTOU between threads)."""

    def __init__(self, table: Optional[GrantTable] = None):
        self.table = table or GrantTable()
        self._lock = threading.Lock()
        self.event_log: List[Tuple[str, int, int, Optional[int]]] = []

    # -- grant primitives --
    def grant_w(self, slot: int, holder: int) -> List[Token]:
        with self._lock:
            self.table.apply_grant_w(slot, holder)      # raises -> zero tokens
            self.event_log.append(('GRANT_W', holder, slot, None))
            return [
                *Encoder.encode_command('GRANT_W', holder),
                *Encoder.encode_integer(slot),
                Token.RECORD,
            ]

    def grant_r(self, slot: int, holder: int, offset: int) -> List[Token]:
        """Shared snapshot pin. Always encodable (append-only storage makes
        pinned offsets immutable by construction)."""
        with self._lock:
            self.table.apply_grant_r(slot, holder, offset)
            self.event_log.append(('GRANT_R', holder, slot, offset))
            return [
                *Encoder.encode_command('GRANT_R', holder),
                *Encoder.encode_integer(slot),
                *Encoder.encode_integer(offset),
                Token.RECORD,
            ]

    def revoke(self, slot: int, holder: int) -> List[Token]:
        with self._lock:
            self.table.apply_revoke(slot, holder)        # raises -> zero tokens
            self.event_log.append(('REVOKE', holder, slot, None))
            return [
                *Encoder.encode_command('REVOKE', holder),
                *Encoder.encode_integer(slot),
                Token.RECORD,
            ]

    # -- guarded write: the point of it all --
    def write_with_grant(self, grid, slot: int, holder: int,
                         tokens: List[Token]) -> int:
        """Append a new version of `slot` iff `holder` holds the live
        write grant. The grant check gates the append; readers pinned to
        old offsets are untouched (append-only)."""
        with self._lock:
            cur = self.table.write_holder.get(slot)
            if cur != holder:
                raise EncodeRefused(
                    f"WRITE refused: slot {slot} write grant is "
                    f"{'unheld' if cur is None else f'held by {cur}'}, "
                    f"not {holder}")
        return grid.write(slot, tokens)


# ────────────────────────────────────────────────────────────────────────────
# Opt-in validator for untrusted streams (LEXICON2 §7 guard pattern)
# ────────────────────────────────────────────────────────────────────────────

_GRANT_CMDS = ('GRANT_W', 'GRANT_R', 'REVOKE')


def extract_grant_events(tokens: List[Token]) -> List[Tuple[str, int, int, Optional[int]]]:
    """Parse a stream with the EXISTING parser and pull grant events out of
    its output: command dict, then slot number (and offset for GRANT_R)."""
    parser = Parser()
    parser.feed_tokens(list(tokens))
    out = parser.output
    # Stream shape per grant record: command, holder NUM (from
    # encode_command's arg), slot NUM [, offset NUM], RECORD.
    events: List[Tuple[str, int, int, Optional[int]]] = []
    i = 0
    while i < len(out):
        item = out[i]
        if isinstance(item, dict) and item.get('type') == 'command' \
                and item.get('cmd') in _GRANT_CMDS:
            cmd = item['cmd']
            need = 3 if cmd == 'GRANT_R' else 2   # holder, slot[, offset]
            nums: List[int] = []
            j = i + 1
            while j < len(out) and len(nums) < need:
                nxt = out[j]
                if isinstance(nxt, ParsedNumber):
                    nums.append(nxt.value)
                    j += 1
                    continue
                if nxt is Token.RECORD or (isinstance(nxt, dict) and nxt.get('type') == 'command'):
                    break
                j += 1
            if len(nums) < need:
                raise MalformedOwnership(
                    f"{cmd} record malformed: expected {need} numbers "
                    f"(holder, slot{', offset' if need == 3 else ''}), got {len(nums)}")
            holder, slot = nums[0], nums[1]
            offset = nums[2] if need == 3 else None
            events.append((cmd, holder, slot, offset))
            i = j
        else:
            i += 1
    return events


def validate_ownership(tokens: List[Token]) -> GrantTable:
    """Opt-in guard: replay grant events from an untrusted stream; raise
    MalformedOwnership on any violation. Returns the final table."""
    events = extract_grant_events(tokens)
    try:
        return GrantTable.replay(events)
    except EncodeRefused as e:
        raise MalformedOwnership(str(e)) from e
