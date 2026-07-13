/**
 * 5bit Interpreter — tokens that DO instead of MEAN
 * ==================================================
 * The walker. Everything before this layer is nouns (records, grants, values);
 * this module adds the verbs. TypeScript port of griddb_interp.py.
 *
 * THE VERBS (SPECIAL3 free slots 6..13):
 *   DEF=6, CALL=7, RET=8, IF=9, LOOP=10, BREAK=11, STORE=12, READ=13
 *
 * REGIONS: LPAREN/RPAREN balanced-paren counting. Structure IS the address.
 * MEMORY: Variables are slots. The grid is the register file.
 * EFFECTS: STORE is gated by ownership — deny-by-default execution.
 */
import { Token } from './types';
import { Encoder } from './encoder';
import { packToBytes, unpackFromBytes } from './serialization';

// ── Verb tokens (SPECIAL3 slots 6-13) ──────────────────────────────────────
export const CMD_DEF   = Token.D6;
export const CMD_CALL  = Token.D7;
export const CMD_RET   = Token.D8;
export const CMD_IF    = Token.D9;
export const CMD_LOOP  = Token.T_PLUS;   // 0b01010 = 10
export const CMD_BREAK = Token.T_MINUS;  // 0b01011 = 11
export const CMD_STORE = Token.T_MUL;    // 0b01100 = 12
export const CMD_READ  = Token.T_DIV;    // 0b01101 = 13

const VERB_NAMES: Record<number, string> = {
  [CMD_DEF]: 'DEF', [CMD_CALL]: 'CALL', [CMD_RET]: 'RET',
  [CMD_IF]: 'IF', [CMD_LOOP]: 'LOOP', [CMD_BREAK]: 'BREAK',
  [CMD_STORE]: 'STORE', [CMD_READ]: 'READ',
};
const HAS_ARG = new Set([CMD_DEF, CMD_CALL, CMD_STORE, CMD_READ]);

const LPAREN = Token.T_LPAREN;
const RPAREN = Token.T_RPAREN;
const EMIT   = Token.T_EQ;

const OPS: Record<number, (a: number, b: number) => number> = {
  [Token.T_PLUS]:  (a, b) => a + b,
  [Token.T_MINUS]: (a, b) => a - b,
  [Token.T_MUL]:   (a, b) => a * b,
  [Token.T_DIV]:   (a, b) => Math.trunc(a / b),
};

const DIGITS: Record<number, number> = {};
for (let i = 0; i <= 9; i++) DIGITS[Token[`D${i}`] as unknown as number] = i;

const NEGS: Record<number, number> = {};
for (let i = 1; i <= 9; i++) NEGS[Token[`N${i}`] as unknown as number] = i;

// ── Control signals ─────────────────────────────────────────────────────────
class BreakSignal extends Error { constructor() { super('BREAK'); } }
class ReturnSignal extends Error { constructor() { super('RET'); } }

export class InterpreterError extends Error {}
export class OutOfGas extends InterpreterError {
  constructor(maxSteps: number) { super(`step budget ${maxSteps} exhausted`); }
}

// ── Program builder ─────────────────────────────────────────────────────────

/** Encode a verb in the encode_command shape: START×4 cmd END×4 [+ NUM arg]. */
export function verb(cmd: Token, arg?: number): Token[] {
  const toks: Token[] = [
    Token.START, Token.START, Token.START, Token.START,
    cmd,
    Token.END, Token.END, Token.END, Token.END,
  ];
  if (arg !== undefined) toks.push(...Encoder.encodeInteger(arg));
  return toks;
}

export function num(n: number): Token[] { return Encoder.encodeInteger(n); }

export function region(...parts: Token[][]): Token[] {
  const out: Token[] = [LPAREN];
  for (const p of parts) out.push(...p);
  out.push(RPAREN);
  return out;
}

/** DEF header + body + RECORD: a complete executable record. */
export function program(slot: number, ...parts: Token[][]): Token[] {
  return [...verb(CMD_DEF, slot), ...parts.flat(), Token.RECORD];
}

// ── The Machine ─────────────────────────────────────────────────────────────

export interface GridLike {
  read(rid: number): { tokens: Token[] } | null;
  write(rid: number, tokens: Token[]): number;
}

export class Machine {
  programs: Map<number, Token[]> = new Map();
  stack: number[] = [];
  output: number[] = [];
  trace: string[] = [];
  private _steps = 0;

  constructor(
    public grid: GridLike | null = null,
    public holder: number = 0,
    public maxSteps: number = 100_000,
  ) {}

  // ── Loading ──────────────────────────────────────────────────────────

  /** Load a record as a program. Refuses if no DEF header. */
  load(slot: number, tokens: Token[]): void {
    const head = verb(CMD_DEF, slot);
    for (let i = 0; i < head.length; i++) {
      if (tokens[i] !== head[i]) {
        throw new InterpreterError(
          `slot ${slot}: record has no DEF header — data is not executable`);
      }
    }
    this.programs.set(slot, tokens);
  }

  loadBytes(slot: number, data: Uint8Array, pad: number): void {
    this.load(slot, unpackFromBytes(data, pad));
  }

  // ── Running ──────────────────────────────────────────────────────────

  call(slot: number): void {
    const toks = this.programs.get(slot);
    if (!toks) throw new InterpreterError(`CALL ${slot}: no DEF'd program at slot`);
    this.trace.push(`CALL ${slot}`);
    try {
      this._exec(toks, 0, toks.length);
    } catch (e) {
      if (!(e instanceof ReturnSignal)) throw e;
    }
    this.trace.push(`RET  ${slot}`);
  }

  run(slot: number): number[] {
    this._steps = 0;
    this.call(slot);
    return this.output;
  }

  // ── Core walk ────────────────────────────────────────────────────────

  private _gas(): void {
    this._steps++;
    if (this._steps > this.maxSteps) throw new OutOfGas(this.maxSteps);
  }

  private _exec(toks: Token[], pos: number, end: number): number {
    while (pos < end) {
      this._gas();
      const t = toks[pos];

      // SPECIAL3 verb: START×4 cmd END×4 [arg]
      if (t === Token.START &&
          toks[pos] === Token.START && toks[pos+1] === Token.START &&
          toks[pos+2] === Token.START && toks[pos+3] === Token.START) {
        const cmd = toks[pos + 4];
        pos += 9; // 4 START + cmd + 4 END
        let arg: number | undefined;
        if (HAS_ARG.has(cmd as number)) {
          const [val, newPos] = this._readInt(toks, pos, end);
          arg = val; pos = newPos;
        }
        pos = this._dispatch(cmd, arg, toks, pos, end);
        continue;
      }

      if (t in DIGITS || t in NEGS) {
        const [val, newPos] = this._readInt(toks, pos, end);
        this.stack.push(val);
        pos = newPos;
        continue;
      }

      if (t in OPS) {
        const b = this.stack.pop()!;
        const a = this.stack.pop()!;
        this.stack.push(OPS[t as number]!(a, b));
        pos++;
        continue;
      }

      if (t === EMIT) {
        const v = this.stack.pop()!;
        this.output.push(v);
        this.trace.push(`EMIT ${v}`);
        pos++;
        continue;
      }

      if (t === LPAREN) {
        const close = this._match(toks, pos, end);
        this._exec(toks, pos + 1, close);
        pos = close + 1;
        continue;
      }

      if (t === Token.END) { pos++; continue; }

      if (t === Token.RECORD) throw new ReturnSignal();

      if (t === Token.CHECKSUM) { pos++; continue; }

      throw new InterpreterError(`unexpected token ${Token[t as number]} at ${pos}`);
    }
    return pos;
  }

  // ── Verb dispatch ────────────────────────────────────────────────────

  private _dispatch(cmd: Token, arg: number | undefined,
                    toks: Token[], pos: number, end: number): number {
    if (cmd === CMD_DEF) return pos;
    if (cmd === CMD_CALL) { this.call(arg!); return pos; }
    if (cmd === CMD_RET) throw new ReturnSignal();
    if (cmd === CMD_BREAK) throw new BreakSignal();
    if (cmd === CMD_IF) {
      const test = this.stack.pop()!;
      const thenOpen = this._expect(toks, pos, LPAREN, 'IF needs ( then-region )');
      const thenClose = this._match(toks, thenOpen, end);
      const after = thenClose + 1;
      const hasElse = after < end && toks[after] === LPAREN;
      const elseClose = hasElse ? this._match(toks, after, end) : -1;
      this.trace.push(`IF -> ${test ? 'then' : 'else'}`);
      if (test) this._exec(toks, thenOpen + 1, thenClose);
      else if (hasElse) this._exec(toks, after + 1, elseClose);
      return hasElse ? elseClose + 1 : after;
    }
    if (cmd === CMD_LOOP) {
      const open = this._expect(toks, pos, LPAREN, 'LOOP needs ( region )');
      const close = this._match(toks, open, end);
      while (true) {
        this._gas();
        try { this._exec(toks, open + 1, close); }
        catch (e) {
          if (e instanceof BreakSignal) { this.trace.push('BREAK'); break; }
          throw e;
        }
      }
      return close + 1;
    }
    if (cmd === CMD_STORE) {
      const v = this.stack.pop()!;
      if (!this.grid) throw new InterpreterError('STORE with no grid attached');
      this.grid.write(arg!, [...Encoder.encodeInteger(v), Token.RECORD]);
      this.trace.push(`STORE ${v} -> slot ${arg}`);
      return pos;
    }
    if (cmd === CMD_READ) {
      if (!this.grid) throw new InterpreterError('READ with no grid attached');
      const rec = this.grid.read(arg!);
      if (!rec) throw new InterpreterError(`READ slot ${arg!}: empty`);
      const vals = this._parseInts(rec.tokens);
      this.stack.push(vals[0]);
      this.trace.push(`READ slot ${arg!} -> ${vals[0]}`);
      return pos;
    }
    throw new InterpreterError(`unknown verb ${VERB_NAMES[cmd as number] || `CMD${cmd}`}`);
  }

  // ── Helpers ──────────────────────────────────────────────────────────

  private _readInt(toks: Token[], pos: number, end: number): [number, number] {
    const digits: number[] = [];
    let neg = false;
    while (pos < end) {
      const t = toks[pos];
      if (t in DIGITS) { digits.push(DIGITS[t as number]!); pos++; }
      else if (t in NEGS) { digits.push(NEGS[t as number]!); neg = true; pos++; }
      else if (t === Token.END) { pos++; break; }
      else break;
    }
    let val = 0;
    for (const d of digits) val = val * 10 + d;
    return [neg ? -val : val, pos];
  }

  private _parseInts(tokens: Token[]): number[] {
    const vals: number[] = [];
    let digits: number[] = [];
    let neg = false;
    for (const t of tokens) {
      if (t in DIGITS) { digits.push(DIGITS[t as number]!); }
      else if (t in NEGS) { digits.push(NEGS[t as number]!); neg = true; }
      else if ((t === Token.END || t === Token.RECORD) && digits.length > 0) {
        let v = 0;
        for (const d of digits) v = v * 10 + d;
        vals.push(neg ? -v : v);
        digits = []; neg = false;
      }
    }
    return vals;
  }

  private _expect(toks: Token[], pos: number, want: Token, why: string): number {
    if (pos >= toks.length || toks[pos] !== want) throw new InterpreterError(why);
    return pos;
  }

  private _match(toks: Token[], openPos: number, end: number): number {
    let depth = 0;
    for (let i = openPos; i < end; i++) {
      if (toks[i] === LPAREN) depth++;
      else if (toks[i] === RPAREN) {
        depth--;
        if (depth === 0) return i;
      }
    }
    throw new InterpreterError('unbalanced region ( ... )');
  }
}
