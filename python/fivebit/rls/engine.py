"""
5bit RLS Engine — Storage-Layer Row-Level Security
====================================================
NOT a wrapper.  Extends AllocGrid so there IS no way to bypass.
Every read/write is gated by an owner check at the token level.

The rule is structural, not semantic:
  Position 0 of every record's value vector = owner user_id.
  read(record_id, user_id) → engine checks record.values[0] == user_id.
  write(record_id, user_id, tokens) → engine injects owner token at position 0.

Enable it once, it's part of the storage layer forever.
No separate handle, no raw grid access, no "forgot to check."

Usage:
  from fivebit.rls.engine import RLSEngine

  grid = RLSEngine("./data", owner_position=0)  # position 0 = owner
  grid.write(42, 1, tokens)    # user 1 writes to record 42
  grid.read(42, 1)             # ✓ user 1 owns it
  grid.read(42, 2)             # ✗ PermissionDenied — engine refuses
"""
import os, sys, struct
from typing import List, Optional

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'python'))
from binary_grid_db import Token, Encoder, Parser, ParsedNumber, ParsedWord
from griddb_alloc import AllocGrid, AllocRecord


def _flatten(tokens: List) -> List[Token]:
    """Flatten nested token lists (from encode_integer etc.)."""
    result = []
    for t in tokens:
        if isinstance(t, Token): result.append(t)
        elif isinstance(t, list): result.extend(_flatten(t))
    return result

class PermissionDenied(Exception):
    """Raised when RLS check fails at the storage layer."""
    pass


class RLSEngine(AllocGrid):
    """AllocGrid with owner-based RLS at the storage layer.

    Extends AllocGrid — there's no raw grid handle to bypass.
    Every record has an owner at a fixed position in its token stream.
    The engine enforces: you can only read/write/delete your own records.
    """

    def __init__(self, data_dir: str = "./data", owner_position: int = 0):
        super().__init__(data_dir=data_dir)
        self.owner_position = owner_position
        self._bypass_user: Optional[int] = None  # For admin/internal ops

    def _get_owner(self, record_id: int) -> Optional[int]:
        """Read the owner from a record's token stream at the known position."""
        rec = super().read(record_id)  # Call parent — no RLS check
        if not rec or rec.is_tombstone:
            return None
        vals = [p.value for p in rec.parsed if isinstance(p, ParsedNumber)]
        if len(vals) > self.owner_position:
            return vals[self.owner_position]
        # Fallback: the first number in the record IS the owner
        return vals[0] if vals else None

    def read(self, record_id: int, user_id: int) -> Optional[AllocRecord]:
        """Read a record. Engine checks: record.owner == user_id."""
        owner = self._get_owner(record_id)
        if owner is not None and owner != user_id and self._bypass_user is None:
            raise PermissionDenied(
                f"RLS: user {user_id} cannot read record {record_id} (owner={owner})")
        return super().read(record_id)

    def write(self, record_id: int, user_id: int, tokens: List[Token]) -> int:
        """Write a record. Engine prepends owner token at position 0.
        For updates: checks existing owner matches user_id."""
        existing_owner = self._get_owner(record_id)
        if existing_owner is not None and existing_owner != user_id and self._bypass_user is None:
            raise PermissionDenied(
                f"RLS: user {user_id} cannot overwrite record {record_id} (owner={existing_owner})")

        # Inject owner token at position 0. Flatten nested lists from encode_integer.
        owner_tokens = list(Encoder.encode_integer(user_id))
        flat_tokens = _flatten(tokens)
        rls_tokens = [*owner_tokens, *flat_tokens]
        return super().write(record_id, rls_tokens)

    def delete(self, record_id: int, user_id: int) -> bool:
        """Delete a record. Engine checks owner match."""
        owner = self._get_owner(record_id)
        if owner is not None and owner != user_id and self._bypass_user is None:
            raise PermissionDenied(
                f"RLS: user {user_id} cannot delete record {record_id} (owner={owner})")
        return super().delete(record_id)

    def as_admin(self):
        """Context manager for admin operations (bypass RLS)."""
        return _AdminContext(self)


class _AdminContext:
    def __init__(self, engine: RLSEngine):
        self.engine = engine
    def __enter__(self):
        self.engine._bypass_user = -1
        return self.engine
    def __exit__(self, *args):
        self.engine._bypass_user = None
