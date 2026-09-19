# Host Interpreter Plan

## Scope

The interpreter implements the EVAL host language used by PolyDraw. It is the reference executor for both JavaScript and C and the semantic source for LLVM JIT.

## Language Model

Values:

- `double` numbers for all numeric expressions.
- Direct string literals passed to external functions.
- References to variables, array elements, static storage, and external variables.
- Function references only for EVAL-style function pointer parameters if needed later.

Names:

- Case-insensitive canonicalization.
- Preserve original spelling for diagnostics.
- Separate namespaces for labels, variables/statics, functions, enum constants, and externals.

Functions:

- Main host function may be bare statements or `(){...}`.
- Additional named functions are supported.
- Last expression returns a value when no explicit `return` is used.
- Parameters support value, reference, array, and string forms as required by observed scripts.

## Lexer

Tokens:

- Identifiers, numbers, strings.
- Keywords: `if`, `else`, `do`, `while`, `for`, `goto`, `return`, `break`, `continue`, `enum`, `static`.
- Operators: `=`, `*=`, `/=`, `%=`, `+=`, `-=`, `++`, `--`, `^`, `*`, `/`, `%`, `+`, `-`, `<`, `<=`, `>`, `>=`, `==`, `!=`, `&&`, `||`, `!`.
- Punctuation: `(`, `)`, `[`, `]`, `{`, `}`, `,`, `;`, `:`.
- Comments: `//` and `/* ... */`.

Track byte offset, line, and column for every token.

## Parser

Use recursive descent for declarations/statements and Pratt parsing for expressions.

Precedence from high to low:

1. Primary: literals, identifiers, calls, arrays, parenthesized expressions.
2. Prefix: unary `+`, `-`, `!`, pre `++`, pre `--`.
3. Postfix: array indexing, calls, post `++`, post `--`.
4. Power `^`.
5. `*`, `/`, `%`.
6. `+`, `-`.
7. `<`, `<=`, `>`, `>=`.
8. `==`, `!=`.
9. `&&`.
10. `||`.
11. Assignments.

Note: verify associativity of `^` and assignments against EVAL behavior during conformance.

Statements:

- Expression statement.
- Block statement.
- `if` / `else`.
- `while`, `do while`, `for`.
- `break`, `continue`, `return`.
- `goto label;` and `label:`.
- `enum { ... };`.
- `static` declaration with arrays and optional initializer.

## Semantic Analysis

Analysis phases:

1. Register functions and parameters.
2. Register statics and enum constants.
3. Resolve locals and implicit variables.
4. Resolve labels and control-flow targets.
5. Resolve external variables/functions by signature.
6. Assign storage slots.
7. Validate lvalues and call arguments.

External signature matching:

- Store externals in canonical uppercase form.
- Include arity and parameter kinds in the signature.
- Support overloads by arity and string/reference markers.
- Keep friendly error messages listing candidate signatures.

Arrays:

- Flatten row-major storage.
- Store dimension sizes.
- Index expression results are converted toward zero.
- Bounds behavior:
  - Power-of-two dimension: mask index.
  - Non-power-of-two dimension: out-of-range index becomes zero.

## IR and Bytecode

Keep IR simple and debuggable:

- Constants and variable loads/stores.
- Array element address/load/store.
- Unary/binary operations.
- Calls to internal and external functions.
- Conditional and unconditional branches.
- Function return.

The interpreter can be stack-based or register-based. Prefer a register-like IR if it lowers cleanly to LLVM later.

## Runtime State

Each compiled host program owns:

- Static storage.
- Function table.
- Bytecode/IR.
- Diagnostics.

Each execution frame owns:

- Locals.
- Parameters.
- Return slot.
- Instruction pointer.

The PolyDraw app owns:

- External variable storage.
- Runtime API registry.
- Per-frame mutable values.

## Built-ins

Math:

- `ABS`, `ACOS`, `ASIN`, `ATAN`, `ATN`, `CEIL`, `COS`, `EXP`, `FABS`, `FACT`, `FLOOR`, `INT`, `LOG`, `SGN`, `SIN`, `SQR`, `SQRT`, `TAN`, `UNIT`.
- `ATAN2`, `FMOD`, `MIN`, `MAX`, `POW`.
- `PI`, `RND`, `NRND`.

PolyDraw-specific:

- Registered by the runtime layer, not hardcoded in the interpreter core.

## Error Handling

Diagnostics should include:

- Error code or category.
- Message.
- Source span.
- Nearby token.
- Optional related note, such as previous declaration or candidate external signatures.

Runtime errors should be recoverable for CLI tests and windowed live reload.

## JS Implementation Notes

- Use typed arrays for dense numeric storage.
- Keep AST nodes plain objects with `kind` tags.
- Keep interpreter deterministic and easy to snapshot.
- Add a trace runtime for testing GL call order.

## C Implementation Notes

- Use arena allocation for lexer/parser/AST.
- Use explicit dynamic arrays for IR and diagnostics.
- Avoid global mutable compiler state.
- Keep public C API small:
  - compile source to program.
  - execute program with runtime callbacks.
  - inspect diagnostics.
  - destroy program.

## Conformance Priorities

1. Expressions and precedence.
2. `static` storage across frames.
3. Multi-dimensional arrays.
4. User functions and recursion guard.
5. `goto` and labels.
6. External function overloads.
7. EVAL-compatible edge cases discovered from sample scripts.
