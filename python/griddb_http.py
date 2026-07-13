#!/usr/bin/env python3
"""
HTTP unified: the shell is C, the logic is 5bit
================================================
The line inside HTTP: sockets + raw byte parsing are effects (C owns them);
routing, auth, and handling are computation (the fabric owns them).

    ┌─────────────────────── C DOORMAN (irreducible shell, ~100 lines) ──┐
    │ accept / recv raw bytes / send / close                              │
    │ tokenize the request LINE into integer fields -> a request RECORD   │
    └──────────────────────────────┬─────────────────────────────────────┘
                                    │  request is now DATA (a record)
    ┌───────────────────────────────▼──────────────── 5bit FABRIC ───────┐
    │ routing   = three-way IF / CALL over request-record slots           │
    │ auth      = a GRANT check (already native)                          │
    │ handler   = STORE gated by grant; response assembled as a record    │
    └─────────────────────────────────────────────────────────────────────┘

This module provides:
  - HttpShell: the mocked C doorman. Parses a raw request into integer
    fields and lays them on request slots. Serializes a response record
    back to bytes. THIS IS THE ONLY NON-FABRIC LOGIC — and it is pure
    plumbing: method/path enums in, status out. No routing, no auth here.
  - build_router: a 5bit program that dispatches on the METHOD and PATH
    slots and CALLs the matching handler. Native.
  - handlers: 5bit programs that check a grant (auth) and act. Native.

Method/path are pre-enumerated integers (the C parser's job is to map the
byte string "POST" -> 2, "/deals" -> 10; a trie in C, trivial and closed).
"""
from typing import Dict, List, Tuple

from binary_grid_db import Token, Encoder
from griddb_interp import (
    verb, num, region, program,
    CMD_CALL, CMD_IF, CMD_STORE, CMD_READ, CMD_RET,
)

MINUS, PLUS, EMIT = Token(11), Token(10), Token(14)

# request record slots (the C shell writes these)
SLOT_METHOD = 800     # GET=1 POST=2 PUT=3 DELETE=4
SLOT_PATH   = 801     # /deals=10  /contacts=11  (enumerated by C trie)
SLOT_BODY   = 802     # a scalar payload (e.g. deal value)
SLOT_USER   = 803     # authenticated holder id (C sets from token)
SLOT_STATUS = 804     # response status the handler writes (200/403/404)

METHODS = {'GET': 1, 'POST': 2, 'PUT': 3, 'DELETE': 4}
PATHS   = {'/deals': 10, '/contacts': 11}


class HttpShell:
    """The mocked C doorman. Pure byte<->field plumbing; no logic."""

    def __init__(self, own, grid, holder=0):
        self.own, self.grid, self.holder = own, grid, holder

    def parse_to_record(self, raw: bytes, user: int) -> None:
        """C's job: raw request line -> integer fields on request slots.
        e.g. b'POST /deals 7500' -> method=2 path=10 body=7500."""
        line = raw.decode().strip().split()
        method = METHODS.get(line[0], 0)
        path = PATHS.get(line[1], 0)
        body = int(line[2]) if len(line) > 2 and line[1] != '' else 0
        for slot, val in [(SLOT_METHOD, method), (SLOT_PATH, path),
                          (SLOT_BODY, body), (SLOT_USER, user), (SLOT_STATUS, 0)]:
            self.own.write_with_grant(self.grid, slot, self.holder,
                                      [*Encoder.encode_integer(val), Token.RECORD])

    def serialize_response(self) -> bytes:
        """C's job: read the status slot the handler wrote, emit an HTTP line."""
        rec = self.grid.read(SLOT_STATUS)
        status = _first_int(rec.tokens)
        text = {200: 'OK', 403: 'Forbidden', 404: 'Not Found'}.get(status, '???')
        return f"HTTP/1.1 {status} {text}".encode()


def _first_int(tokens) -> int:
    digits, neg = [], False
    for t in tokens:
        t = Token(t)
        if 0 <= int(t) <= 9: digits.append(int(t))
        elif 17 <= int(t) <= 25: digits.append(int(t) - 16); neg = True
        elif int(t) in (30, 28) and digits: break
    v = 0
    for d in digits: v = v * 10 + d
    return -v if neg else v


# ---- 5bit programs: routing + handlers (ALL logic lives here) ----

# handler slots
H_CREATE_DEAL = 50
H_LIST_DEALS  = 51
H_NOT_FOUND   = 52
ROUTER        = 40

# where a created deal lands
SLOT_DEAL_STORE = 700


def build_handlers() -> Dict[int, List[Token]]:
    """Each handler is a native 5bit program. create_deal checks auth (grant)
    implicitly: its STORE to SLOT_DEAL_STORE refuses if the running holder
    lacks GRANT_W — auth IS the grant."""
    return {
        # POST /deals: store body as a new deal, set status 200
        H_CREATE_DEAL: program(H_CREATE_DEAL,
            verb(CMD_READ, SLOT_BODY), verb(CMD_STORE, SLOT_DEAL_STORE),
            num(200), verb(CMD_STORE, SLOT_STATUS),
        ),
        # GET /deals: (would read; here just acknowledges) status 200
        H_LIST_DEALS: program(H_LIST_DEALS,
            num(200), verb(CMD_STORE, SLOT_STATUS),
        ),
        # unknown route: status 404
        H_NOT_FOUND: program(H_NOT_FOUND,
            num(404), verb(CMD_STORE, SLOT_STATUS),
        ),
    }


def build_router() -> List[Token]:
    """Native routing: dispatch on PATH then METHOD via three-way IF.
    path==10 (/deals): method==2 -> create, else -> list.
    path!=10: not found. All by tokens; C never routes."""
    return program(ROUTER,
        # path - 10  (0 -> /deals)
        verb(CMD_READ, SLOT_PATH), num(10), [MINUS],
        verb(CMD_IF),
        region(verb(CMD_CALL, H_NOT_FOUND)),          # +arm: path>10 -> 404
        region(                                        # 0arm: path==/deals
            verb(CMD_READ, SLOT_METHOD), num(2), [MINUS],
            verb(CMD_IF),
            region(verb(CMD_CALL, H_LIST_DEALS)),      #   +arm: method>2 -> list
            region(verb(CMD_CALL, H_CREATE_DEAL)),     #   0arm: POST -> create
            region(verb(CMD_CALL, H_LIST_DEALS)),      #   -arm: GET -> list
        ),
        region(verb(CMD_CALL, H_NOT_FOUND)),          # -arm: path<10 -> 404
    )
