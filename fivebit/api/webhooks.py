"""
5bit Webhooks — WAL Change Stream → HTTP POST
===============================================
Taps the existing WAL change stream. Fires webhooks on insert/update/delete.
Retry with exponential backoff. Dead-letter after max retries. Grid-durable.

POST /api/webhooks  { url, table, events }  → { id, secret }
GET  /api/webhooks                          → list configured webhooks
DELETE /api/webhooks/{id}                    → remove
"""
import os, sys, json, time, hashlib, hmac, threading, urllib.request
from collections import defaultdict
from typing import List, Dict, Optional

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'python'))
from binary_grid_db import Token, Encoder, Parser, ParsedNumber, ParsedWord
from griddb_alloc import AllocGrid

WEBHOOK_BASE = 70_000_000
DELIVERY_BASE = 71_000_000
MAX_RETRIES = 5
BACKOFF = [1, 2, 4, 8, 16]


class WebhookManager:
    """WAL-tail → HTTP POST. Config + deliveries stored in grid."""

    def __init__(self, grid: AllocGrid):
        self.grid = grid
        self._running = False
        self._thread: Optional[threading.Thread] = None

    # ── CRUD ──────────────────────────────────────────────────────────

    def create(self, url: str, table: str, events: List[str]) -> dict:
        """Register a webhook. Returns { id, secret }."""
        rid = self._next_id(WEBHOOK_BASE)
        secret = hashlib.sha256(os.urandom(32)).hexdigest()[:32]
        tokens = [
            *Encoder.encode_word(url),
            *Encoder.encode_word(table),
            *Encoder.encode_word(','.join(events)),
            *Encoder.encode_word(secret),
            Token.RECORD,
        ]
        self.grid.write(rid, tokens)
        return {'id': rid, 'secret': secret, 'url': url, 'table': table, 'events': events}

    def list(self) -> List[dict]:
        """List all configured webhooks."""
        hooks = []
        for rid in range(WEBHOOK_BASE, WEBHOOK_BASE + 1000):
            rec = self.grid.read(rid)
            if not rec or rec.is_tombstone: continue
            words = [p.text for p in rec.parsed if isinstance(p, ParsedWord)]
            if len(words) >= 4:
                hooks.append({
                    'id': rid, 'url': words[0], 'table': words[1],
                    'events': words[2].split(','), 'secret': words[3],
                })
        return hooks

    def delete(self, hook_id: int) -> bool:
        rec = self.grid.read(hook_id)
        if not rec: return False
        self.grid.delete(hook_id)
        return True

    # ── Delivery ───────────────────────────────────────────────────────

    def on_change(self, table: str, event_type: str, record: dict):
        """Called by WAL tail when a record changes."""
        hooks = self.list()
        for hook in hooks:
            if hook['table'] != table: continue
            if event_type not in hook['events']: continue
            self._deliver(hook, event_type, record)

    def _deliver(self, hook: dict, event_type: str, record: dict):
        """Fire a webhook delivery with retry."""
        payload = json.dumps({
            'table': hook['table'],
            'event': event_type,
            'record': record,
            'timestamp': int(time.time()),
        }).encode()

        signature = hmac.new(
            hook['secret'].encode(), payload, 'sha256'
        ).hexdigest()

        headers = {
            'Content-Type': 'application/json',
            'X-Fivebit-Signature': f'sha256={signature}',
            'X-Fivebit-Event': event_type,
            'X-Fivebit-Table': hook['table'],
        }

        # Attempt delivery with backoff
        for attempt in range(MAX_RETRIES):
            try:
                req = urllib.request.Request(hook['url'], data=payload, headers=headers, method='POST')
                resp = urllib.request.urlopen(req, timeout=10)
                if 200 <= resp.status < 300:
                    return  # Success
            except Exception:
                pass
            time.sleep(BACKOFF[attempt])

        # Dead-letter: store failed delivery in grid
        dlq_rid = self._next_id(DELIVERY_BASE)
        dlq_tokens = [
            *Encoder.encode_word(hook['url']),
            *Encoder.encode_integer(hook['id']),
            *Encoder.encode_word(event_type),
            *Encoder.encode_word(payload.decode()[:500]),
            Token.RECORD,
        ]
        self.grid.write(dlq_rid, dlq_tokens)

    def dead_letter_queue(self) -> List[dict]:
        """List failed deliveries."""
        dlq = []
        for rid in range(DELIVERY_BASE, DELIVERY_BASE + 1000):
            rec = self.grid.read(rid)
            if not rec or rec.is_tombstone: continue
            words = [p.text for p in rec.parsed if isinstance(p, ParsedWord)]
            nums = [p.value for p in rec.parsed if isinstance(p, ParsedNumber)]
            if len(words) >= 3:
                dlq.append({'id': rid, 'url': words[0], 'hook_id': nums[0] if nums else 0,
                            'event': words[2], 'payload': words[3][:100]})
        return dlq

    def retry_dead_letter(self):
        """Retry all dead-lettered deliveries."""
        for item in self.dead_letter_queue():
            hooks = self.list()
            hook = next((h for h in hooks if h['id'] == item['hook_id']), None)
            if hook:
                self._deliver(hook, item['event'], {'_retry': True})
            self.grid.delete(item['id'])

    def _next_id(self, base: int) -> int:
        rid = base
        while self.grid.read(rid): rid += 1
        return rid

    def start(self):
        """Background retry loop for dead-letter queue."""
        self._running = True
        def _loop():
            while self._running:
                time.sleep(30)
                self.retry_dead_letter()
        self._thread = threading.Thread(target=_loop, daemon=True)
        self._thread.start()

    def stop(self):
        self._running = False
