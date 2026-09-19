import { tokenize } from "./lexer.js";

const PRECEDENCE = new Map([
  ["=", 1], ["*=", 1], ["/=", 1], ["%=", 1], ["+=", 1], ["-=", 1],
  ["||", 2],
  ["&&", 3],
  ["==", 4], ["!=", 4],
  ["<", 5], ["<=", 5], [">", 5], [">=", 5],
  ["+", 6], ["-", 6],
  ["*", 7], ["/", 7], ["%", 7],
  ["^", 8],
]);

const RIGHT_ASSOC = new Set(["=", "*=", "/=", "%=", "+=", "-=", "^"]);
const ASSIGNMENT_OPS = new Set(["=", "*=", "/=", "%=", "+=", "-="]);

export class ParseError extends Error {
  constructor(token, message) {
    super(`${token.line}:${token.column}: ${message}`);
    this.token = token;
  }
}

export function parseProgram(source) {
  return new Parser(tokenize(source)).parseProgram();
}

class Parser {
  constructor(tokens) {
    this.tokens = tokens;
    this.pos = 0;
  }

  current() {
    return this.tokens[this.pos];
  }

  next() {
    return this.tokens[this.pos++];
  }

  is(value) {
    return this.current().value.toLowerCase() === value.toLowerCase();
  }

  match(value) {
    if (this.is(value)) {
      return this.next();
    }
    return null;
  }

  matchKeyword(value) {
    const token = this.current();
    if (token.type === "keyword" && token.value.toLowerCase() === value) {
      return this.next();
    }
    return null;
  }

  expect(value) {
    const token = this.current();
    if (!this.match(value)) throw new ParseError(token, `expected '${value}'`);
  }

  expectIdentifier() {
    const token = this.current();
    if (token.type !== "identifier") throw new ParseError(token, "expected identifier");
    return this.next();
  }

  parseProgram() {
    const body = [];
    while (this.current().type !== "eof") {
      if (this.looksLikeFunction()) body.push(this.parseFunctionDecl());
      else body.push(this.parseStatement());
    }
    return { kind: "Program", body };
  }

  looksLikeFunction() {
    let p = this.pos;
    if (this.tokens[p].type === "identifier") p++;
    if (this.tokens[p]?.value !== "(") return false;
    let depth = 0;
    while (p < this.tokens.length) {
      if (this.tokens[p].value === "(") depth++;
      if (this.tokens[p].value === ")") {
        depth--;
        if (depth === 0) break;
      }
      p++;
    }
    const after = this.tokens[p + 1];
    return after?.value === "{" && (this.tokens[this.pos].type === "identifier" || this.tokens[this.pos].value === "(");
  }

  parseFunctionDecl() {
    let name = "";
    if (this.current().type === "identifier") name = this.next().value;
    this.expect("(");
    const params = [];
    if (!this.is(")")) {
      do {
        let mode = "value";
        if (this.match("&")) mode = "ref";
        else if (this.match("$")) mode = "string";
        const id = this.expectIdentifier().value;
        const dims = [];
        while (this.match("[")) {
          if (!this.is("]")) dims.push(this.parseExpression());
          this.expect("]");
        }
        params.push({ name: id, mode, dims });
      } while (this.match(","));
    }
    this.expect(")");
    const body = this.parseBlock();
    return { kind: "FunctionDecl", name, params, body };
  }

  parseStatement() {
    if (this.match("{")) return this.parseBlockAfterOpen();
    if (this.matchKeyword("if")) return this.parseIf();
    if (this.matchKeyword("while")) return this.parseWhile();
    if (this.matchKeyword("do")) return this.parseDo();
    if (this.matchKeyword("for")) return this.parseFor();
    if (this.matchKeyword("goto")) return this.parseGoto();
    if (this.matchKeyword("return")) return this.parseReturn();
    if (this.matchKeyword("break")) { this.match(";"); return { kind: "Break" }; }
    if (this.matchKeyword("continue")) { this.match(";"); return { kind: "Continue" }; }
    if (this.matchKeyword("enum")) return this.parseEnum();
    if (this.matchKeyword("static")) return this.parseStatic();
    if (this.current().type === "identifier" && this.tokens[this.pos + 1]?.value === ":") {
      const name = this.next().value;
      this.expect(":");
      return { kind: "Label", name };
    }
    if (this.match(";")) return { kind: "Empty" };
    const expr = this.parseExpressionList();
    this.match(";");
    return { kind: "ExprStmt", expr };
  }

  parseBlock() {
    this.expect("{");
    return this.parseBlockAfterOpen();
  }

  parseBlockAfterOpen() {
    const body = [];
    while (!this.is("}") && this.current().type !== "eof") body.push(this.parseStatement());
    this.expect("}");
    return { kind: "Block", body };
  }

  parseIf() {
    this.expect("(");
    const test = this.parseExpression();
    this.expect(")");
    const consequent = this.parseStatement();
    const alternate = this.matchKeyword("else") ? this.parseStatement() : null;
    return { kind: "If", test, consequent, alternate };
  }

  parseWhile() {
    this.expect("(");
    const test = this.parseExpression();
    this.expect(")");
    return { kind: "While", test, body: this.parseStatement() };
  }

  parseDo() {
    const body = this.parseStatement();
    if (!this.matchKeyword("while")) throw new ParseError(this.current(), "expected while after do body");
    this.expect("(");
    const test = this.parseExpression();
    this.expect(")");
    this.match(";");
    return { kind: "DoWhile", body, test };
  }

  parseFor() {
    this.expect("(");
    const init = this.is(";") ? null : this.parseExpressionList();
    this.expect(";");
    const test = this.is(";") ? null : this.parseExpression();
    this.expect(";");
    const update = this.is(")") ? null : this.parseExpressionList();
    this.expect(")");
    return { kind: "For", init, test, update, body: this.parseStatement() };
  }

  parseGoto() {
    const name = this.expectIdentifier().value;
    this.match(";");
    return { kind: "Goto", name };
  }

  parseReturn() {
    const expr = this.is(";") || this.is("}") ? null : this.parseExpression();
    this.match(";");
    return { kind: "Return", expr };
  }

  parseEnum() {
    this.expect("{");
    const items = [];
    while (!this.is("}")) {
      const name = this.expectIdentifier().value;
      const value = this.match("=") ? this.parseExpression() : null;
      items.push({ name, value });
      if (!this.match(",")) break;
    }
    this.expect("}");
    this.match(";");
    return { kind: "Enum", items };
  }

  parseStatic() {
    const items = [];
    do {
      const name = this.expectIdentifier().value;
      const dims = [];
      while (this.match("[")) {
        dims.push(this.parseExpression());
        this.expect("]");
      }
      let init = null;
      if (this.match("=")) init = this.match("{") ? this.parseInitializerList() : this.parseExpression();
      items.push({ name, dims, init });
    } while (this.match(","));
    this.match(";");
    return { kind: "Static", items };
  }

  parseInitializerList() {
    const values = [];
    while (!this.is("}")) {
      values.push(this.match("{") ? this.parseInitializerList() : this.parseExpression());
      if (!this.match(",")) break;
    }
    this.expect("}");
    return { kind: "InitList", values };
  }

  parseExpressionList() {
    const expressions = [this.parseExpression()];
    while (this.match(",")) expressions.push(this.parseExpression());
    return expressions.length === 1 ? expressions[0] : { kind: "Sequence", expressions };
  }

  parseExpression(minPrec = 1) {
    let left = this.parsePrefix();
    while (true) {
      left = this.parsePostfix(left);
      const op = this.current().value;
      const prec = PRECEDENCE.get(op);
      if (!prec || prec < minPrec) break;
      this.next();
      const rhsMin = RIGHT_ASSOC.has(op) ? prec : prec + 1;
      const right = this.parseExpression(rhsMin);
      left = { kind: ASSIGNMENT_OPS.has(op) ? "Assign" : "Binary", op, left, right };
    }
    return left;
  }

  parsePrefix() {
    const token = this.current();
    if (["+", "-", "!", "++", "--", "&"].includes(token.value)) {
      this.next();
      if (token.value === "&") return { kind: "RefArg", expr: this.parseExpression(9) };
      return { kind: "Unary", op: token.value, expr: this.parseExpression(9), prefix: true };
    }
    return this.parsePrimary();
  }

  parsePostfix(expr) {
    while (true) {
      if (this.match("(")) {
        const args = [];
        if (!this.is(")")) {
          do args.push(this.parseExpression());
          while (this.match(","));
        }
        this.expect(")");
        expr = { kind: "Call", callee: expr, args };
      } else if (this.match("[")) {
        const index = this.parseExpression();
        this.expect("]");
        expr = { kind: "Index", object: expr, index };
      } else if (this.is("++") || this.is("--")) {
        const op = this.next().value;
        expr = { kind: "Unary", op, expr, prefix: false };
      } else {
        break;
      }
    }
    return expr;
  }

  parsePrimary() {
    const token = this.current();
    if (token.type === "number") {
      this.next();
      return { kind: "Number", value: token.value.startsWith("0x") || token.value.startsWith("0X") ? Number.parseInt(token.value, 16) : Number(token.value) };
    }
    if (token.type === "string") {
      this.next();
      return { kind: "String", value: token.value };
    }
    if (token.type === "identifier") {
      this.next();
      return { kind: "Identifier", name: token.value };
    }
    if (this.match("(")) {
      const expr = this.parseExpressionList();
      this.expect(")");
      return expr;
    }
    throw new ParseError(token, "expected expression");
  }
}
