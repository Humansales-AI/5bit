"""
5bit Realtime — WebSocket + Presence + Channels
=================================================
Bidirectional real-time on top of the WAL change stream.

Capabilities:
  - Database change subscription (table-level)
  - Broadcast channels (publish/subscribe)
  - Presence tracking ("who's online" with TTL heartbeat)

Architecture:
  WAL tail → parse events → route to subscribers by table
  Presence: heartbeat writes to grid record, stale entries auto-tombstone
  Channels: in-memory pub/sub with grid-backed message log (optional)

python3 -m fivebit.api.realtime --port 8081
"""
import os, sys, json, time, hashlib, struct, threading, asyncio
from typing import Dict, Set, Optional
from dataclasses import dataclass, field

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'python'))
from binary_grid_db import Token, Encoder, unpack_from_bytes

try:
    import websockets
except ImportError:
    websockets = None

PRESENCE_BASE = 90_000_000
PRESENCE_TTL = 30  # seconds


@dataclass
class Presence:
    userId: int; name: str; lastSeen: float; channel: str = ''


class RealtimeServer:
    """WebSocket real-time server with presence + channels."""

    def __init__(self, data_dir: str = "./data", port: int = 8081):
        self.port = port
        self.data_dir = data_dir
        self.subscribers: Dict[str, Set] = {}     # table → set of ws connections
        self.channels: Dict[str, Set] = {}         # channel → set of ws
        self.presence: Dict[int, Presence] = {}    # userId → Presence
        self._running = False
        self._lock = threading.Lock()

    # ── Database change subscription ────────────────────────────────────

    def subscribe_table(self, table: str, ws):
        with self._lock:
            if table not in self.subscribers:
                self.subscribers[table] = set()
            self.subscribers[table].add(ws)

    def unsubscribe_table(self, table: str, ws):
        with self._lock:
            if table in self.subscribers:
                self.subscribers[table].discard(ws)

    def broadcast_change(self, table: str, event: dict):
        with self._lock:
            subs = list(self.subscribers.get(table, set()))  # snapshot — safe to iterate
        dead = []
        for ws in subs:
            try:
                ws.send(json.dumps({'type': 'change', 'table': table, 'event': event}))
            except Exception:
                dead.append(ws)
        for ws in dead:
            with self._lock:
                self.subscribers.get(table, set()).discard(ws)

    # ── Channels ────────────────────────────────────────────────────────

    def subscribe_channel(self, channel: str, ws):
        with self._lock:
            if channel not in self.channels:
                self.channels[channel] = set()
            self.channels[channel].add(ws)

    def publish(self, channel: str, message: dict, sender_ws=None):
        with self._lock:
            subs = list(self.channels.get(channel, set()))  # snapshot
        dead = []
        for ws in subs:
            if ws is sender_ws: continue
            try:
                ws.send(json.dumps({'type': 'broadcast', 'channel': channel, 'payload': message}))
            except Exception:
                dead.append(ws)
        for ws in dead:
            with self._lock:
                self.channels.get(channel, set()).discard(ws)

    # ── Presence ────────────────────────────────────────────────────────

    def update_presence(self, userId: int, name: str, channel: str = ''):
        with self._lock:
            self.presence[userId] = Presence(userId, name, time.time(), channel)

        # Broadcast presence update to channel subscribers
        presence_list = self.get_presence(channel)
        self.publish(f'__presence:{channel}', {
            'type': 'presence',
            'users': [{'userId': p.userId, 'name': p.name} for p in presence_list],
        })

    def get_presence(self, channel: str = '') -> list:
        now = time.time()
        with self._lock:
            # Remove stale entries
            stale = [uid for uid, p in self.presence.items() if now - p.lastSeen > PRESENCE_TTL]
            for uid in stale: del self.presence[uid]
            if channel:
                return [p for p in self.presence.values() if p.channel == channel and now - p.lastSeen <= PRESENCE_TTL]
            return [p for p in self.presence.values() if now - p.lastSeen <= PRESENCE_TTL]

    # ── WebSocket handler ───────────────────────────────────────────────

    async def _handler(self, ws, path):
        """Handle one WebSocket connection."""
        subscribed_tables = set()
        subscribed_channels = set()

        try:
            async for message in ws:
                data = json.loads(message)
                msg_type = data.get('type', '')

                if msg_type == 'subscribe':
                    table = data.get('table', '')
                    if table:
                        self.subscribe_table(table, ws)
                        subscribed_tables.add(table)
                        await ws.send(json.dumps({'type': 'subscribed', 'table': table}))

                elif msg_type == 'channel':
                    ch = data.get('channel', '')
                    if ch:
                        self.subscribe_channel(ch, ws)
                        subscribed_channels.add(ch)
                        await ws.send(json.dumps({'type': 'joined', 'channel': ch}))

                elif msg_type == 'broadcast':
                    ch = data.get('channel', '')
                    payload = data.get('payload', {})
                    self.publish(ch, payload, ws)

                elif msg_type == 'presence':
                    uid = data.get('userId', 0)
                    name = data.get('name', '')
                    ch = data.get('channel', '')
                    self.update_presence(uid, name, ch)
                    presence = self.get_presence(ch)
                    await ws.send(json.dumps({
                        'type': 'presence',
                        'channel': ch,
                        'users': [{'userId': p.userId, 'name': p.name} for p in presence],
                    }))

        except Exception:
            pass
        finally:
            # Cleanup
            for table in subscribed_tables:
                self.unsubscribe_table(table, ws)
            for ch in subscribed_channels:
                with self._lock:
                    self.channels.get(ch, set()).discard(ws)

    async def start_ws(self):
        """Start WebSocket server."""
        if websockets is None:
            print("[realtime] websockets not installed — pip install websockets")
            return
        print(f"[realtime] ws://0.0.0.0:{self.port}")
        async with websockets.serve(self._handler, '0.0.0.0', self.port):
            await asyncio.Future()  # run forever

    def start(self):
        """Start in background thread."""
        if websockets is None:
            print("[realtime] pip install websockets")
            return
        self._running = True

        def _run():
            asyncio.run(self.start_ws())

        t = threading.Thread(target=_run, daemon=True)
        t.start()


# ── Standalone ──────────────────────────────────────────────────────────

if __name__ == '__main__':
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument('--port', type=int, default=8081)
    p.add_argument('--data', default='./data')
    args = p.parse_args()

    server = RealtimeServer(data_dir=args.data, port=args.port)
    print(f"[5bit Realtime] Presence + Channels + Change Streams")
    print(f"[5bit Realtime] ws://localhost:{args.port}")
    asyncio.run(server.start_ws())
