const KEYWORDS = new Set([
  "if", "else", "do", "while", "for", "goto", "return", "break", "continue", "enum", "static",
]);

const TWO_CHAR = new Set(["*=", "/=", "%=", "+=", "-=", "++", "--", "<=", ">=", "==", "!=", "&&", "||"]);
const ONE_CHAR = new Set(["=", "^", "*", "/", "%", "+", "-", "<", ">", "!", "&", "$", "(", ")", "[", "]", "{", "}", ",", ";", ":"]);

export class LexerError extends Error {
  constructor(message, line, column) {
    super(`${line}:${column}: ${message}`);
    this.line = line;
    this.column = column;
  }
}

export function tokenize(source) {
  const tokens = [];
  let i = 0;
  let line = 1;
  let column = 1;

  function push(type, value, start, startLine, startColumn) {
    tokens.push({ type, value, start, end: i, line: startLine, column: startColumn });
  }

  function advance(n = 1) {
    while (n-- > 0) {
      if (source[i] === "\n") {
        line++;
        column = 1;
      } else {
        column++;
      }
      i++;
    }
  }

  while (i < source.length) {
    const ch = source[i];
    const start = i;
    const startLine = line;
    const startColumn = column;

    if (/\s/.test(ch)) {
      advance();
      continue;
    }

    if (ch === "/" && source[i + 1] === "/") {
      while (i < source.length && source[i] !== "\n") advance();
      continue;
    }

    if (ch === "/" && source[i + 1] === "*") {
      advance(2);
      while (i < source.length && !(source[i] === "*" && source[i + 1] === "/")) advance();
      if (i >= source.length) throw new LexerError("unterminated block comment", startLine, startColumn);
      advance(2);
      continue;
    }

    if (/[A-Za-z_]/.test(ch)) {
      advance();
      while (i < source.length && /[A-Za-z0-9_]/.test(source[i])) advance();
      const raw = source.slice(start, i);
      const lower = raw.toLowerCase();
      push(KEYWORDS.has(lower) ? "keyword" : "identifier", raw, start, startLine, startColumn);
      continue;
    }

    if (/[0-9.]/.test(ch) && (ch !== "." || /[0-9]/.test(source[i + 1]))) {
      if (ch === "0" && /[xX]/.test(source[i + 1])) {
        advance(2);
        while (i < source.length && /[0-9A-Fa-f]/.test(source[i])) advance();
      } else {
        advance();
        while (i < source.length && /[0-9]/.test(source[i])) advance();
        if (source[i] === ".") {
          advance();
          while (i < source.length && /[0-9]/.test(source[i])) advance();
        }
        if (/[eE]/.test(source[i])) {
          advance();
          if (/[+-]/.test(source[i])) advance();
          while (i < source.length && /[0-9]/.test(source[i])) advance();
        }
      }
      push("number", source.slice(start, i), start, startLine, startColumn);
      continue;
    }

    if (ch === "\"") {
      advance();
      let value = "";
      while (i < source.length && source[i] !== "\"") {
        if (source[i] === "\\") {
          advance();
          const esc = source[i];
          const map = { n: "\n", r: "\r", t: "\t", "\"": "\"", "\\": "\\" };
          value += map[esc] ?? esc;
          advance();
        } else {
          value += source[i];
          advance();
        }
      }
      if (source[i] !== "\"") throw new LexerError("unterminated string", startLine, startColumn);
      advance();
      tokens.push({ type: "string", value, raw: source.slice(start, i), start, end: i, line: startLine, column: startColumn });
      continue;
    }

    const two = source.slice(i, i + 2);
    if (TWO_CHAR.has(two)) {
      advance(2);
      push("operator", two, start, startLine, startColumn);
      continue;
    }

    if (ONE_CHAR.has(ch)) {
      advance();
      push("operator", ch, start, startLine, startColumn);
      continue;
    }

    throw new LexerError(`unexpected character '${ch}'`, line, column);
  }

  tokens.push({ type: "eof", value: "", start: i, end: i, line, column });
  return tokens;
}
