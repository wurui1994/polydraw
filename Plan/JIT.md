# LLVM JIT Plan

## Positioning

LLVM JIT is a C-runtime optimization path. The interpreter remains the reference implementation and the first engine to reach full semantic coverage.

## Engine Contract

Both interpreter and JIT consume the same analyzed IR:

- Same symbol resolution.
- Same static storage layout.
- Same external runtime ABI.
- Same diagnostics before lowering.

CLI option:

```text
polydrawc --engine interp script.pss
polydrawc --engine jit script.pss
```

## Runtime ABI

Represent runtime calls with fixed C callback signatures.

Suggested approach:

- Numeric return: `double`.
- Runtime context pointer passed explicitly.
- Arguments marshaled as tagged values or by generated wrapper depending on call signature.
- String literals stored in program constant storage.
- Array/reference arguments pass pointers to `double`.

Example conceptual ABI:

```c
typedef double (*PdExternalFn)(PdRuntime *rt, const PdValue *args, int argc);
```

For performance, JIT can later specialize direct calls by exact signature.

## LLVM Lowering

Map IR to LLVM:

- `double` values to `double`.
- Boolean results to `double` `0.0` or `1.0` to preserve EVAL style.
- Locals to allocas or SSA values.
- Static storage as pointers into the program static block.
- Array access through helper functions or inlined bounds logic.
- Control flow to basic blocks.
- Internal function calls to generated LLVM functions.
- External calls to runtime callback thunks.

## ORC JIT

Use LLVM ORC, not legacy MCJIT:

- Initialize native target.
- Create one JIT session per process or app runtime.
- One module per compiled host program.
- Keep object lifetime tied to the compiled program.
- Support recompilation on file changes by dropping old resource trackers.

## Correctness Strategy

Every JIT test must also run the interpreter:

- Compare numeric return values.
- Compare static storage snapshots.
- Compare trace-runtime call logs.
- Compare runtime error behavior where practical.

JIT is allowed to be disabled for scripts using a feature not lowered yet, but it must report the unsupported feature before execution.

## Optimization Strategy

Initial JIT should be unoptimized or lightly optimized:

- `mem2reg`
- instruction combining
- reassociation only if it does not break EVAL-visible edge cases

Do not enable aggressive floating-point transforms by default.

## Risks

- Vararg-style external calls do not map cleanly to LLVM. Use runtime thunks first.
- String and reference arguments need exact lifetime rules. Store strings in compiled program memory.
- `goto` and labels inside functions require robust CFG generation. Let the IR builder normalize control flow before LLVM lowering.
- Infinite loops need the same cancellation story as the interpreter. Insert poll checks on backward branches.
