// EVAL Pratt parser + recursive-descent statements — TypeScript port of
// c_impl/src/eval/pd_parser.c (Plan A). Behaviour matches the C implementation.
//
// Operator precedence (higher = binds tighter), from eval.c:7352:
//   ^ : 7 (right-assoc)   * / % : 6   + - : 5   < <= > >= : 4
//   == != : 3   && : 2   || : 1   = += ... : 0 (assignment, right, lowest)

import type { Reg, Program, Instr } from './ir.ts';
import { Builder, Op, Fam, reg, NEGMOV } from './ir.ts';
import type { Tok, LexResult } from './lexer.ts';
import { TokKind, lex } from './lexer.ts';

// ---- symbol kinds ----
const SymKind = {
  VAR: 0, PARAM: 1, CONST: 2, BUILTIN: 3, EXT_VAR: 4, EXT_FUNC: 5, FUNC: 6, ARRAY: 7,
} as const;
type SymKind = (typeof SymKind)[keyof typeof SymKind];

interface Sym {
  name: string;
  kind: SymKind;
  nParams: number;
  reg: Reg;
  arraySize: number;   // 0 if not array
  dims: number[];      // per-dim sizes (multidim)
  nDims: number;
  nextOverload: number;
  funcIdx: number;
}

const NO_LOOP = -2147483648;

// ---- builtin function table (mirrors BUILTINS[]) ----
const BUILTINS: { name: string; op1: number; op2: number }[] = [
  { name: 'ABS', op1: Op.FABS, op2: -1 },
  { name: 'FABS', op1: Op.FABS, op2: -1 },
  { name: 'ACOS', op1: Op.ACOS, op2: -1 },
  { name: 'ASIN', op1: Op.ASIN, op2: -1 },
  { name: 'ATAN', op1: Op.ATAN, op2: -1 },
  { name: 'ATN', op1: Op.ATAN, op2: -1 },
  { name: 'CEIL', op1: Op.CEIL, op2: -1 },
  { name: 'COS', op1: Op.COS, op2: -1 },
  { name: 'EXP', op1: Op.EXP, op2: -1 },
  { name: 'FACT', op1: Op.FACT, op2: -1 },
  { name: 'FLOOR', op1: Op.FLOOR, op2: -1 },
  { name: 'INT', op1: Op.ROUND0, op2: -1 },
  { name: 'LOG', op1: Op.LOG, op2: Op.LOGB },
  { name: 'SGN', op1: Op.SGN, op2: -1 },
  { name: 'SIN', op1: Op.SIN, op2: -1 },
  { name: 'SQR', op1: Op.SQRT, op2: -1 },
  { name: 'SQRT', op1: Op.SQRT, op2: -1 },
  { name: 'TAN', op1: Op.TAN, op2: -1 },
  { name: 'UNIT', op1: Op.UNIT, op2: -1 },
  { name: 'ATAN2', op1: -1, op2: Op.ATAN2 },
  { name: 'FMOD', op1: -1, op2: Op.FMOD },
  { name: 'MIN', op1: -1, op2: Op.MIN },
  { name: 'MAX', op1: -1, op2: Op.MAX },
  { name: 'POW', op1: -1, op2: Op.POW },
  { name: 'FADD', op1: -1, op2: Op.FADD },
];

interface BinOp { tok: string; prec: number; rightAssoc: number; op: number; }
const BINOPS: BinOp[] = [
  { tok: '^', prec: 7, rightAssoc: 1, op: Op.POW },
  { tok: '*', prec: 6, rightAssoc: 0, op: Op.TIMES },
  { tok: '/', prec: 6, rightAssoc: 0, op: Op.SLASH },
  { tok: '%', prec: 6, rightAssoc: 0, op: Op.PERC },
  { tok: '+', prec: 5, rightAssoc: 0, op: Op.PLUS },
  { tok: '-', prec: 5, rightAssoc: 0, op: Op.MINUS },
  { tok: '<', prec: 4, rightAssoc: 0, op: Op.LES },
  { tok: '<=', prec: 4, rightAssoc: 0, op: Op.LESEQ },
  { tok: '>', prec: 4, rightAssoc: 0, op: Op.MOR },
  { tok: '>=', prec: 4, rightAssoc: 0, op: Op.MOREQ },
  { tok: '==', prec: 3, rightAssoc: 0, op: Op.EQU },
  { tok: '!=', prec: 3, rightAssoc: 0, op: Op.NEQU },
  { tok: '&&', prec: 2, rightAssoc: 0, op: Op.LAND },
  { tok: '||', prec: 1, rightAssoc: 0, op: Op.LOR },
];
const ASSIGNOPS: BinOp[] = [
  { tok: '=', prec: 0, rightAssoc: 1, op: Op.MOV },
  { tok: '+=', prec: 0, rightAssoc: 1, op: Op.PLUS },
  { tok: '-=', prec: 0, rightAssoc: 1, op: Op.MINUS },
  { tok: '*=', prec: 0, rightAssoc: 1, op: Op.TIMES },
  { tok: '/=', prec: 0, rightAssoc: 1, op: Op.SLASH },
  { tok: '%=', prec: 0, rightAssoc: 1, op: Op.PERC },
];

export class Parser {
  b: Builder;
  ts: Tok[];
  tok = 0;
  syms: Sym[] = [];
  globals: Float64Array = new Float64Array(0);
  globalsCap = 0;
  nGlobals = 0;
  funcs: Program[] = [];
  breakLabel = NO_LOOP;
  contLabel = NO_LOOP;
  lastLValue: Sym | null = null;
  lastLValueIsArrayIndex = 0;
  lastArrayIdx: Reg = reg(Fam.VOID, 0);
  lastValueReg: Reg = reg(Fam.VOID, 0);
  ok = true;
  err = '';
  errLine = 0;

  constructor(b: Builder, ts: Tok[]) {
    this.b = b;
    this.ts = ts;
  }

  // ---- token helpers ----
  cur(): Tok { return this.ts[this.tok] ?? this.ts[this.ts.length - 1]; }
  curAt(idx: number): Tok { return this.ts[idx] ?? this.ts[this.ts.length - 1]; }
  eat(): Tok { const t = this.cur(); if (this.tok < this.ts.length) this.tok++; return t; }
  accept(kind: number, text?: string): boolean {
    const t = this.cur();
    if (t.kind !== kind) return false;
    if (text !== undefined && (t.text !== text)) return false;
    this.tok++;
    return true;
  }
  acceptPunct(text: string): boolean { return this.accept(TokKind.PUNCT, text); }
  acceptIdent(text: string): boolean { return this.accept(TokKind.IDENT, text); }
  expectPunct(text: string): boolean {
    const t = this.cur();
    if (t.kind === TokKind.PUNCT && t.text === text) { this.tok++; return true; }
    this.error(`expected '${text}'`);
    return false;
  }
  error(msg: string): void {
    if (this.ok) {
      this.err = `${msg} at line ${this.cur().origLine}`;
      this.errLine = this.cur().origLine;
      this.ok = false;
    }
  }

  // ---- symbol table ----
  symFind(name: string, nParams = -1): Sym | null {
    // search backwards (innermost scope last)
    for (let i = this.syms.length - 1; i >= 0; i--) {
      const s = this.syms[i];
      if (s.name !== name) continue;
      if (nParams >= 0 && s.nParams !== nParams) {
        // try overload chain
        let o: Sym | null = (s.nextOverload >= 0) ? this.syms[s.nextOverload] : null;
        while (o) { if (o.nParams === nParams) return o; o = (o.nextOverload >= 0) ? this.syms[o.nextOverload] : null; }
        continue;
      }
      return s;
    }
    return null;
  }
  symFindName(name: string): Sym | null {
    for (let i = this.syms.length - 1; i >= 0; i--) if (this.syms[i].name === name) return this.syms[i];
    return null;
  }
  symAdd(name: string, kind: SymKind): Sym {
    const s: Sym = {
      name, kind, nParams: -1, reg: reg(Fam.VOID, 0),
      arraySize: 0, dims: [], nDims: 0, nextOverload: -1, funcIdx: -1,
    };
    this.syms.push(s);
    return s;
  }
  declareLocal(name: string): Sym {
    const s = this.symAdd(name, SymKind.VAR);
    s.reg = this.b.newLocal();
    return s;
  }

  installBuiltins(): void {
    for (const bi of BUILTINS) {
      if (bi.op1 >= 0) {
        const s = this.symAdd(bi.name, SymKind.BUILTIN);
        s.nParams = 1;
        s.reg = reg(Fam.VOID, bi.op1); // encode op in reg.off
      }
      if (bi.op2 >= 0) {
        const prev = this.symFindName(bi.name);
        const s = this.symAdd(bi.name, SymKind.BUILTIN);
        s.nParams = 2;
        s.reg = reg(Fam.VOID, bi.op2);
        if (prev) { s.nextOverload = prev.nextOverload; prev.nextOverload = this.syms.length - 1; }
      }
    }
    // PI constant
    const pi = this.symAdd('PI', SymKind.CONST);
    pi.reg = this.b.newConst(3.141592653589793);
  }

  // ---- expression parser (Pratt) ----
  parseExpr(): Reg { return this.parseExprPrec(0); }

  parseExprPrec(minPrec: number): Reg {
    // Prefix: handle unary +/- (count consecutive, track net negate).
    let negate = false;
    while (this.cur().kind === TokKind.PUNCT && this.cur().text === '+' || this.cur().text === '-') {
      if (this.cur().kind !== TokKind.PUNCT || this.cur().len !== 1) break;
      const ch = this.cur().text;
      if (ch !== '+' && ch !== '-') break;
      if (ch === '-') negate = !negate;
      this.eat();
    }
    let left = this.parsePrimary();
    if (!this.ok) return left;
    for (;;) {
      const t = this.cur();
      if (t.kind !== TokKind.PUNCT) break;
      // plain '=' is assignment — stop (caller). Two-char compound-assigns too.
      if (t.len === 1 && t.text === '=') break;
      if (t.len === 2 && t.text[1] === '=' &&
          '+-*/%'.indexOf(t.text[0]) >= 0) break;
      let bo: BinOp | null = null;
      for (const b of BINOPS) { if (t.text === b.tok) { bo = b; break; } }
      if (!bo) break;
      if (bo.prec < minPrec) break;
      this.eat();
      const nextMin = bo.rightAssoc ? bo.prec : bo.prec + 1;
      const right = this.parseExprPrec(nextMin);
      if (!this.ok) return left;
      const out = this.b.newLocal();
      this.b.emit2(bo.op, out, left, right);
      left = out;
    }
    // apply deferred unary negate after ^ bound into left
    if (negate) {
      const out = this.b.newLocal();
      this.b.emit1(NEGMOV, out, left);
      left = out;
    }
    return left;
  }

  parsePrimary(): Reg {
    const t = this.eat();
    this.lastLValue = null;
    this.lastLValueIsArrayIndex = 0;
    // address-of prefix: &ident
    if (t.kind === TokKind.PUNCT && t.len === 1 && t.text === '&') {
      return this.parsePrimary();
    }
    // string-prefix $ident / $string: return 0
    if (t.kind === TokKind.PUNCT && t.len === 1 && t.text.charCodeAt(0) === 0x24) {
      const c = this.cur();
      if (c.kind === TokKind.IDENT || c.kind === TokKind.STRING) this.eat();
      const out = this.b.newLocal();
      const z = this.b.newConst(0);
      this.b.emit1(Op.MOV, out, z);
      return out;
    }
    if (t.kind === TokKind.NUMBER) {
      const c = this.b.newConst(t.num);
      const out = this.b.newLocal();
      this.b.emit1(Op.MOV, out, c);
      return out;
    }
    if (t.kind === TokKind.STRING) {
      const out = this.b.newLocal();
      const zero = this.b.newConst(0);
      this.b.emit1(Op.MOV, out, zero);
      return out;
    }
    if (t.kind === TokKind.PUNCT && t.len === 1 && t.text === '(') {
      const r = this.parseExprPrec(0);
      this.expectPunct(')');
      return r;
    }
    if (t.kind === TokKind.IDENT) {
      const name = t.text;
      // RND/NRND (bare or rnd())
      if (name === 'RND' || name === 'NRND') {
        if (this.cur().kind === TokKind.PUNCT && this.cur().text === '(') {
          this.eat();
          this.expectPunct(')');
        }
        const out = this.b.newLocal();
        this.b.emit0(name === 'RND' ? Op.RND : Op.NRND, out);
        return out;
      }
      // function call?
      if (this.cur().kind === TokKind.PUNCT && this.cur().text === '(') {
        this.eat(); // (
        const args: Reg[] = [];
        let nArgs = 0;
        if (!(this.cur().kind === TokKind.PUNCT && this.cur().text === ')')) {
          for (;;) {
            if (this.cur().kind === TokKind.PUNCT && this.cur().text === ',') {
              args.push(this.b.newConst(0)); nArgs++; this.eat(); continue;
            }
            args.push(this.parseExprPrec(0)); nArgs++;
            if (!this.ok) return args[args.length - 1];
            if (this.acceptPunct(',')) {
              if (this.cur().kind === TokKind.PUNCT && this.cur().text === ')') {
                args.push(this.b.newConst(0)); nArgs++; break;
              }
              continue;
            }
            break;
          }
        }
        if (!this.expectPunct(')')) { return this.b.newConst(0); }
        let s = this.symFind(name, nArgs);
        if (!s) s = this.symFindName(name);
        if (!s) {
          // unresolved function — emit CALL with aux=-1 (returns 0)
          const out = this.b.newLocal();
          const idx = this.b.emit(Op.CALL, out, args, nArgs);
          this.b.instr[idx].aux = -1;
          return out;
        }
        if (s.kind === SymKind.BUILTIN) {
          const op = s.reg.off;
          const out = this.b.newLocal();
          if (nArgs === 1) this.b.emit1(op, out, args[0]);
          else if (nArgs === 2) this.b.emit2(op, out, args[0], args[1]);
          else this.error('bad arg count');
          return out;
        }
        if (s.kind === SymKind.EXT_FUNC || s.kind === SymKind.FUNC) {
          const out = this.b.newLocal();
          const idx = this.b.emit(Op.CALL, out, args, nArgs);
          if (s.kind === SymKind.FUNC) this.b.instr[idx].aux = s.funcIdx;
          else this.b.instr[idx].aux = -1000 - s.funcIdx;
          return out;
        }
        this.error('not a function');
        return this.b.newConst(0);
      }
      // array access? name[expr] or multi-dim
      if (this.cur().kind === TokKind.PUNCT && this.cur().text === '[') {
        let s = this.symFindName(name);
        if (!s) s = this.declareLocal(name);
        const idxs: Reg[] = [];
        let nd = 0;
        while (this.acceptPunct('[')) {
          const ix = this.parseExprPrec(0);
          if (!this.ok) return ix;
          if (!this.expectPunct(']')) return ix;
          if (nd < 8) idxs.push(ix), nd++;
        }
        // flatten multi-dim
        let flat: Reg;
        if (nd > 1) {
          let acc = this.b.newConst(0);
          for (let d = 0; d < nd; d++) {
            let term = idxs[d];
            if (d < nd - 1) {
              const tr = this.b.newLocal();
              this.b.emit1(Op.ROUND0, tr, term);
              term = tr;
            }
            let stride = 1;
            const symDims = s.nDims > 0 ? s.nDims : nd;
            for (let e = d + 1; e < symDims; e++) stride *= s.dims[e];
            if (stride !== 1) {
              const st = this.b.newConst(stride);
              const mul = this.b.newLocal();
              this.b.emit2(Op.TIMES, mul, term, st);
              term = mul;
            }
            const sum = this.b.newLocal();
            this.b.emit2(Op.PLUS, sum, acc, term);
            acc = sum;
          }
          flat = acc;
        } else {
          flat = idxs[0];
        }
        const out = this.b.newLocal();
        const ii = this.b.emit2(Op.PEEK, out, s.reg, flat);
        this.b.instr[ii].aux = s.arraySize;
        this.lastLValue = s;
        this.lastLValueIsArrayIndex = 1;
        this.lastArrayIdx = flat;
        return out;
      }
      // plain variable
      let s = this.symFindName(name);
      if (!s) s = this.declareLocal(name);
      if (s.kind === SymKind.CONST || s.kind === SymKind.VAR || s.kind === SymKind.PARAM ||
          s.kind === SymKind.EXT_VAR ||
          (s.kind === SymKind.ARRAY && s.arraySize === 0)) {
        if (s.kind === SymKind.VAR || s.kind === SymKind.PARAM || s.kind === SymKind.EXT_VAR ||
            (s.kind === SymKind.ARRAY && s.arraySize === 0)) {
          this.lastLValue = s;
          this.lastLValueIsArrayIndex = 0;
        }
        const out = this.b.newLocal();
        this.b.emit1(Op.MOV, out, s.reg);
        return out;
      }
      // function referenced without call — 0
      const out = this.b.newLocal();
      const z = this.b.newConst(0);
      this.b.emit1(Op.MOV, out, z);
      return out;
    }
    this.error('unexpected token in expression');
    return this.b.newConst(0);
  }

  // ---- statement parser ----
  parseStmt(): void {
    const t = this.cur();
    // label: IDENT :
    if (t.kind === TokKind.IDENT) {
      const a1 = this.tok + 1;
      if (a1 < this.ts.length &&
          this.ts[a1].kind === TokKind.PUNCT && this.ts[a1].text === ':') {
        this.eat(); this.eat(); this.acceptPunct(';'); return;
      }
    }
    if (t.kind === TokKind.IDENT) {
      if (this.acceptIdent('IF')) { this.parseIf(); return; }
      if (this.acceptIdent('WHILE')) { this.parseWhile(); return; }
      if (this.acceptIdent('FOR')) { this.parseFor(); return; }
      if (this.acceptIdent('DO')) { this.parseDoWhile(); return; }
      if (this.acceptIdent('ENUM')) { this.parseEnum(); this.acceptPunct(';'); return; }
      if (this.acceptIdent('STATIC')) { this.parseStatic(); this.acceptPunct(';'); return; }
      if (this.tryParseFunctionDef()) return;
      if (this.acceptIdent('RETURN')) {
        if (!(this.cur().kind === TokKind.PUNCT && this.cur().text === ';')) {
          const v = this.parseExpr();
          this.b.emit1(Op.RETURN, reg(Fam.VOID, 0), v);
        } else {
          this.b.emit1(Op.RETURN, reg(Fam.VOID, 0), this.b.newConst(0));
        }
        this.acceptPunct(';');
        return;
      }
      if (this.acceptIdent('BREAK')) {
        if (this.breakLabel !== NO_LOOP) this.b.emit0(Op.GOTO, reg(Fam.LABEL, this.breakLabel));
        this.acceptPunct(';'); return;
      }
      if (this.acceptIdent('CONTINUE')) {
        if (this.contLabel !== NO_LOOP) this.b.emit0(Op.GOTO, reg(Fam.LABEL, this.contLabel));
        this.acceptPunct(';'); return;
      }
      if (this.acceptIdent('GOTO')) {
        // skip label name; goto not fully supported
        this.eat(); this.acceptPunct(';'); return;
      }
    }
    if (t.kind === TokKind.PUNCT && t.text === '{') { this.parseBlock(); return; }
    if (t.kind === TokKind.PUNCT && t.text === ';') { this.eat(); return; }
    this.parseExprStmt();
  }

  parseExprStmt(): number {
    const t = this.cur();
    let isAssign = false;
    let lvSym: Sym | null = null;
    let lvIsArray = false;
    let assignOp = Op.MOV;
    if (t.kind === TokKind.IDENT) {
      const save = this.tok;
      const name = t.text;
      this.tok++;
      // skip array dims [..]
      if (this.cur().kind === TokKind.PUNCT && this.cur().text === '[') {
        while (this.cur().kind === TokKind.PUNCT && this.cur().text === '[') {
          this.tok++;
          let depth = 1;
          while (this.cur().kind !== TokKind.EOF && depth > 0) {
            if (this.cur().kind === TokKind.PUNCT && this.cur().text === '[') depth++;
            else if (this.cur().kind === TokKind.PUNCT && this.cur().text === ']') depth--;
            this.tok++;
          }
        }
      }
      const nt = this.cur();
      if (nt.kind === TokKind.PUNCT &&
          ((nt.len === 1 && nt.text === '=') ||
           (nt.len === 2 && nt.text[1] === '=' && '+-*/%^'.indexOf(nt.text[0]) >= 0))) {
        isAssign = true;
        for (const a of ASSIGNOPS) { if (nt.text === a.tok) { assignOp = a.op; break; } }
        let hadArray = false;
        for (let k = save + 1; k < this.tok; k++) {
          if (this.ts[k].kind === TokKind.PUNCT && this.ts[k].text === '[') { hadArray = true; break; }
        }
        lvIsArray = hadArray;
      }
      this.tok = save;
      if (isAssign) {
        lvSym = this.symFindName(name);
        if (!lvSym) lvSym = this.declareLocal(name);
      }
    }
    if (isAssign && lvSym) {
      this.lastLValue = null;
      this.parseExpr(); // consume lvalue tokens
      this.eat(); // assign op
      const value = this.parseExpr();
      if (!this.ok) return 0;
      if (lvIsArray) {
        const pop = assignOp === Op.MOV ? Op.POKE :
                    assignOp === Op.PLUS ? Op.POKEPLUS :
                    assignOp === Op.MINUS ? Op.POKEMINUS :
                    assignOp === Op.TIMES ? Op.POKETIMES :
                    assignOp === Op.SLASH ? Op.POKESLASH : Op.POKEPERC;
        const ii = this.b.emit2(pop, lvSym.reg, value, this.lastArrayIdx);
        this.b.instr[ii].aux = lvSym.arraySize;
      } else {
        if (assignOp === Op.MOV) {
          this.b.emit1(Op.MOV, lvSym.reg, value);
        } else {
          const cur = this.b.newLocal();
          this.b.emit1(Op.MOV, cur, lvSym.reg);
          const combined = this.b.newLocal();
          this.b.emit2(assignOp, combined, cur, value);
          this.b.emit1(Op.MOV, lvSym.reg, combined);
        }
      }
      const valResult = this.b.newLocal();
      this.b.emit1(Op.MOV, valResult, value);
      this.lastValueReg = valResult;
      if (this.acceptPunct(',')) { this.parseExprStmt(); return 0; }
      this.acceptPunct(';');
      return 0;
    }
    // postfix ++ / --
    if (t.kind === TokKind.IDENT) {
      const a1 = this.tok + 1;
      if (a1 + 1 < this.ts.length &&
          this.ts[a1].kind === TokKind.PUNCT && this.ts[a1].len === 1 &&
          (this.ts[a1].text === '+' || this.ts[a1].text === '-') &&
          this.ts[a1 + 1].kind === TokKind.PUNCT && this.ts[a1 + 1].len === 1 &&
          this.ts[a1 + 1].text === this.ts[a1].text) {
        const name = t.text;
        const c1 = this.ts[a1].text;
        this.eat(); this.eat(); this.eat();
        let lv = this.symFindName(name);
        if (!lv) lv = this.declareLocal(name);
        if (lv.kind === SymKind.VAR || lv.kind === SymKind.PARAM || lv.kind === SymKind.EXT_VAR ||
            (lv.kind === SymKind.ARRAY && lv.arraySize === 0)) {
          const o = c1 === '+' ? Op.PLUS : Op.MINUS;
          const one = this.b.newConst(1);
          const cur = this.b.newLocal();
          const combined = this.b.newLocal();
          this.b.emit1(Op.MOV, cur, lv.reg);
          this.b.emit2(o, combined, cur, one);
          this.b.emit1(Op.MOV, lv.reg, combined);
        }
        if (this.acceptPunct(',')) { this.parseExprStmt(); return 0; }
        this.acceptPunct(';');
        return 0;
      }
    }
    // bare expression
    const left = this.parseExpr();
    if (!this.ok) return 0;
    this.lastValueReg = left;
    if (this.acceptPunct(',')) { this.parseExprStmt(); return 1; }
    this.acceptPunct(';');
    return 1;
  }

  parseBlock(): void {
    if (!this.expectPunct('{')) return;
    while (this.ok && !(this.cur().kind === TokKind.PUNCT && this.cur().text === '}')) {
      this.parseStmt();
      if (!this.ok) break;
    }
    this.expectPunct('}');
  }
  parseBlockOrStmt(): void {
    if (this.cur().kind === TokKind.PUNCT && this.cur().text === '{') this.parseBlock();
    else this.parseStmt();
  }

  parseIf(): void {
    if (!this.expectPunct('(')) return;
    const cond = this.parseExpr();
    if (!this.expectPunct(')')) return;
    const jElse = this.b.emit1(Op.IF0, reg(Fam.VOID, 0), cond);
    this.parseBlockOrStmt();
    if (this.acceptIdent('ELSE')) {
      const jEnd = this.b.emit0(Op.GOTO, reg(Fam.VOID, 0));
      const elseLbl = this.b.labelHere();
      this.b.patchGotoTarget(jElse, elseLbl);
      this.parseBlockOrStmt();
      const endLbl = this.b.labelHere();
      this.b.patchGotoTarget(jEnd, endLbl);
    } else {
      const endLbl = this.b.labelHere();
      this.b.patchGotoTarget(jElse, endLbl);
    }
  }

  parseWhile(): void {
    const top = this.b.labelHere();
    if (!this.expectPunct('(')) return;
    const cond = this.parseExpr();
    if (!this.expectPunct(')')) return;
    const jEnd = this.b.emit1(Op.IF0, reg(Fam.VOID, 0), cond);
    const saveBrk = this.breakLabel, saveCont = this.contLabel;
    // sentinel: a negative value, never a real instr index. Break GOTOs
    // emitted inside the body use it as their target; we repoint them at end.
    const brkSentinel = -(jEnd + 1);
    this.breakLabel = brkSentinel;
    this.contLabel = top;
    this.parseBlockOrStmt();
    this.breakLabel = saveBrk; this.contLabel = saveCont;
    const goBack = this.b.emit0(Op.GOTO, reg(Fam.VOID, 0));
    this.b.patchGotoTarget(goBack, top);
    const endLbl = this.b.labelHere();
    this.b.patchGotoTarget(jEnd, endLbl);
    // repoint break GOTOs that used the sentinel
    for (let i = top; i < this.b.instr.length; i++) {
      const ins = this.b.instr[i];
      if (ins.op === Op.GOTO && ins.out.fam === Fam.LABEL && ins.out.off === brkSentinel) {
        ins.out.off = endLbl;
      }
    }
  }

  parseFor(): void {
    if (!this.expectPunct('(')) return;
    if (!this.acceptPunct(';')) this.parseExprStmt();
    this.acceptPunct(';');
    const top = this.b.labelHere();
    let cond: Reg = this.b.newConst(1);
    if (!(this.cur().kind === TokKind.PUNCT && this.cur().text === ';')) cond = this.parseExpr();
    if (!this.expectPunct(';')) return;
    const jEnd = this.b.emit1(Op.IF0, reg(Fam.VOID, 0), cond);
    const stepLabel = this.b.labelHere();
    // save step token range; parse body; parse step after body
    const stepStart = this.tok;
    let depth = 0;
    while (this.cur().kind !== TokKind.EOF) {
      if (this.cur().kind === TokKind.PUNCT && this.cur().text === '(') depth++;
      else if (this.cur().kind === TokKind.PUNCT && this.cur().text === ')') { if (depth === 0) break; depth--; }
      this.tok++;
    }
    const stepEnd = this.tok;
    if (!this.expectPunct(')')) return;
    const saveBrk = this.breakLabel, saveCont = this.contLabel;
    const brkSentinel = -(jEnd + 1);
    this.breakLabel = brkSentinel;
    this.contLabel = stepLabel;
    this.parseBlockOrStmt();
    // step
    const afterBody = this.tok;
    this.tok = stepStart;
    if (stepEnd > stepStart) this.parseExprStmt();
    this.tok = afterBody;
    const goBack = this.b.emit0(Op.GOTO, reg(Fam.VOID, 0));
    this.b.patchGotoTarget(goBack, top);
    const endLbl = this.b.labelHere();
    this.b.patchGotoTarget(jEnd, endLbl);
    for (let i = top; i < this.b.instr.length; i++) {
      const ins = this.b.instr[i];
      if (ins.op === Op.GOTO && ins.out.fam === Fam.LABEL && ins.out.off === brkSentinel) {
        ins.out.off = endLbl;
      }
    }
    this.breakLabel = saveBrk; this.contLabel = saveCont;
  }

  parseDoWhile(): void {
    const top = this.b.labelHere();
    const saveBrk = this.breakLabel, saveCont = this.contLabel;
    const brkSentinel = -1; // do-while break target patched at end
    this.breakLabel = brkSentinel;
    this.contLabel = top;
    this.parseBlockOrStmt();
    this.breakLabel = saveBrk; this.contLabel = saveCont;
    if (!this.expectPunct('(')) return;
    const cond = this.parseExpr();
    if (!this.expectPunct(')')) return;
    this.b.emit1(Op.IF1, reg(Fam.LABEL, top), cond);
    this.acceptPunct(';');
    const endLbl = this.b.labelHere();
    for (let i = top; i < this.b.instr.length; i++) {
      const ins = this.b.instr[i];
      if (ins.op === Op.GOTO && ins.out.fam === Fam.LABEL && ins.out.off === brkSentinel) {
        ins.out.off = endLbl;
      }
    }
  }

  // ---- const expr (for enum values, array dims) ----
  evalConstExpr(): number {
    const t = this.cur();
    let v: number;
    if (t.kind === TokKind.PUNCT && t.text === '(') {
      this.eat();
      v = this.evalConstExpr();
      if (!this.ok) return -1;
      if (!this.expectPunct(')')) return -1;
    } else if (t.kind === TokKind.NUMBER) {
      this.eat();
      v = t.num;
    } else if (t.kind === TokKind.IDENT) {
      const s = this.symFindName(t.text);
      if (s && s.kind === SymKind.CONST) {
        this.eat();
        v = this.b.consts[s.reg.off / 8];
      } else { this.error('expected constant'); return -1; }
    } else { this.error('expected constant'); return -1; }
    // power operator ^ (right-assoc)
    const t2 = this.cur();
    if (t2.kind === TokKind.PUNCT && t2.text === '^') {
      this.eat();
      const r = this.evalConstExpr();
      if (!this.ok) return -1;
      const base = v; let acc = 1;
      let e = r; if (e < 0) e = 0;
      for (let i = 0; i < e; i++) acc *= base;
      v = acc;
    }
    // trailing * / + -
    let tc = this.cur();
    while (tc.kind === TokKind.PUNCT && tc.len === 1 && '+-*/'.indexOf(tc.text) >= 0) {
      const op = tc.text; this.eat();
      const r = this.evalConstExpr();
      if (!this.ok) return -1;
      if (op === '*') v *= r;
      else if (op === '/') v /= r;
      else if (op === '+') v += r;
      else v -= r;
      tc = this.cur();
    }
    return v | 0;
  }

  parseEnum(): void {
    if (!this.expectPunct('{')) return;
    let nextVal = 0;
    for (;;) {
      const nt = this.eat();
      if (nt.kind !== TokKind.IDENT) { this.error('enum: expected name'); return; }
      const name = nt.text;
      if (this.acceptPunct('=')) {
        nextVal = this.evalConstExpr();
        if (!this.ok) return;
      }
      const s = this.symAdd(name, SymKind.CONST);
      s.reg = this.b.newConst(nextVal);
      nextVal++;
      if (this.acceptPunct(',')) continue;
      break;
    }
    this.expectPunct('}');
  }

  parseStatic(): void {
    for (;;) {
      const nt = this.eat();
      if (nt.kind !== TokKind.IDENT) { this.error('static: expected name'); return; }
      const name = nt.text;
      let isArr = false; let arrSize = 1;
      const dims: number[] = []; let nDims = 0;
      if (this.acceptPunct('[')) {
        isArr = true;
        for (;;) {
          const d = this.evalConstExpr();
          if (!this.ok) return;
          if (d <= 0) { this.error('invalid array dimension'); return; }
          arrSize *= d;
          if (nDims < 8) dims.push(d), nDims++;
          if (!this.expectPunct(']')) return;
          if (!this.acceptPunct('[')) break;
        }
      }
      // grow globals
      if (this.globalsCap === 0) { this.globalsCap = 256; this.globals = new Float64Array(256); }
      while (this.nGlobals + arrSize > this.globalsCap) {
        const ng = new Float64Array(this.globalsCap * 2);
        ng.set(this.globals);
        this.globals = ng;
        this.globalsCap *= 2;
      }
      const baseOff = this.nGlobals * 8;
      this.nGlobals += arrSize;
      const s = this.symAdd(name, SymKind.ARRAY);
      s.reg = reg(Fam.GLOBAL, baseOff);
      s.arraySize = isArr ? arrSize : 0;
      s.nDims = nDims;
      s.dims = dims.slice();
      // initializer
      if (this.acceptPunct('=')) {
        if (this.acceptPunct('{')) {
          let elem = 0;
          if (!(this.cur().kind === TokKind.PUNCT && this.cur().text === '}')) {
            for (;;) {
              const v = this.parseExpr();
              if (!this.ok) return;
              const slot = reg(Fam.GLOBAL, baseOff + elem * 8);
              this.b.emit1(Op.MOV, slot, v);
              elem++;
              if (this.acceptPunct(',')) {
                if (this.cur().kind === TokKind.PUNCT && this.cur().text === '}') break;
                continue;
              }
              break;
            }
          }
          this.expectPunct('}');
        } else {
          const v = this.parseExpr();
          if (!this.ok) return;
          this.b.emit1(Op.MOV, s.reg, v);
        }
      }
      if (this.acceptPunct(',')) continue;
      break;
    }
  }

  // ---- function defs ----
  tryParseFunctionDef(): boolean {
    const t = this.cur();
    if (t.kind !== TokKind.IDENT) return false;
    // function name followed by ( ... ) {
    const afterName = this.tok + 1;
    if (afterName >= this.ts.length) return false;
    if (!(this.ts[afterName].kind === TokKind.PUNCT && this.ts[afterName].text === '(')) return false;
    // scan to matching )
    let depth = 0; let s = afterName;
    while (s < this.ts.length) {
      const st = this.ts[s];
      if (st.kind === TokKind.PUNCT && st.text === '(') depth++;
      else if (st.kind === TokKind.PUNCT && st.text === ')') { depth--; if (depth === 0) break; }
      s++;
    }
    if (s >= this.ts.length) return false;
    const after = this.ts[s + 1];
    if (!(after.kind === TokKind.PUNCT && after.text === '{')) return false;
    // it's a function def
    this.eat(); // name
    this.eat(); // (
    const name = t.text;
    const savedSyms = this.syms.length;
    const fnSym = this.symFindName(name);
    // parse params
    let nParams = 0;
    if (!(this.cur().kind === TokKind.PUNCT && this.cur().text === ')')) {
      for (;;) {
        // skip optional type-prefix ident (ident followed by ident)
        while (this.cur().kind === TokKind.IDENT) {
          const nx = this.tok + 1;
          if (this.ts[nx]?.kind !== TokKind.IDENT) break;
          this.eat();
        }
        if (this.cur().kind === TokKind.PUNCT && this.cur().len === 1 &&
            (this.cur().text === '&' || this.cur().text === '$')) this.eat();
        const pt = this.eat();
        if (pt.kind !== TokKind.IDENT) { this.error('expected param name'); break; }
        const pname = pt.text;
        // array param name[...]
        if (this.cur().kind === TokKind.PUNCT && this.cur().text === '[') {
          while (this.acceptPunct('[')) {
            while (!(this.cur().kind === TokKind.EOF ||
                     (this.cur().kind === TokKind.PUNCT && this.cur().text === ']'))) this.eat();
            this.expectPunct(']');
          }
        }
        // fnptr param name(,,)
        if (this.cur().kind === TokKind.PUNCT && this.cur().text === '(') {
          let d = 0;
          do {
            if (this.cur().kind === TokKind.PUNCT && this.cur().text === '(') d++;
            this.eat();
          } while (d > 0 && this.cur().kind !== TokKind.EOF);
        }
        const ps = this.symAdd(pname, SymKind.PARAM);
        ps.reg = reg(Fam.PARAM, nParams * 8);
        nParams++;
        if (this.acceptPunct(',')) continue;
        break;
      }
    }
    if (fnSym) fnSym.nParams = nParams;
    if (!this.expectPunct(')')) { this.syms.length = savedSyms; return true; }
    if (!this.expectPunct('{')) { this.syms.length = savedSyms; return true; }
    // pre-register the function so recursive calls inside the body resolve.
    const fidx = this.funcs.length;
    this.funcs.push(emptyProgram()); // placeholder; filled after body parse
    let preSym = this.symFindName(name);
    if (!preSym) preSym = this.symAdd(name, SymKind.FUNC);
    preSym.nParams = nParams; preSym.funcIdx = fidx;
    // body
    const savedBreak = this.breakLabel, savedCont = this.contLabel;
    this.breakLabel = NO_LOOP; this.contLabel = NO_LOOP;
    this.lastValueReg = this.b.newConst(0);
    const savedBuilder = this.b;
    const fb = new Builder();
    this.b = fb;
    fb.consts = savedBuilder.consts; // share const table
    while (this.ok && !(this.cur().kind === TokKind.PUNCT && this.cur().text === '}')) {
      this.parseStmt();
      if (!this.ok) break;
    }
    this.expectPunct('}');
    fb.emit1(Op.RETURN, reg(Fam.VOID, 0), this.lastValueReg);
    this.breakLabel = savedBreak; this.contLabel = savedCont;
    // drop params/locals but KEEP the function symbol (so callers can find it)
    this.syms.length = savedSyms;
    if (!this.symFindName(name)) {
      const fs = this.symAdd(name, SymKind.FUNC);
      fs.nParams = nParams; fs.funcIdx = fidx;
    }
    // fill the reserved slot
    if (this.ok) {
      const fnp = fb.finish();
      fnp.nParams = nParams;
      fnp.globals = new Float64Array(0); // inherited at runtime
      this.funcs[fidx] = fnp;
    }
    this.b = savedBuilder;
    return true;
  }

  // ---- program top-level ----
  tryParseAnonMain(): boolean {
    const hasParen = this.cur().kind === TokKind.PUNCT && this.cur().text === '(' &&
      this.curAt(this.tok + 1).kind === TokKind.PUNCT && this.curAt(this.tok + 1).text === ')' &&
      this.curAt(this.tok + 2).kind === TokKind.PUNCT && this.curAt(this.tok + 2).text === '{';
    const hasBareBrace = this.cur().kind === TokKind.PUNCT && this.cur().text === '{';
    if (!hasParen && !hasBareBrace) return false;
    if (hasParen) { this.eat(); this.eat(); }
    if (!this.expectPunct('{')) return true;
    while (this.ok && !(this.cur().kind === TokKind.PUNCT && this.cur().text === '}')) {
      this.parseStmt();
      if (!this.ok) break;
    }
    this.expectPunct('}');
    this.b.emit1(Op.RETURN, reg(Fam.VOID, 0), this.lastValueReg);
    return true;
  }

  parseProgram(): boolean {
    this.lastValueReg = this.b.newConst(0);
    let sawMain = false;
    while (this.ok && this.cur().kind !== TokKind.EOF) {
      if (!sawMain && this.tryParseAnonMain()) { sawMain = true; continue; }
      this.parseStmt();
      if (!this.ok) break;
    }
    if (!sawMain) {
      this.b.emit1(Op.RETURN, reg(Fam.VOID, 0), this.lastValueReg);
    }
    return this.ok;
  }
}

// ---- top-level compile ----
export interface CompileResult { ok: boolean; program: Program; err: string; }

export function compile(src: string): CompileResult {
  const lexRes = lex(src);
  if (!lexRes.ok) {
    return { ok: false, program: emptyProgram(), err: `lex error: ${lexRes.err}` };
  }
  const b = new Builder();
  const p = new Parser(b, lexRes.toks);
  p.installBuiltins();
  p.parseProgram();
  if (!p.ok) {
    return { ok: false, program: emptyProgram(), err: p.err };
  }
  const prog = b.finish();
  prog.globals = p.globals.subarray(0, p.nGlobals).slice();
  prog.funcs = p.funcs;
  prog.host = null;
  return { ok: true, program: prog, err: '' };
}

function emptyProgram(): Program {
  return { instr: [], consts: [], strings: [], extra: [], nLocals: 0, nParams: 0,
    globals: new Float64Array(0), funcs: [], host: null, err: '' };
}
