// glsl.ts — tiny GLSL ES 1.00 fragment-shader subset interpreter for the JS
// software rasterizer.
//
// Pipeline: GLSL source -> tokens -> expression-tree AST (one-time, at
// compileGLSL) -> generated JS function body (new Function). Each pixel then
// executes the generated native JS function, so shader-heavy scenes (balls.pss:
// 16k polygons) stay fast. Variable binding for varyings/uniforms happens via
// an injected `v0..vN` environment object, resolved by name through a compact
// slot map.
//
// Supported subset (what the ken/ tigrou/ examples use):
//   * types: float, int, bool, vec2/3/4, sampler2D; varying/uniform qualifiers
//   * vector ops: .x/.y/.z/.w .r/.g/.b/.a .s/.t swizzle, vecN() constructors,
//     scalar<->vector arithmetic, swizzle assignment
//   * control flow: if/else, return, discard
//   * built-ins: cos sin sqrt abs mod exp pow floor min max clamp length dot
//     cross normalize mix step smoothstep sign fract atan atan2 texture2D
//     int() float()

export interface GLSLVaryings {
  r: number; g: number; b: number; a: number; // color
  s: number; t: number; p: number; q: number; // texcoord
  nx: number; ny: number; nz: number;         // normal
  px: number; py: number; pz: number; pw: number; // object-space pos (gl_Vertex)
  ndx: number; ndy: number; ndz: number; ndw: number; // NDC pos (gl_Position)
}

export type GLSLTexFn = (unit: number, u: number, v: number, w?: number) => [number, number, number];

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------
type Tok = { t: 'id' | 'num' | 'op'; v: string | number };

function tokenize(src: string): Tok[] {
  const toks: Tok[] = [];
  let i = 0;
  const n = src.length;
  while (i < n) {
    const ch = src[i];
    if (ch === ' ' || ch === '\t' || ch === '\r' || ch === '\n') { i++; continue; }
    if (ch === '/') {
      if (src[i + 1] === '/') { while (i < n && src[i] !== '\n') i++; continue; }
      if (src[i + 1] === '*') { i += 2; while (i < n && !(src[i] === '*' && src[i + 1] === '/')) i++; i += 2; continue; }
    }
    if (/[a-zA-Z_]/.test(ch)) {
      let j = i;
      while (j < n && /[a-zA-Z0-9_]/.test(src[j])) j++;
      toks.push({ t: 'id', v: src.slice(i, j) });
      i = j;
      continue;
    }
    if (/[0-9]/.test(ch) || (ch === '.' && /[0-9]/.test(src[i + 1] ?? ''))) {
      let j = i;
      let seenDot = false;
      while (j < n && /[0-9.eE+-]/.test(src[j])) {
        const cj = src[j];
        if (cj === '.') { if (seenDot) break; seenDot = true; }
        if ((cj === 'e' || cj === 'E') && !/[0-9]/.test(src[j + 1] ?? '')) break;
        if ((cj === '+' || cj === '-') && src[j - 1] !== 'e' && src[j - 1] !== 'E') break;
        j++;
      }
      toks.push({ t: 'num', v: parseFloat(src.slice(i, j)) });
      i = j;
      continue;
    }
    const two = src.slice(i, i + 2);
    if (two === '&&' || two === '||' || two === '==' || two === '!=' || two === '<=' || two === '>=' ||
      two === '+=' || two === '-=' || two === '*=' || two === '/=' || two === '++' || two === '--') {
      toks.push({ t: 'op', v: two });
      i += 2;
      continue;
    }
    toks.push({ t: 'op', v: ch });
    i++;
  }
  return toks;
}

// ---------------------------------------------------------------------------
// AST (expression tree, type-tagged)
// ---------------------------------------------------------------------------
type E =
  | { k: 'num'; v: number }
  | { k: 'var'; name: string }
  | { k: 'swz'; e: E; sw: string }
  | { k: 'idx'; e: E; i: E }
  | { k: 'bin'; op: string; l: E; r: E }
  | { k: 'uni'; op: string; e: E }
  | { k: 'tern'; c: E; a: E; b: E }
  | { k: 'call'; name: string; args: E[] }
  | { k: 'ctor'; name: string; args: E[] }
  | { k: 'calluser'; name: string; args: E[] };

type S =
  | { k: 'expr'; e: E }
  | { k: 'assign'; name: string; e: E }
  | { k: 'idxassign'; name: string; i: E; e: E }
  | { k: 'swassign'; name: string; sw: string; e: E }
  | { k: 'decl'; type: string; names: string[]; inits: Map<string, E>; sizes: Map<string, number> }
  | { k: 'if'; cond: E; then: S[]; els: S[] }
  | { k: 'for'; init: S[]; cond: E | null; step: S[]; body: S[] }
  | { k: 'discard' }
  | { k: 'break' }
  | { k: 'continue' }
  | { k: 'return'; e: E | null }
  | { k: 'block'; body: S[] };

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------
interface FnDef { params: { type: string; name: string }[]; body: S[]; name: string; }

class GLSLParser {
  private toks: Tok[];
  private pos = 0;
  // user-defined functions collected during top-level parsing
  fnDefs = new Map<string, FnDef>();
  // top-level `varying` declarations (names, in order) — the VS outputs /
  // FS inputs shared between the two stages
  varyNames: string[] = [];
  // top-level `uniform sampler2D` names — each maps to a texture unit by
  // name (texN -> unit N), mirroring polydraw.c's hard-coded sampler setup
  uniSamplers: string[] = [];

  constructor(toks: Tok[]) { this.toks = toks; }

  private peek(): Tok | undefined { return this.toks[this.pos]; }
  private next(): Tok | undefined { return this.toks[this.pos++]; }
  private expect(t: 'id' | 'num' | 'op', v?: string): Tok {
    const tok = this.next();
    if (!tok || tok.t !== t || (v !== undefined && tok.v !== v)) {
      throw new Error(`glsl: expected ${t}${v ? ' ' + v : ''} at ${this.pos}, got ${JSON.stringify(tok)}`);
    }
    return tok;
  }
  private isOp(v: string): boolean { const p = this.peek(); return !!p && p.t === 'op' && p.v === v; }
  private eatOp(v: string): boolean { if (this.isOp(v)) { this.next(); return true; } return false; }
  private isId(v: string): boolean { const p = this.peek(); return !!p && p.t === 'id' && p.v === v; }
  private eatId(v: string): boolean { if (this.isId(v)) { this.next(); return true; } return false; }

  private isTypeWord(w: string): boolean {
    return w === 'float' || w === 'int' || w === 'bool' || w === 'vec2' || w === 'vec3' || w === 'vec4' ||
      w === 'sampler2D' || w === 'sampler3D' || w === 'varying' || w === 'uniform';
  }

  parseProgram(): S[] {
    const main: S[] = [];
    while (this.peek()) {
      if (this.isId('void') && this.toks[this.pos + 1]?.t === 'id' && this.toks[this.pos + 1]?.v === 'main') {
        main.push(...this.parseFunction());
      } else {
        this.parseDeclaration();
      }
    }
    return main;
  }

  private parseStmt(): S {
    if (this.isOp('{')) return { k: 'block', body: this.parseBlock() };
    if (this.isId('if')) return this.parseIf();
    if (this.isId('return')) {
      this.next();
      let e: E | null = null;
      if (!this.isOp(';')) e = this.parseExpr();
      this.expect('op', ';');
      return { k: 'return', e };
    }
    if (this.isId('discard')) { this.next(); this.expect('op', ';'); return { k: 'discard' }; }
    if (this.isId('break')) { this.next(); this.expect('op', ';'); return { k: 'break' }; }
    if (this.isId('continue')) { this.next(); this.expect('op', ';'); return { k: 'continue' }; }
    if (this.isId('for')) return this.parseFor();
    if (this.isId('while')) return this.parseWhile();
    if (this.peek()?.t === 'id' && this.isTypeWord(this.peek()!.v as string)) return this.parseVarDecl(false);
    if (this.peek()?.t === 'id') {
      const save = this.pos;
      const name = (this.peek() as Tok).v as string;
      this.next();
      if (this.eatOp('[')) { // indexed lvalue: arr[i] = / += ...
        const i = this.parseExpr();
        this.expect('op', ']');
        const compOp = this.peek()?.t === 'op' && (this.peek()!.v === '+=' || this.peek()!.v === '-=' || this.peek()!.v === '*=' || this.peek()!.v === '/=') ? this.next()!.v as string : null;
        if (this.eatOp('=') || compOp) {
          const e = this.parseAssignment();
          this.expect('op', ';');
          if (compOp) {
            const bin: E = { k: 'bin', op: compOp[0], l: { k: 'idx', e: { k: 'var', name }, i }, r: e };
            return { k: 'idxassign', name, i, e: bin };
          }
          return { k: 'idxassign', name, i, e };
        }
        // not an assignment (e.g. part of a larger expr) — reparse as expr stmt
        this.pos = save;
        const e = this.parseExpr();
        this.expect('op', ';');
        return { k: 'expr', e };
      }
      if (this.eatOp('=')) {
        const e = this.parseAssignment();
        this.expect('op', ';');
        return { k: 'assign', name, e };
      }
      const compOp = this.peek()?.t === 'op' && (this.peek()!.v === '+=' || this.peek()!.v === '-=' || this.peek()!.v === '*=' || this.peek()!.v === '/=') ? this.next()!.v as string : null;
      if (compOp) {
        const e = this.parseAssignment();
        this.expect('op', ';');
        const op = compOp[0];
        const bin: E = { k: 'bin', op, l: { k: 'var', name }, r: e };
        return { k: 'assign', name, e: bin };
      }
      if (this.eatOp('.')) {
        const sw = (this.next() as Tok).v as string;
        if (this.eatOp('=')) {
          const e = this.parseAssignment();
          this.expect('op', ';');
          return { k: 'swassign', name, sw, e };
        }
        if (this.peek()?.t === 'op' && (this.peek()!.v === '+=' || this.peek()!.v === '-=' || this.peek()!.v === '*=' || this.peek()!.v === '/=')) {
          const compOp2 = this.next()!.v as string;
          const e = this.parseAssignment();
          this.expect('op', ';');
          const op = compOp2[0];
          const bin: E = { k: 'bin', op, l: { k: 'swz', e: { k: 'var', name }, sw }, r: e };
          return { k: 'swassign', name, sw, e: bin };
        }
      }
      this.pos = save;
    }
    const e = this.parseExpr();
    this.expect('op', ';');
    return { k: 'expr', e };
  }

  private parseBlock(): S[] {
    this.expect('op', '{');
    const body: S[] = [];
    while (!this.isOp('}')) {
      if (!this.peek()) throw new Error('glsl: unterminated block');
      body.push(this.parseStmt());
    }
    this.expect('op', '}');
    return body;
  }

  private parseIf(): S {
    this.expect('id', 'if');
    this.expect('op', '(');
    const cond = this.parseExpr();
    this.expect('op', ')');
    const t = this.parseSingleOrBlock();
    let els: S[] = [];
    if (this.eatId('else')) els = this.parseSingleOrBlock();
    return { k: 'if', cond, then: t, els };
  }

  private parseSingleOrBlock(): S[] {
    if (this.isOp('{')) return this.parseBlock();
    return [this.parseStmt()];
  }

  private parseFor(): S {
    this.expect('id', 'for');
    this.expect('op', '(');
    const init: S[] = [];
    if (!this.isOp(';')) {
      if (this.peek()?.t === 'id' && this.isTypeWord(this.peek()!.v as string)) init.push(this.parseVarDecl(false));
      else { const e = this.parseExpr(); this.expect('op', ';'); init.push({ k: 'expr', e }); }
    } else this.expect('op', ';');
    let cond: E | null = null;
    if (!this.isOp(';')) cond = this.parseExpr();
    this.expect('op', ';');
    const step: S[] = [];
    if (!this.isOp(')')) { const e = this.parseExpr(); step.push({ k: 'expr', e }); }
    this.expect('op', ')');
    const body = this.parseSingleOrBlock();
    return { k: 'for', init, cond, step, body };
  }

  private parseWhile(): S {
    this.expect('id', 'while');
    this.expect('op', '(');
    const cond = this.parseExpr();
    this.expect('op', ')');
    const body = this.parseSingleOrBlock();
    return { k: 'for', init: [], cond, step: [], body };
  }

  private parseVarDecl(allowType: boolean): S {
    let type = '';
    if (allowType) type = this.parseType();
    else type = (this.next()?.v as string) ?? '';
    const names: string[] = [];
    const inits = new Map<string, E>();
    const sizes = new Map<string, number>();
    for (;;) {
      const name = this.expect('id');
      const nm = name.v as string;
      names.push(nm);
      if (this.eatOp('[')) { // fixed-size array: float rr[2];
        const sz = this.expect('num');
        sizes.set(nm, Math.max(0, sz.v as number));
        this.expect('op', ']');
      }
      if (this.eatOp('=')) inits.set(nm, this.parseAssignment());
      if (this.eatOp(',')) continue;
      break;
    }
    this.expect('op', ';');
    return { k: 'decl', type, names, inits, sizes };
  }

  private parseType(): string {
    const id = this.next();
    if (!id || id.t !== 'id') throw new Error('glsl: expected type');
    return id.v as string;
  }

  private parseDeclaration(): void {
    const id = this.peek();
    if (!id) return;
    if (id.t === 'id' && (id.v === 'varying' || id.v === 'uniform')) {
      const isVarying = id.v === 'uniform' ? false : true;
      this.next();
      // consume the type word, then collect declared names (comma list,
      // optional [size]) so the vertex runner knows which env slots to
      // export as varyings; uniform sampler2D names feed the unit mapping
      const tw = this.peek();
      const typeWord = tw && tw.t === 'id' ? (tw.v as string) : '';
      if (tw && tw.t === 'id') this.next();
      const isSampler = typeWord === 'sampler2D' || typeWord === 'sampler3D';
      while (!this.isOp(';')) {
        const p = this.peek();
        if (p && p.t === 'id') {
          if (isVarying) this.varyNames.push(p.v as string);
          else if (isSampler) this.uniSamplers.push(p.v as string);
          this.next(); continue;
        }
        if (!this.next()) throw new Error('glsl: unterminated decl');
      }
      this.expect('op', ';');
      return;
    }
    // Function definition: <rettype> name ( params ) { body }. Ret type may be
    // a type word (vec3/float/...) or custom; detect `type name (`.
    if (id.t === 'id') {
      const save = this.pos;
      const retType = (this.next()?.v as string) ?? '';
      if (this.peek()?.t === 'id' && this.toks[this.pos + 1]?.t === 'op' && this.toks[this.pos + 1]?.v === '(') {
        const fname = (this.next() as Tok).v as string;
        this.expect('op', '(');
        const params: { type: string; name: string }[] = [];
        while (!this.isOp(')')) {
          if (this.peek()?.t === 'id' && !this.isTypeWord(this.peek()!.v as string)) { this.next(); break; } // void
          const ptype = this.parseType();
          const pname = (this.expect('id').v as string) ?? '';
          params.push({ type: ptype, name: pname });
          if (!this.eatOp(',')) break;
        }
        this.expect('op', ')');
        const body = this.parseBlock();
        this.fnDefs.set(fname, { name: fname, params, body });
        return;
      }
      this.pos = save;
      this.parseType();
      this.parseVarDecl(false);
      return;
    }
    this.parseType();
    this.parseVarDecl(true);
  }

  private parseFunction(): S[] {
    this.expect('id', 'void');
    this.expect('id', 'main');
    this.expect('op', '(');
    while (!this.isOp(')')) { if (!this.peek()) throw new Error('glsl: unterminated main params'); this.next(); }
    this.expect('op', ')');
    return this.parseBlock();
  }

  // ---- expressions ----
  private parseExpr(): E { return this.parseAssignment(); }
  private parseAssignment(): E { return this.parseTernary(); }

  private parseTernary(): E {
    const c = this.parseOr();
    if (this.eatOp('?')) {
      const a = this.parseTernary();
      this.expect('op', ':');
      const b = this.parseTernary();
      return { k: 'tern', c, a, b };
    }
    return c;
  }

  private parseOr(): E {
    let l = this.parseAnd();
    for (;;) {
      if (this.eatOp('||')) { const r = this.parseAnd(); l = { k: 'bin', op: '||', l, r }; continue; }
      break;
    }
    return l;
  }

  private parseAnd(): E {
    let l = this.parseEq();
    for (;;) {
      if (this.eatOp('&&')) { const r = this.parseEq(); l = { k: 'bin', op: '&&', l, r }; continue; }
      break;
    }
    return l;
  }

  private parseEq(): E {
    let l = this.parseRel();
    for (;;) {
      if (this.eatOp('==')) { const r = this.parseRel(); l = { k: 'bin', op: '==', l, r }; continue; }
      if (this.eatOp('!=')) { const r = this.parseRel(); l = { k: 'bin', op: '!=', l, r }; continue; }
      break;
    }
    return l;
  }

  private parseRel(): E {
    let l = this.parseAdd();
    for (;;) {
      if (this.eatOp('<')) { const r = this.parseAdd(); l = { k: 'bin', op: '<', l, r }; continue; }
      if (this.eatOp('>')) { const r = this.parseAdd(); l = { k: 'bin', op: '>', l, r }; continue; }
      if (this.eatOp('<=')) { const r = this.parseAdd(); l = { k: 'bin', op: '<=', l, r }; continue; }
      if (this.eatOp('>=')) { const r = this.parseAdd(); l = { k: 'bin', op: '>=', l, r }; continue; }
      break;
    }
    return l;
  }

  private parseAdd(): E {
    let l = this.parseMul();
    for (;;) {
      if (this.eatOp('+')) { const r = this.parseMul(); l = { k: 'bin', op: '+', l, r }; continue; }
      if (this.eatOp('-')) { const r = this.parseMul(); l = { k: 'bin', op: '-', l, r }; continue; }
      break;
    }
    return l;
  }

  private parseMul(): E {
    let l = this.parseUnary();
    for (;;) {
      if (this.eatOp('*')) { const r = this.parseUnary(); l = { k: 'bin', op: '*', l, r }; continue; }
      if (this.eatOp('/')) { const r = this.parseUnary(); l = { k: 'bin', op: '/', l, r }; continue; }
      break;
    }
    return l;
  }

  private parseUnary(): E {
    if (this.eatOp('-')) return { k: 'uni', op: '-', e: this.parseUnary() };
    if (this.eatOp('+')) return this.parseUnary();
    if (this.eatOp('!')) return { k: 'uni', op: '!', e: this.parseUnary() };
    return this.parsePostfix();
  }

  private parsePostfix(): E {
    let e = this.parsePrimary();
    for (;;) {
      if (this.eatOp('.')) {
        const sw = (this.next() as Tok).v as string;
        e = { k: 'swz', e, sw };
        continue;
      }
      if (this.eatOp('[')) { // array indexing (e.g. uniform float rr[2])
        const i = this.parseExpr();
        this.expect('op', ']');
        e = { k: 'idx', e, i };
        continue;
      }
      if (this.peek()?.t === 'op' && (this.peek()!.v === '++' || this.peek()!.v === '--')) {
        const op = this.next()!.v as string;
        const nm = e.k === 'var' ? e.name : null;
        if (nm) {
          const bin: E = { k: 'bin', op: op[0], l: { k: 'var', name: nm }, r: { k: 'num', v: 1 } };
          // emit as side-effecting assignment: name = name ± 1, value = old
          e = { k: 'call', name: '__inc', args: [nm ? { k: 'var', name: nm } : { k: 'num', v: 0 }, bin] };
        }
        continue;
      }
      break;
    }
    return e;
  }

  private parsePrimary(): E {
    const p = this.peek();
    if (!p) throw new Error('glsl: unexpected end of expression');
    if (p.t === 'num') { this.next(); return { k: 'num', v: p.v as number }; }
    if (p.t === 'op' && p.v === '(') {
      this.next();
      const e = this.parseExpr();
      this.expect('op', ')');
      return e;
    }
    if (p.t === 'id') {
      const name = p.v as string;
      if (this.toks[this.pos + 1]?.t === 'op' && this.toks[this.pos + 1]?.v === '(') {
        if (BUILTINS.has(name)) { this.next(); return this.parseCall(name); }
        if (name === 'vec2' || name === 'vec3' || name === 'vec4') { this.next(); return this.parseCtor(name); }
        if (this.fnDefs.has(name)) {
          this.next();
          const args: E[] = [];
          this.expect('op', '(');
          while (!this.isOp(')')) {
            args.push(this.parseAssignment());
            if (!this.eatOp(',')) break;
          }
          this.expect('op', ')');
          return { k: 'calluser', name, args };
        }
        // unknown function call: treat as 0 (lenient)
        this.next();
        while (!this.isOp(')')) { if (!this.next()) break; }
        this.expect('op', ')');
        return { k: 'num', v: 0 };
      }
      this.next();
      return { k: 'var', name };
    }
    throw new Error(`glsl: unexpected token ${JSON.stringify(p)} (ctx: ${this.toks.slice(Math.max(0, this.pos - 5), this.pos + 5).map((t) => t.v).join(' ')})`);
  }

  private parseCtor(name: string): E {
    const args: E[] = [];
    this.expect('op', '(');
    while (!this.isOp(')')) {
      args.push(this.parseAssignment());
      if (!this.eatOp(',')) break;
    }
    this.expect('op', ')');
    return { k: 'ctor', name, args };
  }

  private parseCall(name: string): E {
    const args: E[] = [];
    this.expect('op', '(');
    while (!this.isOp(')')) {
      args.push(this.parseAssignment());
      if (!this.eatOp(',')) break;
    }
    this.expect('op', ')');
    return { k: 'call', name, args };
  }
}

const BUILTINS = new Set([
  'cos', 'sin', 'sqrt', 'abs', 'mod', 'exp', 'pow', 'floor', 'min', 'max',
  'clamp', 'length', 'dot', 'cross', 'normalize', 'mix', 'step', 'smoothstep',
  'sign', 'fract', 'atan', 'atan2', 'texture2D', 'texture3D', 'int', 'float', 'ftransform',
]);

// ---------------------------------------------------------------------------
// Code generation: AST -> JS function body
// ---------------------------------------------------------------------------
// Variable binding strategy: every shader variable lives in a flat Map `env`.
// Hot variables (gl_FragColor output, gl_Color, c/t/p/n and small locals) are
// accessed via env.get/set — Map<string,Val> with number|number[] values is
// fast enough for the per-pixel workload once the interpreter overhead is gone.
const COMP_IDX: Record<string, number> = { x: 0, r: 0, s: 0, y: 1, g: 1, t: 1, z: 2, b: 2, p: 2, w: 3, a: 3, q: 3 };
const COMP_NAME = ['x', 'y', 'z', 'w'];

function genE(e: E): string {
  switch (e.k) {
    case 'num': return String(e.v);
    case 'var': return `env[${JSON.stringify(e.name)}]`;
    case 'swz': {
      const inner = genE(e.e);
      if (e.sw.length === 1) return `_swz1(${inner},${COMP_IDX[e.sw[0]]})`;
      return `_swz(${inner},${JSON.stringify(e.sw)})`;
    }
    case 'idx': return `_ag(${genE(e.e)},${genE(e.i)})`;
    case 'bin': {
      const l = genE(e.l), r = genE(e.r);
      switch (e.op) {
        case '+': return `_bop(${l},${r},1)`;
        case '-': return `_bop(${l},${r},2)`;
        case '*': return `_bop(${l},${r},3)`;
        case '/': return `_bop(${l},${r},4)`;
        case '&&': return `(_sc(${l})!==0 && _sc(${r})!==0 ? 1 : 0)`;
        case '||': return `(_sc(${l})!==0 || _sc(${r})!==0 ? 1 : 0)`;
        case '==': return `(_sc(${l})===_sc(${r}) ? 1 : 0)`;
        case '!=': return `(_sc(${l})!==_sc(${r}) ? 1 : 0)`;
        case '<': return `(_sc(${l})<_sc(${r}) ? 1 : 0)`;
        case '>': return `(_sc(${l})>_sc(${r}) ? 1 : 0)`;
        case '<=': return `(_sc(${l})<=_sc(${r}) ? 1 : 0)`;
        case '>=': return `(_sc(${l})>=_sc(${r}) ? 1 : 0)`;
        default: throw new Error('glsl: bad binop ' + e.op);
      }
    }
    case 'uni': {
      const inner = genE(e.e);
      switch (e.op) {
        case '-': return `_neg(${inner})`;
        case '!': return `(_sc(${inner})!==0 ? 0 : 1)`;
        default: throw new Error('glsl: bad unop ' + e.op);
      }
    }
    case 'tern': return `(_sc(${genE(e.c)})!==0 ? ${genE(e.a)} : ${genE(e.b)})`;
    case 'call': return genCall(e);
    case 'calluser': {
      const args = e.args.map(genE).join(',');
      return `_f_${e.name}(${args})`;
    }
    case 'ctor': {
      const sz = e.name === 'vec2' ? 2 : e.name === 'vec3' ? 3 : 4;
      const args = e.args.map(genE).join(',');
      return `_ctor([${args}],${sz})`;
    }
  }
}

function genCall(e: { name: string; args: E[] }): string {
  const a = e.args.map(genE);
  switch (e.name) {
    case 'cos': return `_ufun(${a[0]},Math.cos)`;
    case 'sin': return `_ufun(${a[0]},Math.sin)`;
    case 'sqrt': return `_ufun(${a[0]},Math.sqrt)`;
    case 'abs': return `_ufun(${a[0]},Math.abs)`;
    case 'exp': return `_ufun(${a[0]},Math.exp)`;
    case 'floor': return `_ufun(${a[0]},Math.floor)`;
    case 'sign': return `_ufun(${a[0]},Math.sign)`;
    case 'fract': return `_ufun(${a[0]},x=>x-Math.floor(x))`;
    case 'mod': return `_bop(${a[0]},${a[1]},(x,y)=>x-y*Math.floor(x/y))`;
    case 'pow': return `_bop(${a[0]},${a[1]},Math.pow)`;
    case 'min': return `_bop(${a[0]},${a[1]},Math.min)`;
    case 'max': return `_bop(${a[0]},${a[1]},Math.max)`;
    case 'clamp': return `_bop(_bop(${a[0]},${a[2]},Math.min),${a[1]},Math.max)`;
    case 'length': return `_glen(${a[0]})`;
    case 'dot': return `_gdot(${a[0]},${a[1]})`;
    case 'cross': return `_gcross(${a[0]},${a[1]})`;
    case 'normalize': return `_gnorm(${a[0]})`;
    case 'mix': return `_g_mix(${a[0]},${a[1]},${a[2]})`;
    case 'step': return `_g_step(${a[0]},${a[1]})`;
    case 'smoothstep': return `_g_smooth(${a[0]},${a[1]},${a[2]})`;
    case 'atan':
      if (a.length >= 2) return `Math.atan2(_sc(${a[0]}),_sc(${a[1]}))`;
      return `_ufun(${a[0]},Math.atan)`;
    case 'atan2': return `Math.atan2(_sc(${a[0]}),_sc(${a[1]}))`;
    case 'int': return `_ufun(${a[0]},x=>Math.trunc(x))`;
    case 'float': return `${a[0]}`;
    case 'ftransform': return `_ftransform(env)`;
    case 'texture2D': return `_tex(${a[0]},${a[1]},texFn)`;
    case 'texture3D': return `_tex3(${a[0]},${a[1]},texFn)`;
    case '__inc': {
      // a[0] is the raw E node: pass the NAME string so _inc writes it back
      const src = e.args[0];
      const nm = src && src.k === 'var' ? JSON.stringify(src.name) : '0';
      return `_inc(env,${nm},${genE(e.args[1])})`;
    }
    default: throw new Error('glsl: unknown builtin ' + e.name);
  }
}

function genStmt(s: S, out: string[]): void {
  switch (s.k) {
    case 'expr': out.push(`${genE(s.e)};`); break;
    case 'assign': out.push(`env[${JSON.stringify(s.name)}]=${genE(s.e)};`); break;
    case 'idxassign': out.push(`_aset(env,${JSON.stringify(s.name)},${genE(s.i)},${genE(s.e)});`); break;
    case 'swassign': {
      const idx = COMP_IDX[s.sw[0]];
      out.push(`_swset(env,${JSON.stringify(s.name)},${idx},${genE(s.e)});`);
      break;
    }
    case 'decl': {
      for (const nm of s.names) {
        const sz = s.sizes.get(nm);
        if (sz !== undefined) { out.push(`env[${JSON.stringify(nm)}]=_arr(${sz},${JSON.stringify(s.type)});`); continue; }
        const init = s.inits.get(nm);
        if (init) out.push(`env[${JSON.stringify(nm)}]=${genE(init)};`);
        else if (s.type === 'sampler2D' || s.type === 'sampler3D') out.push(`env[${JSON.stringify(nm)}]=_sunit(${JSON.stringify(nm)});`);
        else if (s.type === 'uniform') out.push(`env[${JSON.stringify(nm)}]=0;`);
        else out.push(`env[${JSON.stringify(nm)}]=_dflt(${JSON.stringify(s.type)});`);
      }
      break;
    }
    case 'if': {
      out.push(`if (_sc(${genE(s.cond)})!==0) {`);
      for (const t of s.then) genStmt(t, out);
      out.push('} else {');
      for (const t of s.els) genStmt(t, out);
      out.push('}');
      break;
    }
    case 'for': {
      out.push('{ let _i = 0;');
      for (const t of s.init) genStmt(t, out);
      // step expressions go in the increment clause so `continue` runs them
      const steps: string[] = [];
      for (const t of s.step) if (t.k === 'expr') steps.push(genE(t.e));
      const inc = `_i++${steps.length ? ', ' + steps.join(', ') : ''}`;
      out.push(`for (; _i < 100000 && (${s.cond ? '_sc(' + genE(s.cond) + ')!==0' : '1'}); ${inc}) {`);
      for (const t of s.body) genStmt(t, out);
      out.push('} }');
      break;
    }
    case 'discard': out.push('return null;'); break;
    case 'break': out.push('break;'); break;
    case 'continue': out.push('continue;'); break;
    case 'return': out.push(`return ${s.e ? genE(s.e) : 'null'};`); break;
    case 'block': for (const t of s.body) genStmt(t, out); break;
  }
}

// ---------------------------------------------------------------------------
// Runtime helpers. Two copies: module-level functions (used by the generated
// code when the RUNTIME string is inlined) and the RUNTIME string that gets
// pasted into each compiled shader function so per-pixel calls are plain JS.
// ---------------------------------------------------------------------------
const RUNTIME = `
function _isV(x){ return Array.isArray(x); }
function _sc(x){ return _isV(x) ? x[0] : x; }
const _I4=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1];
function _matvec(m,v){ return [m[0]*v[0]+m[4]*v[1]+m[8]*v[2]+m[12]*v[3], m[1]*v[0]+m[5]*v[1]+m[9]*v[2]+m[13]*v[3], m[2]*v[0]+m[6]*v[1]+m[10]*v[2]+m[14]*v[3], m[3]*v[0]+m[7]*v[1]+m[11]*v[2]+m[15]*v[3]]; }
function _matmat(a,b){ const o=new Array(16); for(let c=0;c<4;c++) for(let r=0;r<4;r++){ let s=0; for(let k=0;k<4;k++) s+=a[k*4+r]*b[c*4+k]; o[c*4+r]=s; } return o; }
function _ftransform(env){ const m=env['gl_ModelViewProjectionMatrix']||_I4; const v=env['gl_Vertex']||[0,0,0,1]; return _matvec(m,v); }
function _bop(a,b,op){ const f=(typeof op==='function')?op:(x,y)=>op===1?x+y:op===2?x-y:op===3?x*y:x/y; if (_isV(a)||_isV(b)) { if(_isV(a)&&a.length===16){ if(_isV(b)&&b.length===16) return _matmat(a,b); return _matvec(a,_isV(b)?b:[b,b,b,1]); } if(_isV(b)&&b.length===16&&_isV(a)) { const t=_matvec(b,[a[0]??0,a[1]??0,a[2]??0,a[3]??0]); return a.length===4?t:t.slice(0,a.length); } if(_isV(a)&&_isV(b)) { const n=Math.max(a.length,b.length); const o=new Array(n); for(let i=0;i<n;i++){ o[i]=f(a[i]??0,b[i]??0); } return o; } const v=_isV(a)?a:b; const s=_isV(a)?b:a; const o=new Array(v.length); for(let i=0;i<v.length;i++){ o[i]=f(v[i],s); } return o; } return f(a,b); }
function _neg(a){ return _isV(a) ? a.map(x=>-x) : -a; }
function _swz1(v,i){ return (_isV(v) ? v : [v])[i] ?? 0; }
function _swz(v,sw){ const a=_isV(v)?v:[v]; const o=[]; for(let i=0;i<sw.length;i++){ const ch=sw.charCodeAt(i); const k=ch===120||ch===114||ch===115?0:ch===121||ch===103||ch===116?1:ch===122||ch===98||ch===112?2:ch===119||ch===97||ch===113?3:-1; if(k<0) return 0; o.push(k<a.length?a[k]:0); } return o.length===1?o[0]:o; }
function _ctor(args,sz){ const flat=[]; for(const a of args){ if(_isV(a)) flat.push(...a); else flat.push(a); } const o=new Array(sz); if(flat.length===1){ for(let i=0;i<sz;i++) o[i]=flat[0]; return o; } for(let i=0;i<sz;i++) o[i]=flat[i]??0; return o; }
function _ufun(a,f){ return _isV(a) ? a.map(f) : f(a); }
function _glen(a){ const arr=_isV(a)?a:[a]; let s=0; for(const x of arr) s+=x*x; return Math.sqrt(s); }
function _gdot(a,b){ const av=_isV(a)?a:[a]; const bv=_isV(b)?b:[b]; let s=0; for(let i=0;i<Math.max(av.length,bv.length);i++) s+=(av[i]??0)*(bv[i]??0); return s; }
function _gcross(a,b){ const u=_isV(a)?a:[a,0,0]; const v=_isV(b)?b:[b,0,0]; return [u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0]]; }
function _gnorm(a){ const arr=_isV(a)?a:[a]; let s=0; for(const x of arr) s+=x*x; const l=Math.sqrt(s); if(l===0) return arr.map(()=>0); return arr.map(x=>x/l); }
function _g_mix(a,b,t){ const tv=_sc(t); if(_isV(a)||_isV(b)){ const av=_isV(a)?a:[a]; const bv=_isV(b)?b:[b]; const n=Math.max(av.length,bv.length); const o=[]; for(let i=0;i<n;i++) o.push(av[i]*(1-tv)+bv[i]*tv); return o; } return a*(1-tv)+b*tv; }
function _g_step(e,x){ if(_isV(e)||_isV(x)) return _bop(x,e,(edge,v)=>v>=edge?1:0); return x>=e?1:0; }
function _g_smooth(a,b,x){ const e0=_sc(a),e1=_sc(b),xv=_sc(x); const t=Math.min(1,Math.max(0,(xv-e0)/(e1-e0))); return t*t*(3-2*t); }
function _dflt(t){ return t==='vec2'?[0,0]:t==='vec3'?[0,0,0]:t==='vec4'?[0,0,0,0]:0; }
function _tex(sm,coord){ const c=_isV(coord)?coord:[coord]; if(!texFn) return [0,0,0,1]; const u=(sm===undefined||sm===null)?0:(_isV(sm)?(sm[0]??0):sm)|0; const r=texFn(u,c[0]??0,c[1]??0); return [r[0],r[1],r[2],1]; }
function _tex3(sm,coord){ const c=_isV(coord)?coord:[coord]; if(!texFn) return [0,0,0,1]; const u=(sm===undefined||sm===null)?0:(_isV(sm)?(sm[0]??0):sm)|0; const r=texFn(u,c[0]??0,c[1]??0,c[2]??0); return [r[0],r[1],r[2],1]; }
function _swset(env,name,i,v){ const cur=env[name]; const a=_isV(cur)?cur.slice():[cur??0]; a[i]=_isV(v)?v[0]:v; env[name]=a.length===1?a[0]:a; }
function _inc(env,name,v){ const old=env[name]; env[name]=v; return old; }
function _ag(a,i){ if(_isV(a)) return a[i|0]??0; return i===0?(a??0):0; }
function _aset(env,name,i,v){ let a=env[name]; if(!Array.isArray(a)){ a=[]; env[name]=a; } a[i|0]=_isV(v)?v[0]:v; }
function _arr(n,t){ const o=new Array(n); for(let i=0;i<n;i++) o[i]=_dflt(t); return o; }
function _sunit(nm){ return /^tex[0-3]$/.test(nm) ? +nm[3] : 0; }
`;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
export interface GLSLUniform { loc: string; kind: number; v: number[]; }
// Per-pixel interpolated varyings keyed by declared name (from the vertex
// stage). Values are scalars or component arrays.
export type GLSLVaryRecord = Record<string, number | number[]>;
export interface GLSLProgram {
  run(vary: GLSLVaryings, tex: GLSLTexFn | null, vmap?: Map<string, string>, uniforms?: GLSLUniform[], vr?: GLSLVaryRecord): [number, number, number] | null;
}

// Parse a vertex shader's `varying` declarations to map each name to the
// semantic it receives. Falls back to name-based heuristics (c/t/p/n).
export function parseVaryingMap(vertSrc: string): Map<string, string> {
  const map = new Map<string, string>();
  if (!vertSrc) return map;
  const re = /varying\s+(?:vec[234]|float|bool)\s+([a-zA-Z_]\w*(?:\s*,\s*[a-zA-Z_]\w*)*)/g;
  const names: string[] = [];
  let m: RegExpExecArray | null;
  while ((m = re.exec(vertSrc)) !== null) {
    for (const part of m[1].split(',')) names.push(part.trim());
  }
  const assignRe = /([a-zA-Z_]\w*)\s*=\s*(gl_Color|gl_MultiTexCoord0|gl_Vertex|gl_Normal|gl_Position)/g;
  const semanticByAssign = new Map<string, string>();
  let am: RegExpExecArray | null;
  while ((am = assignRe.exec(vertSrc)) !== null) {
    const g = am[2];
    if (g.startsWith('gl_Color')) semanticByAssign.set(am[1], 'color');
    else if (g.startsWith('gl_MultiTexCoord0')) semanticByAssign.set(am[1], 'tex');
    else if (g.startsWith('gl_Vertex')) semanticByAssign.set(am[1], 'pos');
    else if (g.startsWith('gl_Normal')) semanticByAssign.set(am[1], 'nrm');
    else if (g.startsWith('gl_Position')) {
      // C's mvp_bake pre-divides vertices to NDC on the CPU ONLY when the
      // vertex shader does NOT reference gl_Vertex (polydraw.c:927). In that
      // mode gl_Position is already NDC (w=1), so p must bind to NDC. When the
      // shader DOES reference gl_Vertex, mvp_bake=0 and gl_Position is the
      // real clip-space output — p must keep the VR (clip-space) value.
      const mvpBake = vertSrc.indexOf('gl_Vertex') < 0;
      semanticByAssign.set(am[1], mvpBake ? 'ndc' : 'clip');
    }
  }
  for (const name of names) {
    const sem = semanticByAssign.get(name);
    if (sem) { map.set(name, sem); continue; }
    if (name === 'c') map.set(name, 'color');
    else if (name === 't') map.set(name, 'tex');
    else if (name === 'p') map.set(name, 'pos');
    else if (name === 'n') map.set(name, 'nrm');
  }
  return map;
}

const genCache = new Map<string, ((vary: GLSLVaryings, texFn: GLSLTexFn | null, vmap?: Map<string, string>, uniforms?: GLSLUniform[]) => [number, number, number] | null) | null>();

// Depth written by the last executed fragment via gl_FragDepth (null if the
// shader never touched it). Read by the soft rasterizer after each run().
let g_fragDepth: number | null = null;
export function lastFragDepth(): number | null { return g_fragDepth; }
export function resetFragDepth(): void { g_fragDepth = null; }

// Compile fragment shader source into an executable program. If `vmap` is
// provided (from the script's vertex shader), the varying bindings are baked
// into the generated JS as plain statements (no per-pixel loop / function
// creation), which matters a lot for heavy per-pixel shaders.
export function compileGLSL(src: string, vmap?: Map<string, string>): GLSLProgram | null {
  const cacheKey = vmap ? src + '\u0000' + JSON.stringify([...vmap]) : src;
  if (genCache.has(cacheKey)) {
    const cached = genCache.get(cacheKey)!;
    return cached ? { run: cached } : null;
  }
  try {
    const toks = tokenize(src);
    const parser = new GLSLParser(toks);
    const stmts = parser.parseProgram();
    // Generate the JS body.
    const body: string[] = [];
    // User-defined functions first (they reference env/texFn from the closure).
    for (const [fname, fd] of parser.fnDefs) {
      const params = fd.params.map((p) => `_a${p.name}`).join(',');
      const fbody: string[] = [];
      fbody.push(`var env = {};`);
      fd.params.forEach((p, i) => fbody.push(`env[${JSON.stringify(p.name)}]=_a${p.name};`));
      for (const st of fd.body) genStmt(st, fbody);
      body.push(`function _f_${fname}(${params}) { ${fbody.join('\n')} }`);
    }
    body.push(`var env = {};`);
    body.push(`var vary = ARGV;`);
    body.push(`env['gl_FragColor']=[0,0,0,0];`);
    body.push(`env['gl_FragDepth']=null;`);
    // Static varying bindings (baked at compile time when vmap is known).
    const bind = (name: string, expr: string): string => `env[${JSON.stringify(name)}]=${expr};`;
    body.push(bind('c', '[vary.r,vary.g,vary.b,vary.a]'));
    body.push(bind('t', '[vary.s,vary.t,vary.p,vary.q]'));
    body.push(bind('p', '[vary.px,vary.py,vary.pz,vary.pw]'));
    body.push(bind('n', '[vary.nx,vary.ny,vary.nz]'));
    body.push(bind('gl_Position', '[vary.ndx,vary.ndy,vary.ndz,vary.ndw]'));
    if (vmap) {
      for (const [name, key] of vmap) {
        if (name === 'c' || name === 't' || name === 'n') continue; // already bound
        const expr = key === 'color' ? '[vary.r,vary.g,vary.b,vary.a]'
          : key === 'tex' ? '[vary.s,vary.t,vary.p,vary.q]'
          : key === 'pos' ? '[vary.px,vary.py,vary.pz,vary.pw]'
          : key === 'nrm' ? '[vary.nx,vary.ny,vary.nz]'
          : key === 'ndc' ? '[vary.ndx,vary.ndy,vary.ndz,vary.ndw]'
          // 'clip': p = gl_Position with mvp_bake=0 — gl_Position is the real
          // clip-space output; VR already holds the correct interpolated value,
          // so emit no binding and let VR override p.
          : key === 'clip' ? null
          : null;
        if (expr) body.push(bind(name, expr));
      }
    }
    // Declared samplers resolve to their texture unit by NAME (tex0->0,
    // tex1->1, ...), mirroring polydraw.c's hard-coded glUniform1i mapping
    // at program link. Emitted before the uniforms loop so an explicit
    // glUniform1i from the script still overrides the default mapping.
    for (const sm of parser.uniSamplers) body.push(bind(sm, `_sunit(${JSON.stringify(sm)})`));
    body.push(`if (uniforms) for (var _u of uniforms) { var _uv = _u.v; env[_u.loc] = _uv.length === 1 ? _uv[0] : _uv; }`);
    // Vertex-stage varyings (interpolated per pixel by the rasterizer) take
    // precedence over the fixed semantic bindings above — they are the real
    // values produced by executing the script's vertex shader. EXCEPT for
    // varyings bound to 'ndc' (p = gl_Position): C's mvp_bake pre-divides
    // positions to NDC on the CPU, so the fragment shader sees NDC (w=1), not
    // the clip-space value the vertex shader wrote. The NDC binding above is
    // already correct and must not be overridden by VR.
    {
      const skip = vmap ? [...vmap.entries()].filter(([, k]) => k === 'ndc').map(([n]) => n) : [];
      if (skip.length) {
        body.push(`if (VR) for (var _vk in VR) { if (${JSON.stringify(skip)}.indexOf(_vk) < 0) env[_vk] = VR[_vk]; }`);
      } else {
        body.push(`if (VR) for (var _vk in VR) env[_vk] = VR[_vk];`);
      }
    }
    body.push(bind('gl_Color', '[vary.r,vary.g,vary.b,vary.a]'));
    body.push(bind('gl_Vertex', '[vary.px,vary.py,vary.pz,vary.pw]'));
    body.push(bind('gl_MultiTexCoord0', '[vary.s,vary.t,vary.p,1]'));
    // unit-0 fallbacks for shaders that sample an UNDECLARED tex0/tex
    if (!parser.uniSamplers.includes('tex0')) body.push(bind('tex0', '0'));
    if (!parser.uniSamplers.includes('tex')) body.push(bind('tex', '0'));
    for (const st of stmts) genStmt(st, body);
    body.push(`var _o = env['gl_FragColor']; var _r = [_o[0],_o[1],_o[2]]; if (env['gl_FragDepth'] !== null) _r.fd = _sc(env['gl_FragDepth']); return _r;`);
    if (process.env.PD_DUMP_BODY) console.error('[glsl] BODY:\n' + body.join('\n'));
    // RUNTIME defines the helpers inside the compiled fn (once per program,
    // zero per-pixel cost), so the generated code references them directly.
    const fnSrc = `return function(ARGV, texFn, vmap, uniforms, VR) {\n${RUNTIME}\n${body.join('\n')}\n};`;
    const factory = new Function(fnSrc) as () => (vary: GLSLVaryings, texFn: GLSLTexFn | null, vmap?: Map<string, string>, uniforms?: GLSLUniform[], vr?: GLSLVaryRecord) => [number, number, number] | null;
    const fn = factory();
    genCache.set(cacheKey, fn);
    return {
      run(vary: GLSLVaryings, tex: GLSLTexFn | null, vmap?: Map<string, string>, uniforms?: GLSLUniform[], vr?: GLSLVaryRecord): [number, number, number] | null {
        const out = fn(vary, tex, vmap, uniforms, vr);
        if (!out) { g_fragDepth = null; return null; }
        g_fragDepth = (out as { fd?: number }).fd ?? null;
        if (process.env.PD_DEBUG_NAN && (Number.isNaN(out[0]) || Number.isNaN(out[1]) || Number.isNaN(out[2]))) {
          if (!(globalThis as Record<string, unknown>).__nanDumped) {
            (globalThis as Record<string, unknown>).__nanDumped = 1;
            // eslint-disable-next-line no-console
            console.error('[nan] DUMP ' + JSON.stringify({ src: cacheKey.split('\u0000')[0], vmap: vmap ? [...vmap] : null, vr, uniforms }));
          }
          console.error(`[nan] out=${JSON.stringify(out)} vr=${JSON.stringify(vr)}`);
        }
        return [Math.min(1, Math.max(0, out[0])), Math.min(1, Math.max(0, out[1])), Math.min(1, Math.max(0, out[2]))];
      },
    };
  } catch (e) {
    if (process.env.PD_DEBUG_GLSL) console.error('[glsl] compile fail:', (e as Error).stack ?? String(e));
    genCache.set(cacheKey, null);
    return null;
  }
}

// ---------------------------------------------------------------------------
// Vertex shader stage
// ---------------------------------------------------------------------------
// Mirrors C adapt_vertex: gl_Vertex/gl_Color/gl_MultiTexCoord0/gl_Normal feed
// in as attributes, ftransform() = gl_ModelViewProjectionMatrix * gl_Vertex,
// and every `varying` the main() writes is exported by name. The rasterizer
// interpolates them perspective-correct and hands them to the fragment stage.
export interface GLSLVertexAttrs {
  vertex: number[];   // vec4 (x,y,z,w)
  color: number[];    // vec4
  texcoord: number[]; // vec4 (s,t,p,q)
  normal: number[];   // vec3
  mvp: number[];        // mat4, column-major (projection * modelview)
  modelview: number[];  // mat4
  projection: number[]; // mat4
  normalMat: number[];  // mat3 (upper-left 3x3 of modelview, for lighting)
}
export interface GLSLVertexResult {
  pos: number[];                     // gl_Position (clip space)
  vary: GLSLVaryRecord;              // declared varyings, by name
  pointSize: number | null;          // gl_PointSize if written
}
export type GLSLVertexRunner = (attrs: GLSLVertexAttrs, uniforms?: GLSLUniform[]) => GLSLVertexResult | null;

const vtxCache = new Map<string, GLSLVertexRunner | null>();

export function compileVertexGLSL(src: string): GLSLVertexRunner | null {
  if (vtxCache.has(src)) return vtxCache.get(src)!;
  let runner: GLSLVertexRunner | null = null;
  try {
    const toks = tokenize(src);
    const parser = new GLSLParser(toks);
    const stmts = parser.parseProgram();
    const body: string[] = [];
    for (const [fname, fd] of parser.fnDefs) {
      const params = fd.params.map((p) => `_a${p.name}`).join(',');
      const fbody: string[] = [];
      fbody.push(`var env = {};`);
      fd.params.forEach((p) => fbody.push(`env[${JSON.stringify(p.name)}]=_a${p.name};`));
      for (const st of fd.body) genStmt(st, fbody);
      body.push(`function _f_${fname}(${params}) { ${fbody.join('\n')} }`);
    }
    body.push(`var env = {};`);
    body.push(`var ATTR = ARGV;`);
    body.push(`env['gl_Vertex']=ATTR.vertex;`);
    body.push(`env['gl_Color']=ATTR.color;`);
    body.push(`env['gl_MultiTexCoord0']=ATTR.texcoord;`);
    body.push(`env['gl_Normal']=ATTR.normal;`);
    body.push(`env['gl_ModelViewProjectionMatrix']=ATTR.mvp;`);
    body.push(`env['gl_ModelViewMatrix']=ATTR.modelview;`);
    body.push(`env['gl_ProjectionMatrix']=ATTR.projection;`);
    body.push(`env['gl_NormalMatrix']=ATTR.normalMat;`);
    body.push(`env['gl_Position']=[0,0,0,1];`);
    body.push(`env['gl_PointSize']=null;`);
    body.push(`if (uniforms) for (var _u of uniforms) { var _uv = _u.v; env[_u.loc] = _uv.length === 1 ? _uv[0] : _uv; }`);
    for (const st of stmts) genStmt(st, body);
    // export gl_Position + every declared varying that main() assigned
    body.push(`var _pos = env['gl_Position']; if (!_isV(_pos)) _pos=[_pos,0,0,1];`);
    body.push(`var _vary = {};`);
    for (const nm of parser.varyNames) body.push(`if (env[${JSON.stringify(nm)}] !== undefined) _vary[${JSON.stringify(nm)}]=env[${JSON.stringify(nm)}];`);
    body.push(`return { pos: _pos, vary: _vary, pointSize: env['gl_PointSize'] };`);
    const fnSrc = `return function(ARGV, uniforms) {\n${RUNTIME}\n${body.join('\n')}\n};`;
    const factory = new Function(fnSrc) as () => (attrs: GLSLVertexAttrs, uniforms?: GLSLUniform[]) => GLSLVertexResult | null;
    runner = factory();
  } catch (e) {
    if (process.env.PD_DEBUG_GLSL) console.error('[glsl-vs] compile fail:', (e as Error).stack ?? String(e));
    runner = null;
  }
  vtxCache.set(src, runner);
  return runner;
}
