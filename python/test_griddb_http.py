#!/usr/bin/env python3
"""test_griddb_http.py — HTTP unified. C moves bytes; the fabric does the rest.

THE RULE: the C shell (HttpShell) only parses request line -> integer fields
and serializes a status back. It contains ZERO routing and ZERO auth logic.
Every routing decision and every permission check below is 5bit tokens.

X1  POST /deals with auth: routed natively to create-handler, deal stored,
    200 emitted. C never routed.
X2  GET /deals: routed to list-handler, 200.
X3  Unknown path /widgets: routed to not-found, 404 — by tokens.
X4  AUTH = GRANT: an unauthenticated user (no GRANT_W on the deal store)
    hits POST /deals -> the handler's STORE fires EncodeRefused. No 200,
    no deal written. Permission enforced by ownership, not by an if-check.
X5  End-to-end bytes: raw request bytes in -> HTTP response bytes out, the
    whole middle native.
"""
import os, sys, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from binary_grid_db import Token, Encoder
from griddb_alloc import AllocGrid
from griddb_ownership import OwnershipEncoder, EncodeRefused
from griddb_interp import Machine
from griddb_http import (
    HttpShell, build_router, build_handlers, _first_int,
    ROUTER, SLOT_STATUS, SLOT_DEAL_STORE,
    SLOT_METHOD, SLOT_PATH, SLOT_BODY, SLOT_USER,
    H_CREATE_DEAL, H_LIST_DEALS, H_NOT_FOUND,
)

PASS = 0
def ok(name):
    global PASS; PASS += 1; print(f"  PASS  {name}")

REQ_SLOTS = [SLOT_METHOD, SLOT_PATH, SLOT_BODY, SLOT_USER, SLOT_STATUS]


def server(authed=True):
    """Wire a server. `authed` decides whether the caller holds GRANT_W on
    the deal store — i.e. whether they're allowed to create deals."""
    tmp = '/tmp/http_grid'; shutil.rmtree(tmp, ignore_errors=True)
    grid = AllocGrid(data_dir=tmp)
    own = OwnershipEncoder()
    for s in REQ_SLOTS:
        own.grant_w(slot=s, holder=0)             # shell owns request slots
    if authed:
        own.grant_w(slot=SLOT_DEAL_STORE, holder=0)   # caller may create deals
    m = Machine(ownership=own, grid=grid, holder=0, max_steps=200000)
    m.load(ROUTER, build_router())
    for slot, toks in build_handlers().items():
        m.load(slot, toks)
    shell = HttpShell(own, grid, holder=0)
    return own, grid, m, shell


def handle(shell, m, raw: bytes, user=0) -> bytes:
    shell.parse_to_record(raw, user)      # C: bytes -> request record
    m.run(ROUTER)                         # 5bit: route + handle
    return shell.serialize_response()     # C: status record -> bytes


print("X1  POST /deals, authed -> native route -> create -> 200")
own, grid, m, shell = server(authed=True)
resp = handle(shell, m, b'POST /deals 7500')
assert resp == b'HTTP/1.1 200 OK', resp
assert _first_int(grid.read(SLOT_DEAL_STORE).tokens) == 7500
ok(f"routed to create-handler by tokens; deal 7500 stored; resp={resp.decode()}")

print("X2  GET /deals -> list -> 200")
resp = handle(shell, m, b'GET /deals 0')
assert resp == b'HTTP/1.1 200 OK', resp
ok(f"routed to list-handler; resp={resp.decode()}")

print("X3  unknown path -> 404 by tokens")
resp = handle(shell, m, b'GET /widgets 0')
assert resp == b'HTTP/1.1 404 Not Found', resp
ok(f"router's three-way IF hit not-found; resp={resp.decode()}")

print("X4  AUTH = GRANT: unauthenticated create refused")
own2, grid2, m2, shell2 = server(authed=False)   # NO grant on deal store
try:
    handle(shell2, m2, b'POST /deals 9999')
    raise AssertionError("unauthenticated create succeeded!")
except EncodeRefused as e:
    # no deal written, status never set to 200
    deal = grid2.read(SLOT_DEAL_STORE)
    assert deal is None or _first_int(deal.tokens) != 9999
    ok(f"POST refused at the handler's STORE — auth enforced by ownership  [{e}]")

print("X5  end-to-end bytes, native middle")
own3, grid3, m3, shell3 = server(authed=True)
raw_in = b'POST /deals 5000'
raw_out = handle(shell3, m3, raw_in)
assert raw_out == b'HTTP/1.1 200 OK'
assert _first_int(grid3.read(SLOT_DEAL_STORE).tokens) == 5000
ok(f"{raw_in.decode()!r} -> [fabric routes+auths+stores] -> {raw_out.decode()!r}")

print(f"\nALL {PASS} CHECKS PASS — C moved bytes; 5bit routed, authed, and handled.")
