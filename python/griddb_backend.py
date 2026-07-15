#!/usr/bin/env python3
"""
Self-Hosting, Full Verb Set — 5bit -> x86-64 (control flow, memory, grants)
===========================================================================
Compiles the full executable subset to native x86-64: arithmetic, three-way
IF (real branches), LOOP/BREAK (jumps), READ/STORE (memory arena),
GRANT-CHECKED STORE (ownership guard in machine code), CALL/RET, EMIT.

Registers (System V; arena ptr in RDI at entry):
    R15 = arena base   R14 = unwind rsp   R13 = value-stack ptr (dedicated,
    separate from call-stack RSP so CALL/RET never collide with values).
Arena: slots[NSLOT] | grant[NSLOT] | out[NOUT] | out_count | holder | vstack.
REFUSED sentinel = INT64_MIN.
"""
import ctypes, mmap, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from binary_grid_db import Token

REFUSED = -(1 << 63)
NSLOT, NOUT, VSTACK_N = 4096, 256, 1024
OFF_SLOTS  = 0
OFF_GRANT  = NSLOT * 8
OFF_OUT    = OFF_GRANT + NSLOT * 8
OFF_OUTCNT = OFF_OUT + NOUT * 8
OFF_HOLDER = OFF_OUTCNT + 8
OFF_VSTACK = OFF_HOLDER + 8
OFF_VTOP   = OFF_VSTACK + VSTACK_N * 8
ARENA_SZ   = OFF_VTOP

def imm64(v): return (v & 0xFFFFFFFFFFFFFFFF).to_bytes(8, 'little')
def imm32(v): return (v & 0xFFFFFFFF).to_bytes(4, 'little')
JG, JE, JNE = 0x8F, 0x84, 0x85


class Asm:
    def __init__(self): self.b = bytearray(); self.fixups = []; self.labels = {}
    def emit(self, *bs):
        for x in bs: self.b += x if isinstance(x, (bytes, bytearray)) else bytes([x])
    def label(self, n): self.labels[n] = len(self.b)
    def jmp(self, l): self.emit(b'\xE9'); self.fixups.append((len(self.b), l)); self.emit(imm32(0))
    def jcc(self, cc, l): self.emit(b'\x0F', bytes([cc])); self.fixups.append((len(self.b), l)); self.emit(imm32(0))
    def call(self, l): self.emit(b'\xE8'); self.fixups.append((len(self.b), l)); self.emit(imm32(0))
    def vpush(self): self.emit(b'\x49\x83\xED\x08', b'\x49\x89\x45\x00')
    def vpop_rax(self): self.emit(b'\x49\x8B\x45\x00', b'\x49\x83\xC5\x08')
    def vpop_rbx(self): self.emit(b'\x49\x8B\x5D\x00', b'\x49\x83\xC5\x08')
    def resolve(self):
        for pos, l in self.fixups: self.b[pos:pos+4] = imm32(self.labels[l] - (pos + 4))


class Compiler:
    def __init__(self, programs): self.progs = programs; self.a = Asm(); self._loop = []; self._n = 0
    def set_entry(self, s): self._entry = s
    def _lbl(self): self._n += 1; return f"L{self._n}"

    @staticmethod
    def _read_int(toks, i):
        v, neg = 0, False
        while i < len(toks):
            t = toks[i]
            if 0 <= t <= 9: v = v*10+t; i += 1
            elif 17 <= t <= 25: v = v*10+(t-16); neg = True; i += 1
            elif t == 30: i += 1; break
            else: break
        return (-v if neg else v), i

    @staticmethod
    def _skip_header(toks):
        i = 9
        while i < len(toks) and toks[i] != 30: i += 1
        return i + 1

    def _match(self, toks, o):
        d = 0
        for j in range(o, len(toks)):
            if toks[j] == 15: d += 1
            elif toks[j] == 16:
                d -= 1
                if d == 0: return j
        raise ValueError("unbalanced region")

    def compile_all(self):
        a = self.a
        a.emit(b'\x41\x57', b'\x41\x56', b'\x41\x55')
        a.emit(b'\x49\x89\xFF'); a.emit(b'\x49\x89\xE6')
        a.emit(b'\x4D\x8D\xAF', imm32(OFF_VTOP))
        a.call(f"P{self._entry}")
        a.label('__epi')
        a.emit(b'\x41\x5D', b'\x41\x5E', b'\x41\x5F'); a.emit(b'\xC3')
        for slot, toks in self.progs.items():
            a.label(f"P{slot}")
            self._body(toks, self._skip_header(toks), len(toks))
            a.emit(b'\xC3')
        a.resolve()
        return bytes(a.b)

    def _body(self, toks, i, hi):
        a = self.a
        while i < hi:
            t = toks[i]
            if t == 31 and toks[i:i+4] == [31,31,31,31]:
                cmd = toks[i+4]; i += 9
                arg = None
                if cmd in (6,7,12,13): arg, i = self._read_int(toks, i)
                i = self._verb(cmd, arg, toks, i, hi); continue
            if (0 <= t <= 9) or (17 <= t <= 25):
                v, i = self._read_int(toks, i); a.emit(b'\x48\xB8', imm64(v)); a.vpush(); continue
            if t == 10: a.vpop_rbx(); a.vpop_rax(); a.emit(b'\x48\x01\xD8'); a.vpush(); i += 1; continue
            if t == 11: a.vpop_rbx(); a.vpop_rax(); a.emit(b'\x48\x29\xD8'); a.vpush(); i += 1; continue
            if t == 12: a.vpop_rbx(); a.vpop_rax(); a.emit(b'\x48\x0F\xAF\xC3'); a.vpush(); i += 1; continue
            if t == 14:
                a.vpop_rax()
                a.emit(b'\x49\x8B\x8F', imm32(OFF_OUTCNT))
                a.emit(b'\x49\x89\x84\xCF', imm32(OFF_OUT))
                a.emit(b'\x48\xFF\xC1')
                a.emit(b'\x49\x89\x8F', imm32(OFF_OUTCNT))
                i += 1; continue
            if t == 15:
                c = self._match(toks, i); self._body(toks, i+1, c); i = c+1; continue
            if t in (30, 29): i += 1; continue
            if t == 28: return i
            i += 1
        return i

    def _verb(self, cmd, arg, toks, i, hi):
        a = self.a
        if cmd == 6: return i
        if cmd == 8: a.emit(b'\xC3'); return i
        if cmd == 7: a.call(f"P{arg}"); return i
        if cmd == 13:
            a.emit(b'\x49\x8B\x87', imm32(OFF_SLOTS + arg*8)); a.vpush(); return i
        if cmd == 12:
            trap, done = self._lbl(), self._lbl()
            a.emit(b'\x49\x8B\x87', imm32(OFF_GRANT + arg*8))
            a.emit(b'\x49\x8B\x8F', imm32(OFF_HOLDER))
            a.emit(b'\x48\xFF\xC1')
            a.emit(b'\x48\x39\xC8')
            a.jcc(JNE, trap)
            a.vpop_rax()
            a.emit(b'\x49\x89\x87', imm32(OFF_SLOTS + arg*8))
            a.jmp(done)
            a.label(trap)
            a.emit(b'\x4C\x89\xF4')
            a.emit(b'\x4D\x8D\xAF', imm32(OFF_VTOP))
            a.emit(b'\x48\xB8', imm64(REFUSED))
            a.jmp('__epi')
            a.label(done)
            return i
        if cmd == 9:
            done = self._lbl(); ap, az, an = self._lbl(), self._lbl(), self._lbl()
            regions = []; p = i
            while len(regions) < 3 and p < hi and toks[p] == 15:
                c = self._match(toks, p); regions.append((p+1, c)); p = c+1
            a.vpop_rax()
            a.emit(b'\x48\x83\xF8\x00')
            a.jcc(JG, ap); a.jcc(JE, az); a.jmp(an)
            for lbl, idx in ((ap,0),(az,1),(an,2)):
                a.label(lbl)
                if idx < len(regions): self._body(toks, regions[idx][0], regions[idx][1])
                a.jmp(done)
            a.label(done)
            return p
        if cmd == 10:
            c = self._match(toks, i); top, brk = self._lbl(), self._lbl()
            self._loop.append((top, brk))
            a.label(top); self._body(toks, i+1, c); a.jmp(top); a.label(brk)
            self._loop.pop(); return c+1
        if cmd == 11:
            if self._loop: a.jmp(self._loop[-1][1])
            return i
        return i


_KEEP = []
def run(programs, entry_slot, grants=None, slot_init=None, holder=0):
    c = Compiler(programs); c.set_entry(entry_slot); code = c.compile_all()
    buf = mmap.mmap(-1, max(len(code), 64),
                    prot=mmap.PROT_READ | mmap.PROT_WRITE | mmap.PROT_EXEC)
    buf.write(code); _KEEP.append(buf)
    ptr = ctypes.cast((ctypes.c_char*len(code)).from_buffer(buf), ctypes.c_void_p)
    arena = (ctypes.c_int64 * (ARENA_SZ // 8))()
    for s, v in (slot_init or {}).items(): arena[(OFF_SLOTS//8)+s] = v
    for s, h in (grants or {}).items(): arena[(OFF_GRANT//8)+s] = h + 1
    arena[OFF_HOLDER//8] = holder
    fn = ctypes.CFUNCTYPE(ctypes.c_int64, ctypes.c_void_p)(ptr.value)
    ret = fn(ctypes.addressof(arena))
    outn = arena[OFF_OUTCNT//8]
    out = [arena[(OFF_OUT//8)+k] for k in range(outn)]
    slots = {s: arena[(OFF_SLOTS//8)+s] for s in (slot_init or {})}
    return {'ret': ret, 'out': out, 'slots': slots,
            'refused': ret == REFUSED, 'code_len': len(code)}
