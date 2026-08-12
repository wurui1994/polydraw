// EVAL IR types — TypeScript port of c_impl/src/eval/pd_ir.h.
// Shared by parser, interpreter, and (future) JIT-JS backend.

// Register families (mirrors pd_Fam).
export const Fam = {
  VOID: 0,
  LOCAL: 1, // temp slot, off = byte offset (index*8)
  CONST: 2, // constant pool, off = index*8
  PARAM: 3, // function parameter, off = param slot
  LABEL: 4, // jump target, off = instr index
  PTR: 5, // pointer-to-parameter
  EXT: 6, // external (host) symbol
  GLOBAL: 7, // global static, off = byte offset
  STR: 8, // string table (folded into CONST)
  ARR: 9, // array table (folded into CONST)
} as const;
export type Fam = (typeof Fam)[keyof typeof Fam];

export interface Reg {
  fam: Fam;
  off: number; // meaning depends on fam
  aux: number; // array index adjust / func-ptr offset
}

export function reg(fam: Fam, off: number): Reg {
  return { fam, off, aux: 0 };
}

// Opcodes (mirrors pd_Op). 1:1 with eval.c:242 enum.
export const Op = {
  NOP: 0,
  // control flow
  GOTO: 1,
  RETURN: 2,
  RND: 3,
  NRND: 4,
  // 1-input (nIn==1): out = f(in[0])
  MOV: 5,
  NEQU0: 7, // out = (in[0] != 0) ? 1 : 0
  IF0: 8, // if (cond==0) goto out(label)
  IF1: 9, // if (cond!=0) goto out(label)
  FABS: 10, SGN: 11, UNIT: 12, FLOOR: 13, CEIL: 14, ROUND0: 15,
  SIN: 16, COS: 17, TAN: 18, ASIN: 19, ACOS: 20, ATAN: 21,
  SQRT: 22, EXP: 23, FACT: 24, LOG: 25,
  // 2-input (nIn==2): out = in[0] OP in[1]
  TIMES: 26, SLASH: 27, PERC: 28, PLUS: 29, MINUS: 30,
  LES: 31, LESEQ: 32, MOR: 33, MOREQ: 34, EQU: 35, NEQU: 36,
  LAND: 37, LOR: 38, POW: 39, MIN: 40, MAX: 41, FADD: 42, FMOD: 43,
  ATAN2: 44, LOGB: 45,
  // memory: arrays
  PEEK: 46,
  POKE: 47,
  POKETIMES: 48, POKESLASH: 49, POKEPERC: 50,
  POKEPLUS: 51, POKEMINUS: 52,
  // function call
  CALL: 53,
} as const;
export type Op = (typeof Op)[keyof typeof Op];

// NEGMOV sits between MOV and NEQU0 in the C enum; keep it explicit.
export const NEGMOV = 6;

export interface Instr {
  op: Op;
  aux: number; // function id for CALL; array size for PEEK/POKE; etc
  nIn: number; // 0,1,2 (CALL may use extra[])
  out: Reg; // destination or label-target
  in0: Reg; // r[1]
  in1: Reg; // r[2]
  extraIdx: number; // index into program.extra[] when nIn>2 (CALL)
}

// Compiled program (mirrors pd_Program).
export interface Program {
  instr: Instr[];
  consts: number[];
  strings: string[]; // raw string entries
  extra: Reg[]; // extra call arguments (>2 args)
  nLocals: number; // frame size (in doubles)
  nParams: number;
  globals: Float64Array; // shared static storage
  funcs: Program[]; // user-defined sub-functions
  host: Host | null; // external function table
  err: string;
}

// Host (external) function table. Each fn: (n:number, args:number[]) => number.
export interface HostFn { name: string; nParams: number; fn: (n: number, args: number[]) => number; }
export interface HostVar { name: string; get: () => number; set: (v: number) => void; }
export interface Host {
  fns: HostFn[];
  vars: Map<string, HostVar>;
  // string-literal table: slot index -> text. Populated by the parser when a
  // string literal is used as a host-function argument (glsettex/printf/...).
  strings?: Map<number, string>;
}

// ---- IR builder (used by the parser) ----
export class Builder {
  instr: Instr[] = [];
  consts: number[] = [];
  strings: string[] = [];
  extra: Reg[] = [];
  nLocals = 0;
  ok = true;
  err = '';

  newLocal(): Reg {
    const r = reg(Fam.LOCAL, this.nLocals * 8);
    this.nLocals++;
    return r;
  }

  newConst(v: number): Reg {
    // Intern: reuse existing equal constant (matches C behaviour where the
    // parser interns consts; dedup is optional but keeps dumps tidy).
    const idx = this.consts.push(v) - 1;
    return reg(Fam.CONST, idx * 8);
  }

  emit0(op: Op, out: Reg): number { return this.emit(op, out, [], 0); }
  emit1(op: Op, out: Reg, in0: Reg): number { return this.emit(op, out, [in0], 1); }
  emit2(op: Op, out: Reg, in0: Reg, in1: Reg): number { return this.emit(op, out, [in0, in1], 2); }

  emit(op: Op, out: Reg, ins: Reg[], nIn: number): number {
    const insFull: Instr = {
      op, aux: 0, nIn, out,
      in0: ins[0] ?? reg(Fam.VOID, 0),
      in1: ins[1] ?? reg(Fam.VOID, 0),
      extraIdx: -1,
    };
    // args beyond in[1] go into the extra[] table
    if (nIn > 2) {
      insFull.extraIdx = this.extra.length;
      for (let k = 2; k < nIn; k++) this.extra.push(ins[k]);
    }
    return this.instr.push(insFull) - 1;
  }

  labelHere(): number {
    // NOP anchor at the current position; returns its index.
    return this.emit0(Op.NOP, reg(Fam.VOID, 0));
  }

  patchGotoTarget(gotoInstrIdx: number, targetInstrIdx: number): void {
    // set the GOTO/IF0/IF1 .out.off = target instr index
    this.instr[gotoInstrIdx].out = reg(Fam.LABEL, targetInstrIdx);
  }

  finish(): Program {
    return {
      instr: this.instr,
      consts: this.consts,
      strings: this.strings,
      extra: this.extra,
      nLocals: this.nLocals,
      nParams: 0,
      globals: new Float64Array(0),
      funcs: [],
      host: null,
      err: this.err,
    };
  }
}

// ---- debug names ----
const OP_NAMES: Record<number, string> = {};
(() => {
  for (const k of Object.keys(Op)) OP_NAMES[(Op as any)[k]] = k;
  OP_NAMES[NEGMOV] = 'NEGMOV';
})();
export function opName(op: number): string { return OP_NAMES[op] ?? `?${op}`; }

const FAM_NAMES = ['VOID', 'LOCAL', 'CONST', 'PARAM', 'LABEL', 'PTR', 'EXT', 'GLOBAL', 'STR', 'ARR'];
export function famName(f: number): string { return FAM_NAMES[f] ?? `?${f}`; }

export function dumpProgram(p: Program): string {
  const lines: string[] = [];
  lines.push(`;; nInstr=${p.instr.length} nConst=${p.consts.length} nLocals=${p.nLocals} nParams=${p.nParams}`);
  for (let i = 0; i < p.instr.length; i++) {
    const in_ = p.instr[i];
    lines.push(`${String(i).padStart(4)}: ${opName(in_.op).padEnd(10)} ${famName(in_.out.fam)}:${in_.out.off}`);
  }
  return lines.join('\n');
}
