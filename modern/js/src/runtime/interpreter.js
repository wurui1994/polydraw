import { parseProgram } from "../parser/parser.js";
import { canonical, Cell, PdArray, TraceRuntime } from "./runtime.js";

class Signal {
  constructor(type, value = undefined) {
    this.type = type;
    this.value = value;
  }
}

export function compileHost(source) {
  const ast = parseProgram(source);
  return new Program(ast);
}

export function runHost(source, runtime = new TraceRuntime()) {
  const program = compileHost(source);
  return program.run(runtime);
}

class Program {
  constructor(ast) {
    this.ast = ast;
    this.functions = new Map();
    this.main = [];
    this.statics = new Map();
    this.enums = new Map();
    this.globalsInitialized = false;
    for (const node of ast.body) {
      if (node.kind === "FunctionDecl") {
        const name = canonical(node.name || "__main");
        this.functions.set(name, node);
      } else {
        this.main.push(node);
      }
    }
  }

  run(runtime = new TraceRuntime(), options = {}) {
    const ctx = new Context(this, runtime, null);
    ctx.stepLimit = options.stepLimit ?? 5_000_000;
    if (!this.globalsInitialized) {
      for (const node of this.main) {
        if (node.kind === "Static" || node.kind === "Enum") ctx.exec(node);
      }
      this.globalsInitialized = true;
    }
    const body = this.functions.get("__MAIN")?.body.body ?? this.main;
    if (this.functions.has("__MAIN")) ctx.staticScope = "__MAIN";
    const result = ctx.execLinear(body);
    const value = result instanceof Signal && result.type === "return" ? result.value ?? 0 : 0;
    const nf = runtime.getExternal("NUMFRAMES");
    if (nf instanceof Cell) nf.value++;
    return { value, runtime, program: this };
  }
}

class Context {
  constructor(program, runtime, parent, staticScope = "__GLOBAL") {
    this.program = program;
    this.runtime = runtime;
    this.parent = parent;
    this.staticScope = staticScope;
    this.locals = new Map();
    this.enums = parent?.enums ?? program.enums;
    this.stepLimit = parent?.stepLimit ?? 5_000_000;
    this.steps = parent?.steps ?? { count: 0 };
  }

  child() {
    return new Context(this.program, this.runtime, this, this.staticScope);
  }

  execLinear(statements) {
    const labels = new Map();
    for (let i = 0; i < statements.length; i++) {
      if (statements[i].kind === "Label") labels.set(canonical(statements[i].name), i);
    }
    for (let ip = 0; ip < statements.length; ip++) {
      const result = this.exec(statements[ip]);
      if (result instanceof Signal) {
        if (result.type === "goto") {
          const target = labels.get(canonical(result.value));
          if (target === undefined) return result;
          ip = target;
          continue;
        }
        return result;
      }
    }
    return null;
  }

  exec(node) {
    this.tick();
    switch (node.kind) {
      case "Empty":
      case "Label":
        return null;
      case "Block":
        return this.execLinear(node.body);
      case "ExprStmt":
        this.eval(node.expr);
        return null;
      case "If":
        return truthy(this.eval(node.test)) ? this.exec(node.consequent) : node.alternate ? this.exec(node.alternate) : null;
      case "While":
        while (truthy(this.eval(node.test))) {
          const r = this.exec(node.body);
          if (r instanceof Signal) {
            if (r.type === "break") return null;
            if (r.type === "continue") continue;
            return r;
          }
        }
        return null;
      case "DoWhile":
        do {
          const r = this.exec(node.body);
          if (r instanceof Signal) {
            if (r.type === "break") return null;
            if (r.type !== "continue") return r;
          }
        } while (truthy(this.eval(node.test)));
        return null;
      case "For":
        if (node.init) this.eval(node.init);
        while (!node.test || truthy(this.eval(node.test))) {
          const r = this.exec(node.body);
          if (r instanceof Signal) {
            if (r.type === "break") return null;
            if (r.type !== "continue") return r;
          }
          if (node.update) this.eval(node.update);
        }
        return null;
      case "Goto":
        return new Signal("goto", node.name);
      case "Break":
        return new Signal("break");
      case "Continue":
        return new Signal("continue");
      case "Return":
        return new Signal("return", node.expr ? this.eval(node.expr) : 0);
      case "Enum":
        this.execEnum(node);
        return null;
      case "Static":
        this.execStatic(node);
        return null;
      default:
        throw new Error(`unsupported statement ${node.kind}`);
    }
  }

  tick() {
    this.steps.count++;
    if (this.steps.count > this.stepLimit) throw new Error(`execution step limit exceeded (${this.stepLimit})`);
  }

  execEnum(node) {
    let value = 0;
    for (const item of node.items) {
      if (item.value) value = this.eval(item.value);
      this.enums.set(canonical(item.name), value);
      value++;
    }
  }

  execStatic(node) {
    for (const item of node.items) {
      const key = this.staticKey(item.name);
      if (this.program.statics.has(key)) continue;
      const dims = item.dims.map((expr) => this.eval(expr));
      const init = flattenInit(item.init, this);
      this.program.statics.set(key, dims.length ? new PdArray(dims, init) : new Cell(init[0] ?? 0));
    }
  }

  eval(node) {
    switch (node.kind) {
      case "Number":
      case "String":
        return node.value;
      case "Identifier":
        return this.resolveValue(node.name);
      case "Sequence": {
        let value = 0;
        for (const expr of node.expressions) value = this.eval(expr);
        return value;
      }
      case "Unary":
        return this.evalUnary(node);
      case "Binary":
        return this.evalBinary(node);
      case "Assign":
        return this.evalAssign(node);
      case "Index": {
        const ref = this.ref(node);
        return ref.get();
      }
      case "RefArg":
        return this.ref(node.expr).get();
      case "Call":
        return this.evalCall(node);
      default:
        throw new Error(`unsupported expression ${node.kind}`);
    }
  }

  evalUnary(node) {
    if (node.op === "++" || node.op === "--") {
      const ref = this.ref(node.expr);
      const old = Number(ref.get());
      const next = old + (node.op === "++" ? 1 : -1);
      ref.set(next);
      return node.prefix ? next : old;
    }
    const v = Number(this.eval(node.expr));
    if (node.op === "+") return +v;
    if (node.op === "-") return -v;
    if (node.op === "!") return truthy(v) ? 0 : 1;
    throw new Error(`unknown unary ${node.op}`);
  }

  evalBinary(node) {
    if (node.op === "&&") {
      return truthy(this.eval(node.left)) && truthy(this.eval(node.right)) ? 1 : 0;
    }
    if (node.op === "||") {
      return truthy(this.eval(node.left)) || truthy(this.eval(node.right)) ? 1 : 0;
    }
    const a = Number(this.eval(node.left));
    const b = Number(this.eval(node.right));
    switch (node.op) {
      case "^": return Math.pow(a, b);
      case "*": return a * b;
      case "/": return a / b;
      case "%": return a % b;
      case "+": return a + b;
      case "-": return a - b;
      case "<": return a < b ? 1 : 0;
      case "<=": return a <= b ? 1 : 0;
      case ">": return a > b ? 1 : 0;
      case ">=": return a >= b ? 1 : 0;
      case "==": return a === b ? 1 : 0;
      case "!=": return a !== b ? 1 : 0;
      default: throw new Error(`unknown binary ${node.op}`);
    }
  }

  evalAssign(node) {
    if (node.op === "=") {
      const right = Number(this.eval(node.right));
      return this.assignRef(node.left).set(right);
    }
    const ref = this.assignRef(node.left);
    const right = Number(this.eval(node.right));
    const left = Number(ref.get());
    const value = node.op === "+=" ? left + right
      : node.op === "-=" ? left - right
      : node.op === "*=" ? left * right
      : node.op === "/=" ? left / right
      : node.op === "%=" ? left % right
      : (() => { throw new Error(`unknown assignment ${node.op}`); })();
    return ref.set(value);
  }

  evalCall(node) {
    if (node.callee.kind !== "Identifier") throw new Error("call target must be identifier");
    const name = node.callee.name;
    const key = canonical(name);
    const fn = this.program.functions.get(key);
    if (fn) return this.callUser(fn, node.args);
    const args = node.args.map((arg) => this.eval(arg));
    if (BUILTINS.has(key)) return BUILTINS.get(key)(args);
    const externalArgs = node.args.map((arg, index) => {
      if (arg.kind === "Identifier") {
        const cell = this.resolveCell(arg.name);
        if (cell instanceof PdArray) return cell;
      }
      return args[index];
    });
    return this.runtime.callExternal(name, externalArgs);
  }

  callUser(fn, argNodes) {
    const child = new Context(this.program, this.runtime, this, canonical(fn.name || "__MAIN"));
    fn.params.forEach((param, index) => {
      const argNode = argNodes[index];
      if (param.mode === "ref") {
        const arg = argNodes[index]?.kind === "RefArg" ? argNodes[index].expr : argNodes[index];
        child.locals.set(canonical(param.name), this.ref(arg));
      } else if (param.dims?.length && argNode?.kind === "Identifier") {
        const cell = this.resolveCell(argNode.name);
        if (!(cell instanceof PdArray)) throw new Error(`${argNode.name} is not an array`);
        child.locals.set(canonical(param.name), cell);
      } else if (argNode?.kind === "Identifier" && this.resolveCell(argNode.name) instanceof PdArray) {
        child.locals.set(canonical(param.name), this.resolveCell(argNode.name));
      } else {
        child.locals.set(canonical(param.name), new Cell(Number(argNode ? this.eval(argNode) : 0)));
      }
    });
    const r = child.execLinear(fn.body.body);
    return r instanceof Signal && r.type === "return" ? Number(r.value ?? 0) : 0;
  }

  resolveCell(name) {
    const key = canonical(name);
    if (this.locals.has(key)) return this.locals.get(key);
    const scopedStatic = this.staticKey(name);
    if (this.program.statics.has(scopedStatic)) return this.program.statics.get(scopedStatic);
    if (this.program.statics.has(key)) return this.program.statics.get(key);
    if (this.enums.has(key)) return new Cell(this.enums.get(key));
    const ext = this.runtime.getExternal(name);
    if (ext) return ext;
    const cell = new Cell(0);
    this.locals.set(key, cell);
    return cell;
  }

  staticKey(name) {
    const key = canonical(name);
    return this.staticScope === "__GLOBAL" ? key : `${this.staticScope}::${key}`;
  }

  resolveValue(name) {
    const key = canonical(name);
    if (key === "PI") return Math.PI;
    if (key === "RND") return Math.random();
    if (key === "NRND") return normalRandom();
    const cell = this.resolveCell(name);
    if (cell instanceof PdArray) return cell.get([0]);
    if (typeof cell.get === "function") return cell.get();
    return cell.value;
  }

  ref(node) {
    if (node.kind === "Identifier") {
      const cell = this.resolveCell(node.name);
      if (cell instanceof PdArray) return { get: () => cell.get([0]), set: (v) => cell.set([0], v) };
      if (typeof cell.get === "function" && typeof cell.set === "function") return cell;
      return { get: () => cell.value, set: (v) => (cell.value = Number(v)) };
    }
    if (node.kind === "Index") {
      const { array, indices } = this.arrayRef(node);
      return { get: () => array.get(indices), set: (v) => array.set(indices, v) };
    }
    throw new Error("expression is not assignable");
  }

  assignRef(node) {
    if (node.kind === "Identifier") {
      const key = canonical(node.name);
      if (this.locals.has(key)) return this.ref(node);
      const scopedStatic = this.staticKey(node.name);
      if (this.program.statics.has(scopedStatic) || this.program.statics.has(key) || this.runtime.getExternal(node.name)) {
        return this.ref(node);
      }
      const cell = new Cell(0);
      this.locals.set(key, cell);
      return { get: () => cell.value, set: (v) => (cell.value = Number(v)) };
    }
    return this.ref(node);
  }

  arrayRef(node) {
    const indices = [];
    let cur = node;
    while (cur.kind === "Index") {
      indices.unshift(this.eval(cur.index));
      cur = cur.object;
    }
    if (cur.kind !== "Identifier") throw new Error("array base must be identifier");
    const cell = this.resolveCell(cur.name);
    if (!(cell instanceof PdArray)) throw new Error(`${cur.name} is not an array`);
    return { array: cell, indices };
  }
}

const BUILTINS = new Map(Object.entries({
  ABS: ([x]) => Math.abs(x),
  FABS: ([x]) => Math.abs(x),
  ACOS: ([x]) => Math.acos(x),
  ASIN: ([x]) => Math.asin(x),
  ATAN: ([x]) => Math.atan(x),
  ATN: ([x]) => Math.atan(x),
  CEIL: ([x]) => Math.ceil(x),
  COS: ([x]) => Math.cos(x),
  EXP: ([x]) => Math.exp(x),
  FLOOR: ([x]) => Math.floor(x),
  INT: ([x]) => Math.trunc(x),
  LOG: (args) => args.length > 1 ? Math.log(args[0]) / Math.log(args[1]) : Math.log(args[0]),
  SGN: ([x]) => x < 0 ? -1 : x > 0 ? 1 : 0,
  SIN: ([x]) => Math.sin(x),
  SQR: ([x]) => Math.sqrt(x),
  SQRT: ([x]) => Math.sqrt(x),
  TAN: ([x]) => Math.tan(x),
  UNIT: ([x]) => x < 0 ? 0 : x > 0 ? 1 : 0.5,
  ATAN2: ([y, x]) => Math.atan2(y, x),
  FMOD: ([x, y]) => x % y,
  MIN: ([x, y]) => Math.min(x, y),
  MAX: ([x, y]) => Math.max(x, y),
  POW: ([x, y]) => Math.pow(x, y),
  FACT: ([x]) => gamma(x + 1),
}));

function flattenInit(init, ctx) {
  if (!init) return [];
  if (init.kind === "InitList") return init.values.flatMap((v) => flattenInit(v, ctx));
  return [ctx.eval(init)];
}

function truthy(value) {
  return Number(value) !== 0;
}

function normalRandom() {
  const u = 1 - Math.random();
  const v = Math.random();
  return Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v);
}

function gamma(z) {
  if (z <= 0) return NaN;
  if (Math.abs(z - Math.round(z)) < 1e-9) {
    let r = 1;
    for (let i = 2; i < z; i++) r *= i;
    return r;
  }
  return Math.exp((z - 0.5) * Math.log(z) - z + 0.5 * Math.log(2 * Math.PI));
}
