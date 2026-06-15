// Lexer tests — mirrors c_impl/tests/test_lexer.c (all 14 cases).
// Uses Node's built-in test runner (zero dev-deps; runs on Node 22+).
import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { lex, TokKind } from '../src/eval/lexer.ts';

const lexIt = (src: string) => lex(src).toks;

describe('lexer', () => {
  it('basic numbers', () => {
    const t = lexIt('3 3.14 .5 0xC8');
    assert.equal(t.length, 5); // 4 nums + EOF
    assert.equal(t[0].kind, TokKind.NUMBER); assert.equal(t[0].num, 3);
    assert.equal(t[1].num, 3.14);
    assert.equal(t[2].num, 0.5);
    assert.equal(t[3].num, 0xc8); // 200
  });

  it('identifiers uppercased', () => {
    const t = lexIt('sin SIN glBegin glvertex');
    assert.equal(t.length, 5);
    assert.equal(t[0].text, 'SIN');
    assert.equal(t[1].text, 'SIN');
    assert.equal(t[2].text, 'GLBEGIN');
    assert.equal(t[3].text, 'GLVERTEX');
  });

  it('multi-char punct (++/-- NOT merged)', () => {
    const t = lexIt('< = <= == != && || +=');
    assert.equal(t[0].len, 1); assert.equal(t[0].text, '<');
    assert.equal(t[1].len, 1); assert.equal(t[1].text, '=');
    assert.equal(t[2].text, '<=');
    assert.equal(t[3].text, '==');
    assert.equal(t[4].text, '!=');
    assert.equal(t[5].text, '&&');
    assert.equal(t[6].text, '||');
    assert.equal(t[7].text, '+=');
  });

  it('comments stripped', () => {
    const t = lexIt('a // comment here\nb /* block */ c');
    assert.equal(t.length, 4); // a b c EOF
    assert.equal(t[0].text, 'A');
    assert.equal(t[1].text, 'B');
    assert.equal(t[2].text, 'C');
  });

  it('string literal', () => {
    const t = lexIt('"hello world"');
    assert.equal(t[0].kind, TokKind.STRING);
    assert.equal(t[0].text, 'hello world');
  });

  it('string case preserved', () => {
    const t = lexIt('"MixedCase String"');
    assert.equal(t[0].kind, TokKind.STRING);
    assert.equal(t[0].text, 'MixedCase String');
  });

  it('string escaped quote', () => {
    const t = lexIt('"say \\"hi\\""');
    assert.equal(t[0].text, 'say "hi"');
  });

  it('#opt directive skipped', () => {
    const t = lexIt('#opt(nowin98) x = 1');
    assert.equal(t[0].text, 'X');
    assert.equal(t[1].text, '=');
    assert.equal(t[2].num, 1);
  });

  it('line numbers tracked', () => {
    const t = lexIt('a\nb\n\nc');
    assert.equal(t[0].origLine, 1);
    assert.equal(t[1].origLine, 2);
    assert.equal(t[2].origLine, 4);
  });

  it('expression tokens', () => {
    const t = lexIt('2 + 3 * x^2');
    assert.equal(t[0].num, 2);
    assert.equal(t[1].text, '+');
    assert.equal(t[2].num, 3);
    assert.equal(t[3].text, '*');
    assert.equal(t[4].text, 'X');
    assert.equal(t[5].text, '^');
    assert.equal(t[6].num, 2);
  });

  it('unterminated string error', () => {
    const r = lex('"oops');
    assert.equal(r.ok, false);
    assert.ok(r.err.length > 0);
  });

  it('empty input', () => {
    const t = lexIt('');
    assert.equal(t.length, 1);
    assert.equal(t[0].kind, TokKind.EOF);
  });

  it('comment only', () => {
    const t = lexIt('// just a comment\n/* and block */');
    assert.equal(t.length, 1);
    assert.equal(t[0].kind, TokKind.EOF);
  });

  it('function call', () => {
    const t = lexIt('sqrt(16)');
    assert.equal(t[0].text, 'SQRT');
    assert.equal(t[1].text, '(');
    assert.equal(t[2].num, 16);
    assert.equal(t[3].text, ')');
  });
});
