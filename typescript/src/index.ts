// Types — values (enums, consts, functions)
export {
  Token,
  ParserState,
  TOKEN_NAME,
  tokenStreamToBinaryString,
} from './types';

// Types — interfaces (type-only)
export type {
  ParsedNumber,
  ParsedWord,
  ParsedOperator,
  ParsedScaledNumber,
  ChecksumResult,
  ParsedToken,
  ParsedRecord,
  GridRecord,
} from './types';

export {
  NUMERIC_DIGIT_VALUE,
  DIGIT_TO_TOKEN,
  DIGIT_TOKENS,
  NUMERIC_OPERATORS,
  NUMERIC_ANNOTATIONS,
  OPERATOR_SYMBOL,
  SYMBOL_TO_OPERATOR,
  WORD_CHAR,
  CHAR_TO_WORD_TOKEN,
  CONTROL_TOKENS,
  isDigitToken,
  isControlToken,
  isOperator,
  isAnnotation,
} from './tokens';

export { Encoder } from './encoder';
export { Parser } from './parser';

export {
  computeChecksum,
  verifyChecksum,
  appendChecksum,
  emitPeriodicChecksums,
} from './checksum';

export {
  packToBytes,
  unpackFromBytes,
  packToHex,
  unpackFromHex,
} from './serialization';

export {
  resolveScaledNumbers,
  evaluateParsed,
  DecimalArithmetic,
} from './arithmetic';

export { BinaryGrid } from './grid';

export {
  hammingDistance,
  manhattanDistance,
  queryByManhattan,
  queryByHammingShard,
  injectBitFlip,
  findNextSyncPoint,
} from './geometry';

export { AllocGrid, GroupCommitAllocGrid } from './alloc';
export type { AllocEntry, AllocRecord } from './alloc';
