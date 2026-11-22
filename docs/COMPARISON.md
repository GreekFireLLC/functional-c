# Functional C (`fc2c`) vs. Other Functional Languages

Branch `native-c` · Version 1.4 · November 21, 2026

This document compares what **Functional C** (specifically the standalone C17 translator `fc2c` on the
`native-c` branch) can do relative to mainstream functional languages, functional-to-C compilers, and
standard C — along with an honest accounting of what other functional languages provide that Functional C
does not yet implement.

---

## 1. What Functional C does differently (and better for C-oriented work)

### 1.1 C-first syntax and mental model (superset direction)
Most functional languages (Haskell, OCaml, F#, Elixir, Lean 4, Scheme) ask a C programmer to adopt a new
syntax, indentation or parenthesis rules, and an expression-only control-flow model from day one.
Functional C moves in the opposite direction: start with familiar C (`int`, `char`, `unsigned char`,
braces, semicolons, `for`, `while`, `do`, `if`/`else`, `?:`, `+=`, `++`, casts, character literals) and
layer functional guarantees (`let` immutability, local lambdas with lexical capture, strict capture
checking) directly on top.

### 1.2 Native imperative control flow alongside functional bindings
In Functional C, `return` works anywhere (including inside deeply nested loops), alongside native
`break`, `continue`, `while`, `do ... while`, and multi-variable `for` loops.
- Pure functional languages (Haskell, Lean 4, Elm) have no native `break`/`continue`/early-`return`
  loops and rely on recursion, folds, or monadic/do-notation desugaring.
- Even hybrid ML languages like **F#** and **OCaml** have no `return`, `break`, or `continue` statements
  (the F# backend in this repo needed ~200 lines of flag-mode machinery — `fc'result`, `fc'returned`,
  `fc'brkN`, `fc'contN` — just to emulate them).
- On `native-c`, Functional C lowers straight to native C control flow while keeping `let` bindings
  immutable (`const`) and lambdas cleanly lifted to `static inline` C functions.

### 1.3 Zero runtime, zero garbage collector, zero VM, zero third-party dependencies
- **No GC or runtime library:** Unlike Haskell (GHC RTS), OCaml, F# (.NET CLR), Erlang/Elixir (BEAM),
  CHICKEN Scheme (Cheney-on-the-MTA GC + `libchicken`), Koka (Perceus RC + `mimalloc`/runtime), or
  Lean 4 (`libleanrt`), `fc2c` emits a **single, standalone C17 source file** containing only ~60 lines
  of `static inline` helpers and standard C17 headers (`<stdint.h>`, `<stdbool.h>`, `<stdio.h>`,
  `<stdlib.h>`, `<inttypes.h>`).
- **No toolchain lock-in:** Generated `.c` files compile with any standard C17 compiler (`gcc`, `clang`,
  MSVC) and pass `-std=c17 -Wall -Wextra -Werror` with zero warnings.
- **Tiny, auditable compiler:** The entire native translator is **4,175 lines of C17** across 6 files
  (`src/fc.h`, `lexer.c`, `parser.c`, `checker.c`, `emitter.c`, `main.c`), builds in under a second with
  one `gcc` command, and requires **zero .NET, zero LLVM, and zero external packages**.

### 1.4 C's loose conversions with an opt-in `--strict` switch — and no C undefined behavior
Mainstream functional compilers (F#, OCaml, Haskell, Lean 4, Rust) reject implicit conversions across
scalar types (`if (n)`, `bool b = 256;`, `count += i % 2 == 0;`, `char c = 200;`). Standard C accepts
them, but pairs them with silent undefined and implementation-defined behavior (signed overflow UB,
unspecified operand/argument evaluation order, platform-dependent `char` signedness, indeterminate
uninitialized locals, UB on falling off a non-`void` function, hardware traps on `/0` or `INT_MIN / -1`).

Functional C occupies the middle ground:
- **Default mode (C-compatible, deterministic):** accepts C's scalar conversions, wraps 8-bit `char`
  (`int8_t`) and `unsigned char` (`uint8_t`) explicitly, initializes uninitialized locals to `0`/`false`,
  returns `0`/`false` if a function falls off its end (with warning `FC3005`), prunes unreachable code
  (with warning `FC3016`), guarantees left-to-right evaluation without GNU extensions, uses `uint32_t`
  wraparound for 32-bit signed arithmetic, and catches `/0` and `INT32_MIN / -1` with a clean `stderr`
  diagnostic and exit code `1`.
- **`--strict` mode:** flips implicit `int`/`char`-to-`bool` storage, unreachable code, missing
  `return`, and uninitialized variables back into hard compile-time errors.

### 1.5 Static, zero-overhead lambda lifting with mutable-capture safety
Standard C (including C23 and current C2y drafts as of late 2026) still has **no** standardized closures
or lambdas. GNU nested functions historically required executable stacks (or heap trampolines), and C++
lambdas introduce complex object lifetimes and dangling `[&]` capture hazards.

`fc2c` takes a strict, zero-overhead design:
- Lambdas capture enclosing immutable bindings (including transitive captures through chained lambdas)
  and lift to file-scope `static inline` C functions — zero heap allocation, zero trampolines, zero
  closure structs for local lambdas, inlined away by `-O2`.
- Capturing a mutable variable that is assigned anywhere in the function is rejected statically (`FC3017`)
  with the exact line/column and a concrete fix (`let xValue = x;`).
- `let` bindings emit `const` in the generated C, so the downstream C compiler double-checks immutability.

### 1.6 Readable, human-inspectable C output and atomic failure safety
Unlike many functional-to-C compilers whose generated C resembles assembly or CPS-transformed runtime
calls (e.g., CHICKEN Scheme's CPS/Cheney C output or GHC's unregisterised C), `fc2c` emits readable,
deterministically formatted C17 that maps closely to the `.fc` source. Nothing is written until every
front-end stage passes, and writes happen via atomic sibling-temp rename so a failed compile never
corrupts an existing `.c` file.

### 1.7 Proven cross-backend semantic equivalence
Across the repository's branches (`main` / `feature/c-target` / `native-c`), the exact same 14 examples
and behavioral test suite have been verified byte for byte on both **.NET/F#** and **native C17**
(186/186 tests passing on `feature/c-target`, 126/126 standalone pure-C17 tests passing on `native-c`).

---

## 2. Side-by-side comparison matrix

| Capability | Functional C (`fc2c`, v1.4) | F# / OCaml | Haskell (GHC) | Koka / Lean 4 | Futhark / SaC | CHICKEN Scheme | Standard C (C17/C23) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| **C-familiar syntax** | Yes (C subset + `let` / `=>`) | No | No | No | SaC: partial; Futhark: ML-like | No (S-exprs) | Native |
| **Native `return`/`break`/`continue`/loops** | Yes | Partial (no `return`/`break`/`continue`) | No | Partial (`Lean` `do` sugar) | Restricted (parallel loops) | No (`call/cc` / recursion) | Yes |
| **Immutable `let` by default** | Yes (`let` → `const`) | Yes | Yes (all) | Yes | Yes | Partial (`set!`) | Opt-in (`const`) |
| **Lexical lambdas & closures** | Local `let` lambdas (lifted, zero overhead) | Full first-class closures | Full first-class closures | Full first-class closures | Higher-order (inlined/defunctionalized) | Full first-class closures | **None in ISO C** (C2y still debated) |
| **Compiles to readable portable C17** | Yes (single file, `-Wall -Wextra -Werror`) | No (CLR / native) | No (native/LLVM) | Yes (with runtime lib) | Yes (array/GPU runtimes) | Yes (CPS C + `libchicken`) | N/A |
| **Runtime / GC required** | **None** (~60 lines `static inline`) | .NET GC / OCaml GC | GHC GC + RTS | Perceus RC / Lean RC + runtime | Array/GPU runtime | Cheney-on-the-MTA GC | None |
| **Compiler size / dependencies** | ~4.2K LOC C17, **0 deps** | Large SDK / toolchain | Very large (GHC) | Large (Haskell/C++/Lean) | Large (Haskell / C) | Medium (Scheme/C) | Large (GCC/Clang) |
| **C loose conversions + `--strict` toggle** | **Yes** (both modes) | No (always strict) | No (always strict) | No (always strict) | No | Dynamic typing | Always loose (with UB) |
| **Defined signed wraparound & checked `/`/`%`** | Yes (no UB) | Yes | Yes (`Int` wraps, `/` throws) | Arbitrary-precision or checked | Checked / platform | Arbitrary-precision | **Undefined behavior** |
| **Guaranteed left-to-right evaluation** | Yes | Mostly (args) | Lazy | Yes | Pure (order-independent) | Unspecified in R5RS | **Unspecified** |
| **First-class / escaping functions** | Not yet | Yes | Yes | Yes | Restricted | Yes | Function pointers only (no capture) |
| **ADTs, pattern matching, generics** | Not yet | Yes | Yes | Yes | Partial | Dynamic / macros | `_Generic` / tagged unions manually |
| **Pointers, structs, arrays, strings, FFI** | Not yet (planned) | Managed / FFI | FFI | FFI / C interop | Arrays + C FFI | C FFI | Native |
| **Purity / effect system / proofs** | None | None | `IO` monad | Algebraic effects / dependent proofs | Pure array semantics | None | None |

---

## 3. Honest boundaries: what other functional languages do that Functional C cannot yet

Functional C v1.4 is a deliberately small, fully verified core. Compared to mature functional languages,
it does **not** yet support:

1. **First-class and escaping functions:** Functions and lambdas cannot be passed as arguments, returned
   from functions, or stored in data structures (`FC3009`, `FC3014`). Higher-order patterns like `map`,
   `filter`, and `fold` require adding closure values (e.g., fat pointers `{ fn_ptr, env_ptr }`).
2. **Algebraic data types (ADTs) and pattern matching:** No sum types (`Option`/`Result`), discriminated
   unions, tuples, or exhaustiveness-checked `match`.
3. **Generics / parametric polymorphism:** Every function and lambda parameter is monomorphically typed
   (`int`, `bool`, `char`, `unsigned char`).
4. **Compound data and C memory features:** No arrays, strings, pointers, `struct`, `union`, `enum`,
   `void`, `switch`, floating-point (`float`/`double`), wider integers (`short`/`long`/`unsigned int`),
   bitwise/shift operators, global variables, forward prototypes, mutual recursion, or C `#include`/FFI.
5. **Tail-call optimization (TCO) guarantee:** Deep non-tail recursion uses the native C call stack
   (tail calls rely on the downstream C compiler at `-O2`, though loops use O(1) stack).
6. **Effect tracking or formal verification:** No pure/impure type separation (Haskell `IO`, Koka
   effects) and no theorem proving (Lean 4).

---

## 4. Strategic takeaway

The unique niche of Functional C on `native-c` is **not** competing with Haskell or Lean 4 on type-level
abstractions — it is bringing **functional safety primitives** (immutable `let`, lexical lambdas with
checked captures, deterministic evaluation, UB-free arithmetic, optional `--strict` checking) into a
**zero-runtime, C-syntax language that emits clean, standalone C17**.

Moving from `.NET`/F# to a native C17 translator (`fc2c`) removes the biggest architectural barrier noted
in earlier versions: adding C's memory model (pointers, arrays, structs, unions, headers, and C library
interop) no longer requires simulating C memory inside a managed runtime, while first-class closures can
be lowered directly to standard C closure structs and function pointers.
