# Translating Functional C into C17 (`CEmitter`)

Version 1.4 · Branch `feature/c-target` · November 21, 2026

## 1. Premise and motivation

The first Greek Fire blog post about Functional C (*Functional C — C syntax, forged in F#*) noted two opposite roads between C and functional languages:

1. **C syntax into a functional target** — Functional C versions 0 through 1.3 translate `.fc` source into F# (`FSharpEmitter.cs`) and hand compilation to the .NET SDK.
2. **Functional code down into C** — systems such as [Koka](https://koka-lang.github.io/koka/doc/index.html), [Lean 4](https://lean-lang.org/), and [CHICKEN Scheme](https://call-cc.org/) compile functional languages down to portable C, letting a standard C compiler handle optimization and native code generation.

Version 1.4 takes the existing Functional C language and translator down that second road under the **exact same premises**:

- **Shared front end.** Lexing (`Lexer.cs`), recursive-descent parsing (`Parser.cs`), AST (`Ast.cs`), control-flow queries (`Flow.cs`), type/mutability/capture checking (`Checker.cs`), diagnostics (`Diagnostics.cs`), and `--strict` mode are completely unchanged and shared by both backends.
- **Zero third-party packages.** The translator remains a self-contained C# console application on `.NET 10` with no parser generators, no Roslyn, and no compiler forks.
- **Readable, deterministic output.** `CEmitter.cs` emits deterministic C17 with 4-space indentation and `\n` line endings. Two translations of the same `.fc` file are byte-for-byte identical.
- **Warnings treated as errors.** Just as the F# backend compiles generated `Program.fs` files with `<TreatWarningsAsErrors>true</TreatWarningsAsErrors>`, every generated C program must compile cleanly under `gcc -std=c17 -Wall -Wextra -Werror`.
- **Atomic writes.** Nothing is written until every front-end stage succeeds; output goes to a sibling `.tmp` file and is renamed over the target. A failed translation leaves any existing `.c` file untouched.
- **Identical observable behavior.** Every example and behavioral test in the repository compiles and runs through both F# and C17 and is verified byte for byte on stdout and exit code.

---

## 2. Pipeline and CLI

```text
program.fc ─► Lexer ─► Parser ─► Checker ─┬─► FSharpEmitter ─► Program.fs ─► dotnet build ─► .NET binary
                                          └─► CEmitter      ─► program.c  ─► gcc -std=c17 ─► native binary
```

### Selecting the C target

- **Automatic by extension:** any `--out` path ending in `.c` (case-insensitive) selects the C17 backend.
- **Explicit flag:** `--target c` selects C17 regardless of output extension; `--target fsharp` (or `fs`) selects F#.
- **Strict mode:** `--strict` applies identically to both targets because checking happens before emission.

```text
dotnet build src/FunctionalC.Compiler/FunctionalC.Compiler.csproj
dotnet run --project src/FunctionalC.Compiler -- examples/sum_squares.fc --out generated/SampleAppC/program.c
gcc -std=c17 -Wall -Wextra -Werror generated/SampleAppC/program.c -o generated/SampleAppC/program.exe
.\generated\SampleAppC\program.exe
```

Exit codes are unchanged across targets:

| Code | Meaning |
| --- | --- |
| `0` | Translated (warnings, if any, printed to stderr) |
| `1` | Source errors (diagnostics to stderr; output file untouched) |
| `2` | Bad command line (including unknown `--target`) |
| `3` | File I/O failure |

---

## 3. F# vs. C17: mirror-image trade-offs

Translating the exact same language to F# and to C exposes an almost exact symmetry: what is free in one target is the hard part in the other.

| Feature | F# backend (`FSharpEmitter.cs`) | C17 backend (`CEmitter.cs`) |
| --- | --- | --- |
| `return`, `break`, `continue` | **Hard.** F# lacks all three; requires expression mode + flag mode (`fc'result`, `fc'returned`, `fc'brkN`, `fc'contN`, `fc'againN`). | **Native.** Emitted directly as C `return`, `break`, `continue`. |
| Loops (`while`, `do`, `for`) | `do` and `for` lowered to `while` + flags; `continue` guards loop tails. | **Native** in the common case; uses a `for (bool fc_firstN = true; ; fc_firstN = false)` wrapper only when a condition needs statement-level evaluation sequencing. |
| Mutable variables & parameters | Parameters shadowed with `let mutable`; variables tracked for assignment. | **Native.** C locals and parameters are already mutable copies. |
| Immutable `let` bindings | **Native** (`let x = ...`). | Emitted as `const <type> u_x = ...;` so the C compiler enforces immutability too. |
| Lambdas & closures | **Native** (`fun (x: int) -> ...`). | **Hard.** Standard C17 has no closures; lifted to file-scope `static inline` functions with direct and transitive captures passed as explicit `const` parameters. |
| Left-to-right evaluation | Inline `(let fc't1 = ... in ...)` expressions. | **Hard.** C17 has no `let ... in` expression (and GNU `({ ... })` is non-standard); lowered to statement-level `fc_tN` temporaries ahead of the enclosing statement or inside lowered `if`/`while`/`for` blocks. |
| 32-bit wraparound arithmetic | **Native** in unchecked .NET `int`. | **UB in C.** Routed through `static inline` `fc_add`/`fc_sub`/`fc_mul`/`fc_neg` using `uint32_t` arithmetic. |
| Division by zero & `INT32_MIN / -1` | Throws .NET `DivideByZeroException` / `OverflowException`. | **UB in C.** Routed through `fc_div`/`fc_rem`, printing a runtime error to `stderr` and calling `exit(1)`. |
| 8-bit `char` / `unsigned char` | Mapped to `sbyte` / `byte` (F# `char` is UTF-16). | Mapped to `int8_t` / `uint8_t` (plain C `char` signedness is implementation-defined). |
| Identifier collisions | Backtick-quotes F# keywords; prefixes uppercase/`_` with `u'`; internals use `fc'`. | Prefixes **all** user identifiers with `u_` and internals with `fc_`, preventing collisions with C keywords, macros (`__LINE__`), `_` names, and libc. |

---

## 4. Translation rules in detail

### 4.1 Types and literals

| Functional C | Emitted C17 | Rationale |
| --- | --- | --- |
| `int`, `signed`, `signed int` | `int32_t` | Exact 32-bit two's-complement width on every platform. |
| `bool`, `_Bool` | `bool` (`<stdbool.h>`) | Standard C17 boolean type. |
| `char`, `signed char` | `int8_t` | Avoids C's implementation-defined signedness of plain `char`. |
| `unsigned char` | `uint8_t` | Exact unsigned 8-bit integer. |
| `-2147483648` | `INT32_MIN` | In C, `-2147483648` parses as unary `-` applied to `2147483648`, which overflows 32-bit signed `int` and changes type. |

Literal conversions are folded at translation time, matching `FSharpEmitter`:
- `char big = 200;` → `int8_t u_big = ((int8_t)-56);`
- `unsigned char ubig = 300;` → `uint8_t u_ubig = ((uint8_t)44);`
- `bool one = 1;` → `bool u_one = true;`

Runtime conversions use explicit casts and two 8-bit narrowing helpers:
- `fc_char(v)` → `(int8_t)(uint8_t)(uint32_t)v`
- `fc_uchar(v)` → `(uint8_t)(uint32_t)v`
- number → `bool` → `(expr != 0)`
- `bool` / `int8_t` / `uint8_t` → `int32_t` → `((int32_t)(expr))`

### 4.2 Lambda lifting and transitive captures

In Functional C v1.3/1.4:
- Every lambda is the entire initializer of a `let` binding (`let f = (int x) => ...;`).
- Lambdas are not first-class values and cannot escape their declaring block.
- Every captured variable is guaranteed immutable (`Checker.cs` rejects capturing any assigned variable with `FC3017`), and shadowing is forbidden (`FC3003`).
- Lambdas cannot be recursive or forward-referenced (`FC3015`, `FC3004`), so lambdas within a function form a DAG in declaration order.

Because of these invariants, `CEmitter` lifts every lambda in a single forward pass over each function body:

1. **Direct captures:** any `NameExpr` inside the lambda body that resolves to an outer `ValueSymbol` (excluding the lambda's own parameters) is added to the lambda's capture list.
2. **Transitive captures:** any `CallExpr` inside the lambda body targeting an earlier `LambdaSymbol` adds all of that callee's captures to the current lambda's capture list.
3. **Unique deterministic naming:** each lifted lambda is named `fc_lam_<enclosingFunc>_<ordinal>_<letName>`, keyed by `LambdaSymbol` reference so sibling blocks with identical lambda names never collide.
4. **Enclosing recursion:** if any lifted lambda calls its enclosing function (such as `step` calling `fact` or `previous` calling `triangle`), a forward prototype of the enclosing function is emitted before its lifted lambdas.
5. **Emission:** each lifted lambda is emitted as `static inline <retType> fc_lam_...(<const captures>, <const params>)`. Using `static inline` prevents `-Wunused-function` warnings on uncalled lambdas and lets the C compiler inline them away completely.

Example from `examples/local_lambdas.fc`:

```c
// Functional C input
int clampedSquares(int low, int high)
{
    let clamp = (int x) => x < low ? low : x > high ? high : x;
    let square = (int x) => x * x;
    let clampedSquare = (int x) => square(clamp(x));
    return clampedSquare(-7) + clampedSquare(3) + clampedSquare(50);
}

int triangle(int n)
{
    let previous = () => triangle(n - 1);
    return n == 0 ? 0 : n + previous();
}
```

Actual C17 emitted by `CEmitter`:

```c
static inline int32_t fc_lam_clampedSquares_1_clamp(const int32_t u_low, const int32_t u_high, const int32_t u_x)
{
    return ((u_x < u_low) ? u_low : ((u_x > u_high) ? u_high : u_x));
}

static inline int32_t fc_lam_clampedSquares_2_square(const int32_t u_x)
{
    return fc_mul(u_x, u_x);
}

static inline int32_t fc_lam_clampedSquares_3_clampedSquare(const int32_t u_low, const int32_t u_high, const int32_t u_x)
{
    return fc_lam_clampedSquares_2_square(fc_lam_clampedSquares_1_clamp(u_low, u_high, u_x));
}

int32_t u_clampedSquares(int32_t u_low, int32_t u_high)
{
    int32_t fc_t1 = fc_lam_clampedSquares_3_clampedSquare(u_low, u_high, -7);
    int32_t fc_t2 = fc_add(fc_t1, fc_lam_clampedSquares_3_clampedSquare(u_low, u_high, 3));
    return fc_add(fc_t2, fc_lam_clampedSquares_3_clampedSquare(u_low, u_high, 50));
}

int32_t u_triangle(int32_t u_n);

static inline int32_t fc_lam_triangle_1_previous(const int32_t u_n)
{
    return u_triangle(fc_sub(u_n, 1));
}

int32_t u_triangle(int32_t u_n)
{
    return ((u_n == 0) ? 0 : fc_add(u_n, fc_lam_triangle_1_previous(u_n)));
}
```

### 4.3 Left-to-right evaluation without GNU extensions

Standard C leaves the evaluation order of binary operands and function arguments unspecified, whereas Functional C guarantees left-to-right evaluation (`tests/cases/run/evaluation_order.fc`).

`CEmitter` uses the same `MayHaveEffects` predicate as `FSharpEmitter` (calls, `/`, and `%`) and threads a `List<string> prep` buffer through expression emission:

- **Pure / single-effect expressions:** emit directly as normal C expressions with zero temporaries.
- **Binary operators (`+ - * / % < <= > >= == !=`):** when both `Left` and `Right` may have effects, `Left` is emitted into a typed temporary (`int32_t fc_tN = ...;`) before `Right` is emitted.
- **Calls (`f(a, b, c)`):** when two or more arguments may have effects, non-atomic arguments before the last effectful argument are bound to `fc_tN` temporaries in source order.
- **Conditional (`c ? a : b`):** if neither branch produces `prep` statements, emits a plain C ternary `(c ? a : b)`. If either branch needs sequencing, lowers to `if (c) { <truePrep> fc_tN = a; } else { <falsePrep> fc_tN = b; }` so the unselected branch is never evaluated.
- **Short-circuit (`&&`, `||`):** if the RHS produces no `prep` statements, emits plain `(a && b)` / `(a || b)`. Otherwise lowers to a guarded `if` around `<rightPrep>` so short-circuiting is preserved.
- **Loops with sequenced conditions:**
  - `while (cond)` lowers to `while (true) { <prep> if (!(cond)) { break; } <body> }`.
  - `do { body } while (cond);` lowers to:
    ```c
    for (bool fc_first1 = true; ; fc_first1 = false) {
        if (!fc_first1) {
            <prep>
            if (!(cond)) {
                break;
            }
        }
        <body>
    }
    ```
    This preserves native `break` and `continue` (a `continue` jumps to `fc_first1 = false`, then evaluates `cond` before the next iteration) without `goto`.
  - `for (init; cond; update)` emits a native C `for (...)` whenever `init`, `cond`, and `update` need no prep statements or `(void)` unused-var suppressions; otherwise it emits a scoped `{ <init> for (bool fc_firstN = true; ; fc_firstN = false) { ... } }` block that preserves loop-header scope, `continue`-runs-update, and `break`-skips-update.

### 4.4 Runtime prelude and elimination of C undefined behavior

Every generated `.c` file begins with a small `static inline` prelude:

- `fc_print(int32_t)` — prints `%" PRId32 "\n"`. In `int main(void)`, `#ifdef _WIN32 _setmode(_fileno(stdout), _O_BINARY); #endif` prevents Windows text-mode `\r\n` translation so stdout matches `\n` byte for byte across all platforms.
- `fc_add`, `fc_sub`, `fc_mul`, `fc_neg` — cast operands to `uint32_t`, perform modular 32-bit arithmetic, and convert back to `int32_t`. Eliminates C signed-integer overflow UB at all optimization levels.
- `fc_div`, `fc_rem` — explicitly guard `b == 0` and `a == INT32_MIN && b == -1`, printing a diagnostic to `stderr` and calling `exit(1)` (which flushes buffered `stdout`, unlike a hardware `SIGFPE` trap).
- `fc_char`, `fc_uchar` — explicit low-8-bit narrowing without sign-conversion warnings.

### 4.5 Identifier hygiene and warning-free `-Wall -Wextra -Werror`

- **Injective naming (`CNames.ForSource`):** every source identifier `x` becomes `u_x` (`u_main`, `u_printf`, `u___LINE__`, `u_fc_add`, `u_u_x`). Internal names all start with `fc_`. Because `u_` and `fc_` are disjoint and no C keyword, standard header symbol, macro, or reserved `_` identifier starts with `u_`, collisions are impossible (`tests/cases/run/c_name_collisions.fc`).
- **Zero warnings:**
  - Read-usage analysis (`CollectReads` / `CollectLambdaLocalReads`) emits `(void)u_name;` only for unused parameters or unused locals/bindings, keeping normal code clean while preventing `-Wunused-parameter`, `-Wunused-variable`, and `-Wunused-but-set-variable`.
  - Runtime helpers and lifted lambdas are `static inline` (no `-Wunused-function`).
  - Zero-parameter functions and lambdas use `(void)` rather than `()`.
  - Source `int main()` is emitted as `int32_t u_main(void)` (so other functions can still call it, as tested by `main_exit_code.fc`), invoked by the C entry point `int main(void)`.

---

## 5. Side-by-side example: `examples/tour.fc`

### Functional C source

```c
int sumSquares(int n)
{
    let square = (int x) => x * x;
    return n <= 0 ? 0 : square(n) + sumSquares(n - 1);
}

bool isPrime(int n)
{
    if (n < 2) return false;
    for (int d = 2; d * d <= n; d++) {
        if (n % d == 0) return false;
    }
    return true;
}
```

### Generated C17 (excerpt from `CEmitter`)

```c
static inline int32_t fc_lam_sumSquares_1_square(const int32_t u_x)
{
    return fc_mul(u_x, u_x);
}

int32_t u_sumSquares(int32_t u_n)
{
    int32_t fc_t2;
    if ((u_n <= 0)) {
        fc_t2 = 0;
    } else {
        int32_t fc_t1 = fc_lam_sumSquares_1_square(u_n);
        fc_t2 = fc_add(fc_t1, u_sumSquares(fc_sub(u_n, 1)));
    }
    return fc_t2;
}

bool u_isPrime(int32_t u_n)
{
    if ((u_n < 2)) {
        return false;
    }
    for (int32_t u_d = 2; (fc_mul(u_d, u_d) <= u_n); u_d = fc_add(u_d, 1)) {
        if ((fc_rem(u_n, u_d) == 0)) {
            return false;
        }
    }
    return true;
}
```

Compare `u_isPrime` above with the F# version's `fc'result` / `fc'returned` flag mode — and compare `u_sumSquares` with F#'s one-line `let square = (fun (x: int) -> (x * x))` expression mode.

---

## 6. Verification and codebase metrics

Recorded in `TEST-RESULTS.md` from `powershell -ExecutionPolicy Bypass -File .\verify.ps1` on 2026-11-21:

- **Toolchains:** `.NET SDK 10.0.300` (`net10.0`), `gcc (Rev8, Built by MSYS2 project) 15.2.0`
- **Compiler build:** `0 Warning(s), 0 Error(s)` (`<TreatWarningsAsErrors>true</TreatWarningsAsErrors>`)
- **Sample workflow (`examples/sum_squares.fc`):**
  - F# target (`generated/SampleApp/Program.fs`): stdout `55\n`, exit `0`
  - C17 target (`generated/SampleAppC/program.c`, compiled with `-std=c17 -Wall -Wextra -Werror`): stdout `55\n`, exit `0`
  - Atomic failure preservation verified for both `.fs` and `.c` outputs
- **Acceptance suite (`tests/FunctionalC.Tests`):** **186 passed, 0 failed, 0 skipped** (65.5 s total; C-only `--target c` runs 129 tests in 13.8 s):
  - 14 examples × 2 targets (F# and C) = 28 tests
  - 39 behavioral `tests/cases/run/*.fc` cases × 2 targets = 78 tests (including new `c_name_collisions.fc` and `c_edge_cases.fc`)
  - 69 rejection `tests/cases/reject/*.fc` cases
  - 11 CLI, lexer, determinism, emitted-shape, and reference-fixture tests

### Test harness directives for C

In addition to the existing `//! out:`, `//! exit:`, `//! stderr-contains:`, `//! fs-contains:`, `//! fs-lacks:`, `//! warning:`, `//! error:`, and `//! flags: --strict` directives, `tests/FunctionalC.Tests` supports:

- `//! c-contains: <substring>` — asserts the generated C contains `<substring>`.
- `//! c-lacks: <substring>` — asserts the generated C does not contain `<substring>`.
- `//! c-stderr-contains: <substring>` — asserts runtime `stderr` on the C target contains `<substring>` (when omitted, `.NET` `System.DivideByZeroException` / `System.OverflowException` expectations map to `"division by zero"` / `"overflow"`).
- Harness CLI flag: `--target both|fsharp|c` (defaults to `both`).

### Compiler source size (`src/FunctionalC.Compiler/`)

| File | Lines | Role |
| --- | ---: | --- |
| `Lexer.cs` | 453 | Hand-written tokenizer (shared) |
| `Parser.cs` | 793 | Recursive-descent parser (shared) |
| `Ast.cs` | 413 | AST and symbol nodes (shared) |
| `Checker.cs` | 803 | Types, scopes, mutability, captures, conversions (shared) |
| `Flow.cs` | 44 | Control-flow queries (shared) |
| `Diagnostics.cs` | 57 | Diagnostic codes and formatting (shared) |
| `FSharpEmitter.cs` | 734 | F# backend |
| `CEmitter.cs` | 1,201 | Portable C17 backend |
| `Translator.cs` | 48 | In-memory pipeline dispatcher |
| `Cli.cs` | 196 | CLI parsing and atomic writes |
| `Program.cs` | 6 | Entry point |
| **Total** | **4,748** | **0 third-party packages** |

---

## 7. What targeting C opens up next

Version 1.3's blog post noted that moving Functional C toward a full superset of C gets expensive on F# once **memory** enters the picture: pointers, pointer arithmetic, arrays, structs, unions, and C headers require emulating a C memory model inside .NET.

With a C17 backend in place, those trade-offs invert:

- **C memory features** (pointers, arrays, structs, unions, `void`, `switch`, wider integer types, C headers) can lower directly to native C constructs instead of a simulated heap.
- **First-class functions and escaping closures** — the next major step on the functional side — can lower using the standard closure-conversion pattern (a fat pointer `struct { ret (*fn)(void *env, ...); void *env; }` paired with an environment struct, plus lifetime/region or arena management as in Koka and Lean 4).
