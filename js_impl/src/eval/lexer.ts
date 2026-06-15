// EVAL tokenizer — TypeScript port of c_impl/src/eval/pd_lexer.c.
// See Plan/01_Lexer.md. Behaviour must match the C implementation exactly
// (case-folding, comments, strings, hex numbers, #opt skip) so that the
// downstream parser/IR stay equivalent across both implementations.
//
// Identifiers are case-FOLDED to uppercase; strings preserve original case.

// Use a plain object (not `const enum`) so Node's --experimental-strip-types
// can run this file without a full type-checking pass.
export const TokKind = {
  EOF: 0,
  NUMBER: 1,
  IDENT: 2,
  STRING: 3,
  PUNCT: 4, // single or multi-char punctuation
  ERROR: 5,
} as const;
export type TokKind = (typeof TokKind)[keyof typeof TokKind];

export interface Tok {
  kind: TokKind;
  text: string; // normalized text (uppercase for idents; raw for punct/number-string)
  len: number; // text length (idents/punct)
  num: number; // numeric value (NUMBER)
  origOff: number; // offset into original source
  origLine: number; // 1-based line in original source
}

export interface LexResult {
  toks: Tok[];
  ok: boolean;
  err: string;
}

// 2-char punctuation; longest match first.
// NOTE: ++ and -- are NOT merged here so "--5" lexes as two unary '-'.
const PUNCT2 = ['<=', '>=', '==', '!=', '&&', '||', '+=', '-=', '*=', '/=', '%=', '^='];
const VALID_PUNCT = "+-*/%^()[]{};,<>=&$.:<=>!&|";

function isIdentStart(c: string): boolean {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c === '_';
}

export function isIdentChar(c: string): boolean {
  return isIdentStart(c) || (c >= '0' && c <= '9');
}

function startsPunct(c: string): boolean {
  return VALID_PUNCT.indexOf(c) >= 0;
}

function isDigit(c: string): boolean {
  return c >= '0' && c <= '9';
}
function isHexDigit(c: string): boolean {
  return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

export function lex(src: string): LexResult {
  const toks: Tok[] = [];
  let i = 0;
  let line = 1;
  const n = src.length;
  let err = '';
  let ok = true;

  const pushTok = (t: Tok) => { toks.push(t); };

  outer: while (i < n) {
    const c = src[i];

    // whitespace
    if (c === ' ' || c === '\t' || c === '\r') { i++; continue; }
    if (c === '\n') { line++; i++; continue; }

    // line comment //
    if (c === '/' && src[i + 1] === '/') {
      while (i < n && src[i] !== '\n') i++;
      continue;
    }
    // block comment
    if (c === '/' && src[i + 1] === '*') {
      i += 2;
      while (i < n && !(src[i] === '*' && src[i + 1] === '/')) {
        if (src[i] === '\n') line++;
        i++;
      }
      if (i < n) i += 2;
      continue;
    }
    // #opt(...) skip with paren nesting
    if (c === '#' && src.substr(i, 5) === '#opt(') {
      i += 5;
      let depth = 1;
      while (i < n && depth > 0) {
        if (src[i] === '(') depth++;
        else if (src[i] === ')') depth--;
        if (src[i] === '\n') line++;
        i++;
      }
      continue;
    }

    // string literal
    if (c === '"') {
      const origOff = i;
      const origLine = line;
      i++; // opening quote
      let s = '';
      while (i < n && src[i] !== '"') {
        if (src[i] === '\\' && src[i + 1] === '"') { s += '"'; i += 2; }
        else { if (src[i] === '\n') line++; s += src[i]; i++; }
      }
      if (src[i] !== '"') {
        err = 'unterminated string';
        ok = false;
        pushTok({ kind: TokKind.ERROR, text: '', len: 0, num: 0, origOff, origLine });
        break;
      }
      i++; // closing quote
      pushTok({ kind: TokKind.STRING, text: s, len: s.length, num: 0, origOff, origLine });
      continue;
    }

    // number: decimal, .5, hex
    if (isDigit(c) || (c === '.' && isDigit(src[i + 1] || ''))) {
      const origOff = i;
      const origLine = line;
      if (c === '0' && (src[i + 1] === 'x' || src[i + 1] === 'X')) {
        let j = i + 2;
        while (j < n && isHexDigit(src[j])) j++;
        const num = parseInt(src.slice(origOff, j), 16);
        pushTok({ kind: TokKind.NUMBER, text: src.slice(origOff, j), len: j - origOff, num, origOff, origLine });
        i = j;
        continue;
      }
      let j = i;
      while (j < n && isDigit(src[j])) j++;
      if (src[j] === '.') { j++; while (j < n && isDigit(src[j])) j++; }
      if (src[j] === 'e' || src[j] === 'E') {
        let k = j + 1;
        if (src[k] === '+' || src[k] === '-') k++;
        if (isDigit(src[k] || '')) { j = k; while (j < n && isDigit(src[j])) j++; }
      }
      const text = src.slice(origOff, j);
      pushTok({ kind: TokKind.NUMBER, text, len: j - origOff, num: parseFloat(text), origOff, origLine });
      i = j;
      continue;
    }

    // identifier (fold to upper)
    if (isIdentStart(c)) {
      const origOff = i;
      const origLine = line;
      let s = '';
      while (i < n && isIdentChar(src[i])) {
        let cc = src[i];
        if (cc >= 'a' && cc <= 'z') cc = String.fromCharCode(cc.charCodeAt(0) - 32);
        s += cc;
        i++;
      }
      pushTok({ kind: TokKind.IDENT, text: s, len: s.length, num: 0, origOff, origLine });
      continue;
    }

    // punctuation: 2-char then 1-char
    if (startsPunct(c)) {
      const origOff = i;
      const origLine = line;
      const two = src.substr(i, 2);
      if (PUNCT2.indexOf(two) >= 0) {
        pushTok({ kind: TokKind.PUNCT, text: two, len: 2, num: 0, origOff, origLine });
        i += 2;
        continue;
      }
      pushTok({ kind: TokKind.PUNCT, text: c, len: 1, num: 0, origOff, origLine });
      i++;
      continue;
    }

    // unknown
    err = `unexpected char '${c}' (0x${c.charCodeAt(0).toString(16)}) at line ${line}`;
    ok = false;
    pushTok({ kind: TokKind.ERROR, text: '', len: 0, num: 0, origOff: i, origLine: line });
    break;
  }

  // EOF token
  toks.push({ kind: TokKind.EOF, text: '', len: 0, num: 0, origOff: i, origLine: line });
  return { toks, ok, err };
}

export function dumpTokens(toks: Tok[]): string {
  const kn = ['EOF', 'NUM', 'IDENT', 'STR', 'PUNCT', 'ERR'];
  const lines: string[] = [];
  for (let k = 0; k < toks.length; k++) {
    const t = toks[k];
    let s = `[${String(k).padStart(3)}] L${t.origLine} ${kn[t.kind].padEnd(6)} `;
    if (t.kind === TokKind.NUMBER) s += t.num;
    else if (t.len) s += `'${t.text}'`;
    lines.push(s);
  }
  return lines.join('\n');
}
