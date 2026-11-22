# Functional C — Native C17 Translator (`fc2c`, version 1.4)

Functional C is a small language with C syntax and functional features: immutable `let` bindings and
lambdas alongside C's typed variables, `if`/`else` and loops. On this `native-c` branch, the translator
itself (`fc2c`) and its acceptance test harness are written in **portable C17 with zero .NET and zero
third-party dependencies** — all you need is a C17 compiler such as GCC or Clang.

```text
examples/sum_squares.fc ──► fc2c (C17) ──► generated/sum_squares.c ──► gcc -std=c17 ──► native binary
```

Related branches in this repository:

- **`main`** — original Functional C → F# translator (C# / .NET SDK, version 1.3).
- **`feature/c-target`** — dual-target C# translator supporting both F# and C17.
- **`native-c`** (this branch) — standalone C17 translator (`fc2c`) and C17 test runner; zero .NET required.

## Requirements

- Any C17 compiler (`gcc` or `clang`). Built and tested with **GCC 15.2.0** (MSYS2 UCRT64 on Windows).
- Zero third-party libraries. No .NET SDK, no Python, no parser generator.

## Layout

| Path | Contents |
| --- | --- |
| `src/fc.h` | Shared arena allocator, string buffer, token/AST/symbol structs, diagnostics |
| `src/lexer.c` | Hand-written tokenizer (`FC1001`–`FC1005`), no regex |
| `src/parser.c` | Recursive-descent parser (`FC2001`–`FC2002`) |
| `src/checker.c` | Scopes, types, mutability, captures, conversions, dead-code pruning, `--strict` (`FC3001`–`FC3018`) |
| `src/emitter.c` | Portable C17 emitter: lambda lifting, transitive captures, left-to-right sequencing, UB-free runtime prelude |
| `src/main.c` | CLI (`fc2c`) and atomic temp-file replacement |
| `examples/*.fc` | 14 complete example programs (expected outputs computed independently in Python) |
| `generated/sum_squares.c` | Generated C17 output for `examples/sum_squares.fc` |
| `tests/test_runner.c` | Native C17 acceptance-test harness (compiles & runs every test with `gcc -std=c17 -Wall -Wextra -Werror`) |
| `tests/cases/run/*.fc` | 39 behavioral end-to-end tests |
| `tests/cases/reject/*.fc` | 69 invalid programs that `fc2c` must reject with exact diagnostics |
| `Makefile` | Cross-platform build/sample/test targets (`make`, `make sample`, `make test`) |
| `verify.ps1` | End-to-end verification script writing `TEST-RESULTS.md` |
| `docs/NATIVE-C.md` | Full design, translation rules, and verification documentation |
| `docs/COMPARISON.md` | What Functional C can do compared to other functional languages and standard C |
| `TEST-RESULTS.md` | Exact commands, exit codes, and outputs from the latest `verify.ps1` run |

## Quick start

Windows (PowerShell) or any shell with `gcc`:

```text
gcc -std=c17 -Wall -Wextra -Werror -O2 src/main.c src/lexer.c src/parser.c src/checker.c src/emitter.c -o fc2c.exe
.\fc2c.exe examples/sum_squares.fc --out generated/sum_squares.c
gcc -std=c17 -Wall -Wextra -Werror generated/sum_squares.c -o generated/sum_squares.exe
.\generated\sum_squares.exe
```

On Linux/macOS/MSYS2 with `make`:

```text
make
make sample
make test
```

The sample prints `55` and exits with code `0`.

Add `--strict` to reject what C accepts by default (a number stored in a `bool`, unreachable code, a
function reaching its end without `return`, and uninitialized declarations such as `int x;`):

```text
.\fc2c.exe examples/sum_squares.fc --out generated/sum_squares.c --strict
```

### Exit codes

| Code | Meaning |
| --- | --- |
| `0` | Translated (warnings, if any, go to stderr) |
| `1` | Source errors (diagnostics go to stderr; **existing output file is untouched**) |
| `2` | Bad command line |
| `3` | File I/O failure |

## Running the acceptance tests

```text
gcc -std=c17 -Wall -Wextra -Werror -O2 tests/test_runner.c -o tests/test_runner.exe
.\tests\test_runner.exe
```

Or run the full verification workflow and regenerate `TEST-RESULTS.md`:

```text
powershell -ExecutionPolicy Bypass -File .\verify.ps1
```

Every behavioral test translates the `.fc` file with `fc2c`, compiles the generated `.c` with
`gcc -std=c17 -Wall -Wextra -Werror`, runs the resulting native binary, and compares stdout byte for
byte along with the exit code and stderr diagnostics. Rejection tests verify exit code `1`, exact
diagnostic file/line/column/code/message, and that no output or `.tmp` file was written.

## Language & translation overview

Functional C implements the exact version 1.3/1.4 language contract:

- **Types:** `int` (`int32_t`), `bool`/`_Bool` (`bool`), `char`/`signed char` (`int8_t`), `unsigned char` (`uint8_t`).
- **Immutable `let` & lambdas:** `let x = expr;` emits `const`; `let f = (int x) => expr;` lifts to a file-scope `static inline` C function with direct and transitive captures passed as explicit `const` parameters.
- **Mutable C variables & control flow:** typed declarations, `= += -= *= /= %= ++ --`, `if`/`else`, `while`, `do`/`while`, `for`, `break`, `continue`, and `return` anywhere.
- **Loose C conversions & `--strict`:** numbers as truth values, `bool` as `1`/`0`, integer promotion in arithmetic, low-8-bit wrapping on `char`/`unsigned char`, explicit casts `(type) x`, and character literals.
- **Deterministic semantics without C UB:** left-to-right operand/argument evaluation via statement-level `fc_tN` temporaries (pure C17, no GNU statement expressions), 32-bit wraparound arithmetic via `uint32_t`, checked `/` and `%` on `0` and `INT32_MIN / -1`, and collision-free `u_`/`fc_` identifier prefixes.

See [docs/NATIVE-C.md](docs/NATIVE-C.md) for the complete technical specification and generated C17 examples.

## License

[MIT](LICENSE) © 2026 Greek Fire LLC.
