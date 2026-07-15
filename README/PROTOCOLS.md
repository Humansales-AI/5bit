# 5bit — Protocol Unification: HTTP, Auth, and the Byte-Shell Boundary

Companion to LEXICON2, PACKING, OWNERSHIP, INTERPRETER, and HOST. This
document specifies where the line falls between the host (C) and the fabric
(5bit) for network protocols: the irreducible byte-shell C must own, and the
routing/auth/handler logic the fabric owns natively. It generalizes from HTTP
to TCP, WebRTC signaling, and SMTP, and specifies how external formats (JSON)
cross the boundary into records.

Status legend follows LEXICON2. [VERIFIED] = confirmed by the gauntlet
(`test_griddb_http.py`, 5/5). [DESIGN] = intended behavior. [NOTE] = caveat.

---

## 1. The universal boundary

Every network protocol splits the same way:

```
  EFFECT (host, C)          |  COMPUTATION (fabric, 5bit)
  ─────────────────────────────────────────────────────────
  accept / recv / send      |  routing (branch on request slots)
  raw byte framing          |  auth (grant check)
  format parse -> fields    |  handlers (read/store under grants)
  fields -> a REQUEST RECORD |  response assembled as a record
  record -> bytes on wire   |
```

The rule, stated once for all protocols: **the host does syscalls and
byte↔field plumbing; the fabric does every decision.** Writing routing or
auth in the host language rebuilds the exact seam the fabric exists to
delete. The host shell is small and closed (a parser + a socket loop); the
logic above it is tokens. [DESIGN; VERIFIED for HTTP]

---

## 2. HTTP [VERIFIED]

### 2.1 The C shell (irreducible, ~100 lines)

Two functions, zero logic:

- `parse_to_record(raw, user)` — request line → integer fields on request
  slots. `POST /deals 7500` → method=2, path=10, body=7500, user=<from
  token>. Method and path are enumerated by a trie in C (`"POST"`→2,
  `"/deals"`→10) — a closed, trivial mapping.
- `serialize_response()` — read the status slot the handler wrote, emit the
  HTTP status line.

Request slots (reserved): METHOD, PATH, BODY, USER, STATUS. The shell writes
them; the fabric reads them. [VERIFIED]

### 2.2 Routing is a 5bit program [VERIFIED]

Dispatch on PATH then METHOD via nested three-way IF, CALL the matching
handler:

```
READ PATH  10 -  IF ( 404 ) ( READ METHOD 2 - IF (list)(create)(list) ) ( 404 )
```

No routing table in C; the branch structure is the router. [VERIFIED — X1,X2,X3]

### 2.3 Auth is a grant, not a check [VERIFIED]

The decisive result: a handler that writes (POST /deals → STORE) is gated by
GRANT_W on the target slot. An unauthenticated caller — one who does not hold
the grant — triggers `EncodeRefused` at the STORE. There is no `if
(user.canWrite)`; permission is a property of the store, enforced by the
ownership layer. **You cannot forget the auth check, because it is not code.**
[VERIFIED — X4]

Identity flow: the C shell resolves a token/cookie to a holder id (via the
HASH capability for token verification, HOST §3) and writes it to the USER
slot; the fabric's grants do the rest. Session records, owner-at-position-0,
and the existing RLS daemon compose here unchanged (OWNERSHIP §3 note).

---

## 3. Generalizing to TCP, WebRTC signaling, SMTP

The boundary is identical; only the shell's parser changes. [DESIGN]

### 3.1 Raw TCP
The shell's socket loop is already TCP. A length-prefixed frame carrying
**packed 5bit bytes** needs no parse at all — it IS a record on arrival
(PACKING §4 zero-copy). This is the cleanest case: wire format = storage
format, the shell just verifies length and hands the bytes to the grid.
Framing (length + pad byte + packed region) is the whole protocol.

### 3.2 WebRTC — SIGNALING ONLY
Ruling stands from prior design: the **media plane is forbidden** (codecs,
jitter buffers, congestion control are anti-deterministic by design — not a
fabric job). The **signaling plane is native**: SDP offers, ICE candidates,
session tokens, and call state are structured records. The shell terminates
the WebRTC data channel / signaling socket (C, using an existing WebRTC lib);
each signaling message becomes a record; "who may join this session," "is
this offer valid," and booking state are grant-gated 5bit programs. Call
setup is replayable from the WAL byte-for-byte — a property normal signaling
stacks lack. RTP carries the pixels; 5bit carries the truth about the session.
[DESIGN]

### 3.3 SMTP
SMTP is a line protocol (HELO/MAIL FROM/RCPT TO/DATA) — structurally close to
HTTP. The C shell speaks the socket dialogue and parses envelope lines into
fields; an email becomes a record (from-slot, to-slot, subject-slot,
body-slot). Delivery/relay is an **effect** (a capability, like EMIT_OUT to a
relay); acceptance rules, spam/permission gating, and mailbox writes are
native grant-checked programs. The relay on Railway remains the effect
endpoint; the decisioning moves into the fabric. [DESIGN]

---

## 4. External formats: how JSON gets written into the DB

The boundary principle applies to formats exactly as to protocols: **parsing
is the shell's job; the parsed result is a record; the fabric never sees
JSON.** [DESIGN — the existing server already does JSON↔tokens at this seam]

### 4.1 Ingress: JSON → record
```
{"title":"Acme deal","value":7500,"stage":1}
        │  (C/host JSON parser — a closed grammar)
        ▼
  LABEL 0 "title" WORD("Acme deal")
  LABEL 1 "value" NUM(7500)
  LABEL 2 "stage" NUM(1)
  RECORD
```
Each JSON key → a LABEL (self-describing schema, LEXICON2 §7 interleaved);
each value → its native encoding (string→WORD, number→NUM with scale for
decimals, LEXICON2 §12). The record is now format-free: it carries its own
field names, so no external schema is needed to read it back.

### 4.2 Egress: record → JSON
`reconstruct_by_labels` (LEXICON2 §7) already rebuilds `{name: value}` from an
interleaved record with no external catalog. The host serializes that map to
JSON for a REST response. Round-trip is lossless (leading zeros, mixed
values, decimals via scale markers all survive).

### 4.3 Why this is not "yet another serializer"
JSON is a *transport skin* at the edge; it never enters storage, compute, or
the wire between fabric nodes (those stay packed 5bit). The seam exists only
where the fabric meets a foreign format, and it is one function each way.
Internally there is no serialization boundary at all — the record is the same
bytes on disk, in compute, and on the 5bit wire (PACKING §4).

---

## 5. So: do we now have REST APIs?

Yes — a REST endpoint is fully expressible: [VERIFIED for the core path]

```
  HTTP request bytes
    → C shell: parse line + JSON body → request record
    → 5bit router: branch on method/path → CALL handler
    → 5bit handler: grant-checked READ/STORE (auth = ownership)
    → response record → reconstruct_by_labels → JSON
    → C shell: JSON + status → response bytes
```

The REST semantics (verbs → handlers, paths → routes, 401/403 → grant
refusals, 404 → routing miss) all live in the fabric. The host is the socket
and the two format parsers. A CRUD API is: GET→read program, POST→store
program, DELETE→tombstone program, each one native and grant-gated.

[NOTE] What still lives in the host today and is worth porting incrementally:
the JSON parser and the HTTP line parser are closed grammars that *could*
become 5bit programs (the fabric can parse), but there is no correctness or
seam benefit to doing so — parsing foreign formats is legitimately edge work.
Keep them in C; keep decisions in tokens.

---

## 6. Gauntlet summary [VERIFIED — 5/5]

| # | Property |
|---|---|
| X1 | POST /deals (authed) routed natively to create-handler; deal stored; 200 |
| X2 | GET /deals routed to list-handler; 200 |
| X3 | Unknown path routed to not-found by the router's three-way IF; 404 |
| X4 | **Auth = grant**: unauthenticated POST refused at the handler's STORE |
| X5 | End-to-end: request bytes → native route+auth+store → response bytes |

---

## 7. Quick reference

```
Boundary: host = syscalls + byte↔field parse; fabric = routing/auth/handlers
Request:  a record (method/path/body/user slots) written by the shell
Routing:  three-way IF / CALL over request slots — no table in the host
Auth:     GRANT_W/GRANT_R — permission is a property of the store, not an if
JSON:     parsed at the edge -> LABEL+value record; never enters storage/wire
TCP:      length-prefixed packed 5bit = a record on arrival (zero parse)
WebRTC:   signaling native (records, grants, WAL-replayable); media plane C
SMTP:     envelope -> record; relay is an effect; acceptance rules native
REST:     verbs->handlers, paths->routes, 401/403->refusals, 404->routing miss
```
