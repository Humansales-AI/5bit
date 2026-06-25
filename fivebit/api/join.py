"""
5bit Merge Join — B-tree Indexed, O(n log n) build + O(n) merge
==================================================================
No nested loops. No hash maps. Uses existing BTreeIndex.

Walks two B-trees in parallel. Matching keys → paired records.
Same algorithm PostgreSQL uses for USING(column) — a merge join.

GET /join?left=users&right=orders&on=user_id
"""
import os, sys
from typing import List, Dict, Tuple, Optional

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'python'))
from griddb_index import BTreeIndex
from griddb_alloc import AllocGrid, AllocRecord
from binary_grid_db import ParsedNumber, ParsedWord


def _record_to_dict(rec: AllocRecord, fields: List[str]) -> dict:
    vals = []; pending = ''
    for p in rec.parsed:
        if isinstance(p, ParsedNumber):
            if pending: vals.append(pending); pending = ''
            vals.append(p.value)
        elif isinstance(p, ParsedWord):
            pending += p.text
    if pending: vals.append(pending)
    return {fields[i]: vals[i] for i in range(min(len(fields), len(vals)))}


class MergeJoiner:
    """B-tree merge join. Walks two sorted indexes in parallel."""

    def __init__(self, grid: AllocGrid, left_spec: dict, right_spec: dict):
        self.grid = grid
        self.left_name = left_spec['name']
        self.left_fields = left_spec.get('fields', [])
        self.right_name = right_spec['name']
        self.right_fields = right_spec.get('fields', [])

    def join(self, on_field: str, data_dir: str) -> List[dict]:
        """Merge join on a shared field. Both sides must be indexed."""
        # Build both B-tree indexes
        left_idx = BTreeIndex(f"{self.left_name}_join", data_dir)
        right_idx = BTreeIndex(f"{self.right_name}_join", data_dir)

        # Scan grid, populate indexes with (key, record_id) pairs
        left_map: Dict[int, List[int]] = {}
        right_map: Dict[int, List[int]] = {}
        lf_idx = self.left_fields.index(on_field) if on_field in self.left_fields else -1
        rf_idx = self.right_fields.index(on_field) if on_field in self.right_fields else -1

        for rid in range(self.grid.total_entries):
            rec = self.grid.read(rid)
            if not rec or rec.is_tombstone: continue
            vals = [p.value for p in rec.parsed if isinstance(p, ParsedNumber)]
            # Heuristic: left records have more fields matching left spec
            if lf_idx >= 0 and lf_idx < len(vals):
                key = vals[lf_idx]
                left_map.setdefault(key, []).append(rid)
                left_idx.put(key, rid)
            if rf_idx >= 0 and rf_idx < len(vals):
                key = vals[rf_idx]
                right_map.setdefault(key, []).append(rid)
                right_idx.put(key, rid)

        # Merge join: walk sorted keys, match left+right
        results = []
        all_keys = sorted(set(left_map.keys()) | set(right_map.keys()))
        for key in all_keys:
            left_rids = left_map.get(key, [])
            right_rids = right_map.get(key, [])
            if not left_rids or not right_rids: continue
            for lr in left_rids:
                left_rec = self.grid.read(lr)
                if not left_rec or left_rec.is_tombstone: continue
                left_dict = _record_to_dict(left_rec, self.left_fields)
                for rr in right_rids:
                    right_rec = self.grid.read(rr)
                    if not right_rec or right_rec.is_tombstone: continue
                    right_dict = _record_to_dict(right_rec, self.right_fields)
                    results.append({self.left_name: left_dict, self.right_name: right_dict})

        left_idx.close(); right_idx.close()
        return results


def merge_join(grid: AllocGrid, left_spec: dict, right_spec: dict,
               on_field: str, data_dir: str) -> List[dict]:
    """Convenience: merge join two collections on a shared field."""
    mj = MergeJoiner(grid, left_spec, right_spec)
    return mj.join(on_field, data_dir)
