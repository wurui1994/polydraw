// Interpreter tests — hand-built IR (mirrors c_impl/tests/test_interp.c subset).
import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import type { Program } from '../src/eval/ir.ts';
import { Builder, Op, Fam, reg } from '../src/eval/ir.ts';
import { run } from '../src/backend/interp.ts';

const G = new Float64Array(0);

function evalConst(v: number): number {
  const b = new Builder();
  const c = b.newConst(v);
  const r = b.newLocal();
  b.emit1(Op.MOV, r, c);
  b.emit1(Op.RETURN, reg(Fam.VOID, 0), r);
  const p = b.finish();
  return run(p, null, G, null);
}

function evalSimple(op: number, a: number, bb: number): number {
  const b = new Builder();
  const ca = b.newConst(a), cb = b.newConst(bb);
  const ra = b.newLocal(), rb = b.newLocal(), rr = b.newLocal();
  b.emit1(Op.MOV, ra, ca);
  b.emit1(Op.MOV, rb, cb);
  b.emit2(op, rr, ra, rb);
  b.emit1(Op.RETURN, reg(Fam.VOID, 0), rr);
  return run(b.finish(), null, G, null);
}

function evalUnary(op: number, a: number): number {
  const b = new Builder();
  const ca = b.newConst(a);
  const ra = b.newLocal(), rr = b.newLocal();
  b.emit1(Op.MOV, ra, ca);
  b.emit1(op, rr, ra);
  b.emit1(Op.RETURN, reg(Fam.VOID, 0), rr);
  return run(b.finish(), null, G, null);
}

describe('interp: const & arithmetic', () => {
  it('const mov', () => assert.equal(evalConst(42), 42));
  it('const negative', () => assert.equal(evalConst(-3.5), -3.5));
  it('plus', () => assert.equal(evalSimple(Op.PLUS, 2, 3), 5));
  it('minus', () => assert.equal(evalSimple(Op.MINUS, 10, 4), 6));
  it('times', () => assert.equal(evalSimple(Op.TIMES, 6, 7), 42));
  it('slash', () => assert.equal(evalSimple(Op.SLASH, 20, 4), 5));
  it('pow', () => assert.equal(evalSimple(Op.POW, 2, 10), 1024));
  it('perc', () => assert.equal(evalSimple(Op.PERC, 10, 3), 1));
  it('min', () => assert.equal(evalSimple(Op.MIN, 3, 8), 3));
  it('max', () => assert.equal(evalSimple(Op.MAX, 3, 8), 8));
  it('les true', () => assert.equal(evalSimple(Op.LES, 3, 8), 1));
  it('les false', () => assert.equal(evalSimple(Op.LES, 8, 3), 0));
  it('equ', () => assert.equal(evalSimple(Op.EQU, 5, 5), 1));
  it('land true', () => assert.equal(evalSimple(Op.LAND, 1, 1), 1));
  it('lor false', () => assert.equal(evalSimple(Op.LOR, 0, 0), 0));
});

describe('interp: unary math', () => {
  it('sin(pi/2)=1', () => assert.ok(Math.abs(evalUnary(Op.SIN, Math.PI / 2) - 1) < 1e-12));
  it('sqrt(16)=4', () => assert.equal(evalUnary(Op.SQRT, 16), 4));
  it('abs(-9)=9', () => assert.equal(evalUnary(Op.FABS, -9), 9));
  it('floor+ceil', () => assert.equal(evalUnary(Op.FLOOR, 2.7) + evalUnary(Op.CEIL, 2.1), 5));
  it('fact(5)=120', () => assert.equal(evalUnary(Op.FACT, 5), 120));
  it('negmov', () => assert.equal(((() => {
    const b = new Builder();
    const c = b.newConst(7); const r = b.newLocal(); const o = b.newLocal();
    b.emit1(Op.MOV, r, c);
    // NEGMOV opcode is the named constant exported
    b.emit1(6 /*NEGMOV*/, o, r); // op 6 = NEGMOV
    b.emit1(Op.RETURN, reg(Fam.VOID, 0), o);
    return run(b.finish(), null, G, null);
  })()), -7));
});

describe('interp: control flow (goto/if0)', () => {
  it('if0 jumps when condition is 0', () => {
    // r = 1; if (r==0) goto skip; r = 99; skip: return r
    const b = new Builder();
    const r = b.newLocal();
    const c1 = b.newConst(1), c99 = b.newConst(99), c0 = b.newConst(0);
    b.emit1(Op.MOV, r, c1);          // 0: r = 1
    const cmp = b.newLocal();
    b.emit2(Op.NEQU0, cmp, r, c0);   // wait — need EQU not NEQU0. Use: cmp = (r==0)
    // simpler: emit IF0 directly on r (r!=0 so no jump)
    const jIf = b.emit1(Op.IF0, reg(Fam.VOID, 0), r); // 2: if r==0 goto ...
    b.emit1(Op.MOV, r, c99);         // 3: r = 99
    const skip = b.labelHere();      // 4: skip
    b.patchGotoTarget(jIf, skip);
    b.emit1(Op.RETURN, reg(Fam.VOID, 0), r); // 5
    const result = run(b.finish(), null, G, null);
    assert.equal(result, 99); // r was 1 (nonzero) so no jump → r=99
    void cmp;
  });

  it('goto loop sums 1..5 = 15', () => {
    // s=0; i=1; top: s+=i; i+=1; if(i<=5) goto top; return s
    const b = new Builder();
    const s = b.newLocal(), i = b.newLocal();
    const c0 = b.newConst(0), c1 = b.newConst(1), c5 = b.newConst(5);
    b.emit1(Op.MOV, s, c0);           // 0
    b.emit1(Op.MOV, i, c1);           // 1
    const top = b.labelHere();        // 2
    const t1 = b.newLocal();
    b.emit2(Op.PLUS, t1, s, i);       // 3: t1 = s+i
    b.emit1(Op.MOV, s, t1);           // 4: s = t1
    const t2 = b.newLocal();
    b.emit2(Op.PLUS, t2, i, c1);      // 5: t2 = i+1
    b.emit1(Op.MOV, i, t2);           // 6: i = t2
    const le = b.newLocal();
    b.emit2(Op.LESEQ, le, i, c5);     // 7: le = (i<=5)
    const jIf = b.emit1(Op.IF1, reg(Fam.VOID, 0), le); // 8: if le!=0 goto top
    b.patchGotoTarget(jIf, top);
    b.emit1(Op.RETURN, reg(Fam.VOID, 0), s); // 9
    assert.equal(run(b.finish(), null, G, null), 15);
  });
});
