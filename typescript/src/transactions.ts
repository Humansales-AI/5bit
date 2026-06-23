/**
 * GridDB Transactions — ACID via WAL + RECORD
 * ===========================================
 * Writes go to WAL immediately (durable).
 * TXN_BEGIN → [writes as PENDING] → TXN_COMMIT makes them visible.
 * Recovery: discard PENDING writes without COMMIT.
 */
import { Token, ParsedNumber } from './types';
import { Encoder } from './encoder';
import { Parser } from './parser';
import { WALedAllocGrid } from './alloc';

export class Transaction {
  grid: WALedAllocGrid;
  txnId: number;
  private finalized = false;
  private opCount = 0;
  private static _nextId = 1;
  private _onDone: (() => void) | null = null;

  constructor(grid: WALedAllocGrid, onDone?: () => void) {
    this.grid = grid;
    this.txnId = Transaction._nextId++;
    this._onDone = onDone || null;
  }

  put(recordId: number, tokens: Token[]): void {
    if (this.finalized) throw new Error('Transaction finalized');
    this.grid.write(recordId, tokens);
    this.opCount++;
  }

  delete(recordId: number): void {
    if (this.finalized) throw new Error('Transaction finalized');
    this.grid.write(recordId, [Token.D0, Token.END, Token.RECORD]);
    this.opCount++;
  }

  swap(fromRid: number, toRid: number, fromTokens: Token[], toTokens: Token[]): void {
    this.put(fromRid, fromTokens);
    this.put(toRid, toTokens);
  }

  commit(): void {
    if (this.finalized) throw new Error('Transaction finalized');
    this.finalized = true;
    const commitTokens = [...Encoder.encodeInteger(this.txnId), ...Encoder.encodeWord('COMMIT'), Token.RECORD];
    this.grid.write(1_000_000_000 + this.txnId, commitTokens);
    if (this._onDone) this._onDone();  // Clear parent's active flag
  }

  rollback(): void {
    if (this.finalized) throw new Error('Transaction finalized');
    this.finalized = true;
    if (this._onDone) this._onDone();  // Clear parent's active flag
  }
}

export class TransactionalGrid {
  grid: WALedAllocGrid;
  private active: Transaction | null = null;
  private txnCount = 0;

  constructor(dataDir: string) {
    this.grid = new WALedAllocGrid(dataDir);
  }

  begin(): Transaction {
    if (this.active) throw new Error('Transaction in progress');
    this.active = new Transaction(this.grid, () => {
      this.txnCount++;
      this.active = null;
    });
    return this.active;
  }

  // commit() and rollback() on the grid are deprecated — use txn.commit() directly.
  // They remain for backward compatibility.
  commit(): void {
    if (!this.active) throw new Error('No transaction');
    this.active.commit();
  }

  rollback(): void {
    if (!this.active) throw new Error('No transaction');
    this.active.rollback();
  }

  put(rid: number, tokens: Token[]): void {
    if (this.active) this.active.put(rid, tokens); else this.grid.write(rid, tokens);
  }

  delete(rid: number): void {
    if (this.active) this.active.delete(rid); else this.grid.delete(rid);
  }

  read(rid: number) { return this.grid.read(rid); }
  stats() { return { txnCount: this.txnCount, active: !!this.active }; }
  close(): void { this.grid.close(); }
}
