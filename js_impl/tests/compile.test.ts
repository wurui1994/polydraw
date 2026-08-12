// Compile (end-to-end) tests — subset of c_impl/tests/test_compile.c.
// source -> lex -> parse -> IR -> interpret -> result.
import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { compile } from '../src/eval/parser.ts';
import { run } from '../src/backend/interp.ts';

const G0 = new Float64Array(0);

function ev(src: string): number {
  const r = compile(src);
  if (!r.ok) { process.stdout.write(`  [compile error: ${r.err}]\n`); return NaN; }
  return run(r.program, null, r.program.globals, null);
}
function evOk(src: string): boolean { return compile(src).ok; }

describe('compile: expressions', () => {
  it('num literal', () => assert.equal(ev('42'), 42));
  it('neg literal', () => assert.equal(ev('-7'), -7));
  it('add', () => assert.equal(ev('2+3'), 5));
  it('mul/div', () => assert.equal(ev('6*7/2'), 21));
  it('pemdas', () => assert.equal(ev('2+3*4'), 14));
  it('parens', () => assert.equal(ev('(2+3)*4'), 20));
  it('power', () => assert.equal(ev('2^10'), 1024));
  it('power left assoc', () => assert.equal(ev('2^3^2'), 64)); // (2^3)^2, matches original eval.c
  it('neg power', () => assert.equal(ev('-2^2'), -4)); // -(2^2)
  it('modulo', () => assert.equal(ev('10%3'), 1));
  it('unary chain', () => assert.equal(ev('--5'), 5));
  // unary-sign rules matching original eval.c: leading sign = binary with
  // implicit 0; mid-expression sign negates the immediate operand before ^.
  it('mid unary times pow', () => assert.equal(ev('3*-2^2'), 12));
  it('mid unary plus expr', () => assert.equal(ev('-2+3'), 1));
  it('mid unary var pow', () => assert.equal(ev('2*-3^2'), 18));
  it('mid unary paren', () => assert.equal(ev('2*-(3^2)'), -18));
  it('double minus pow', () => assert.equal(ev('1--2^2'), -3));
  it('plus then neg pow', () => assert.equal(ev('4+-2^2'), 8));
  it('neg exp', () => assert.equal(ev('2^-3'), 0.125));
  it('neg exp chain', () => assert.equal(ev('2^-3^2'), 0.015625));
  it('double minus', () => assert.equal(ev('2--3'), 5));
});

describe('compile: builtins', () => {
  it('sin(pi/2)', () => assert.ok(Math.abs(ev('sin(PI/2)') - 1) < 1e-12));
  it('sqrt', () => assert.equal(ev('sqrt(16)'), 4));
  it('abs', () => assert.equal(ev('abs(-9)'), 9));
  it('floor+ceil', () => assert.equal(ev('floor(2.7)+ceil(2.1)'), 5));
  it('log 2-arg', () => assert.ok(Math.abs(ev('log(100,10)') - 2) < 1e-12));
  it('min+max', () => assert.equal(ev('min(3,8)+max(3,8)'), 11));
  it('atn alias', () => assert.equal(ev('atn(0)'), 0));
  it('sqr alias', () => assert.equal(ev('sqr(25)'), 5));
  it('int truncates', () => assert.equal(ev('int(2.9)'), 2));
  it('fact', () => assert.equal(ev('fact(5)'), 120));
});

describe('compile: logic & comparisons', () => {
  it('less true', () => assert.equal(ev('3<8'), 1));
  it('equal', () => assert.equal(ev('5==5'), 1));
  it('and', () => assert.equal(ev('1&&0'), 0));
  it('or', () => assert.equal(ev('0||1'), 1));
});

describe('compile: variables & assignment', () => {
  it('assign return', () => assert.equal(ev('x=5'), 5));
  it('var use', () => assert.equal(ev('x=10; x*2'), 20));
  it('compound add', () => assert.equal(ev('x=10; x+=5; x'), 15));
  it('case insensitive', () => assert.equal(ev('Foo=7; FOO+foo'), 14));
});

describe('compile: control flow', () => {
  it('if true', () => assert.equal(ev('r=0; if(1){r=42}else{r=0}; r'), 42));
  it('if false', () => assert.equal(ev('r=0; if(0){r=42}else{r=99}; r'), 99));
  it('while loop', () => assert.equal(ev('i=0; s=0; while(i<5){s+=i; i+=1}; s'), 10));
  it('for loop', () => assert.equal(ev('s=0; for(i=0;i<5;i+=1){s+=i}; s'), 10));
  it('for factorial', () => assert.equal(ev('p=1; for(i=1;i<=5;i+=1){p*=i}; p'), 120));
  it('break', () => assert.equal(ev('i=0; while(1){ if(i>=3) break; i+=1 }; i'), 3));
});

describe('compile: EVAL idioms', () => {
  it('last value', () => assert.equal(ev('1; 2; 3'), 3));
  it('anon main after static', () => assert.equal(ev('static g; () { g = 42; g }'), 42));
  it('bare block main', () => assert.equal(ev('{ t=5; t*2 }'), 10));
  it('comma assignments', () => assert.equal(ev('a=1,b=2,c=3; c'), 3));
  it('param ref prefix', () => assert.equal(evOk('f(&x,&y,&z){ 1 } f(1,2,3)'), true));
});

describe('compile: functions', () => {
  it('simple', () => assert.equal(ev('sq(x){x*x} sq(7)'), 49));
  it('two args', () => assert.equal(ev('add(a,b){a+b} add(3,4)'), 7));
  it('calls func', () => assert.equal(ev('sq(x){x*x} sum(a,b){a+b} sum(sq(2),sq(3))'), 13));
  it('recursion', () => assert.equal(ev('fib(n){ if(n<2) return n; return fib(n-1)+fib(n-2) } fib(10)'), 55));
  it('5-arg call', () => assert.equal(ev('f(a,b,c,d,e){a*b*c*d*e} f(1,2,3,4,5)'), 120));
});

describe('compile: enum, static, arrays', () => {
  it('enum', () => assert.equal(ev('enum{A,B,C}; A+B+C'), 3));
  it('enum with init', () => assert.equal(ev('enum{A=10,B,C=20}; A+B+C'), 41));
  it('static scalar', () => assert.equal(ev('static g; g=42; g'), 42));
  it('static array write', () => assert.equal(ev('static a[4]; a[1]=7; a[1]'), 7));
  it('enum as size', () => assert.equal(ev('enum{N=4}; static a[N]; a[2]=5; a[2]'), 5));
  it('multidim 2d', () => assert.equal(ev('static g[3][4]; g[1][2]=99; g[1][2]'), 99));
  it('multidim 3d', () => assert.equal(ev('static b[2][3][4]; b[1][2][3]=7; b[1][2][3]'), 7));
  it('const dim power', () => assert.equal(evOk('enum{N=2^16}; static a[N]; 5'), true));
  it('init trailing comma', () => assert.equal(ev('static a[5]={1,2,3,}; a[2]'), 3));
});
