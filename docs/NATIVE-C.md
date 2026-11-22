# Functional C → C17: Standalone Native C Translator (`fc2c`)

Branch `native-c` · Version 1.4 · November 21, 2026

## 1. Premise

The original Functional C prototype translated `.fc` source into F# using a C# console application on
the .NET SDK. The `feature/c-target` branch added a C17 emitter (`CEmitter.cs`) alongside F#, still
hosted in C# / .NET.

This `native-c` branch removes the .NET dependency altogether: **both the translator (`fc2c`) and its
acceptance test harness (`tests/test_runner.c`) are written in portable C17** with zero third-party
libraries, preserving every premise of the original project:

- Hand-written lexer and recursive-descent parser (no regex, no parser generator).
- Exact same Functional C v1.3/1.4 language, type/conversion rules, `--strict` mode, and `FC1001`–`FC3018` diagnostics.
- Deterministic, readable C17 output with 4-space indentation and `\n` line endings.
- Atomic output writes: nothing is written until lexing, parsing, and checking all succeed; output is written to a sibling `.tmp` file and atomically renamed over the target.
- Both `fc2c`, `test_runner`, and **every single generated C program** compile cleanly under `-std=c17 -Wall -Wextra -Werror`.

---

## 2. Architecture

```text
src/fc.h       (420 lines)  — Arena allocator, StrBuf, Token/AST/Symbol structs, DiagList
src/lexer.c    (424 lines)  — Tokenizer, character/integer literals, FC1001–FC1005
src/parser.c   (883 lines)  — Recursive-descent parser with C precedence, FC2001–FC2002
src/checker.c  (783 lines)  — Scopes, types, mutability, captures, conversions, dead code, FC3001–FC3018
src/emitter.c (1154 lines)  — Lambda lifting, transitive captures, sequencing, UB-free C17 output
src/main.c     (511 lines)  — Arena/StrBuf/DiagList implementation, CLI, atomic file write
───────────────────────────
Total:        4,175 lines of C17 (0 external dependencies)
Test harness: 1,273 lines of C17 (tests/test_runner.c)
```

### Memory management

Every token string, AST node, symbol, capture set, emitted line, and diagnostic message is allocated
from a single linked-block `Arena` (`64 KiB` default slabs). One `arena_free(&arena)` call before exit
releases the entire compilation state cleanly. `setjmp`/`longjmp` is used for early abort on the first
lexical or syntax error without leaking memory.

---

## 3. Translation rules (`.fc` → `.c`)

| Functional C | Emitted C17 | Notes |
| --- | --- | --- |
| `int`, `bool`/`_Bool`, `char`, `unsigned char` | `int32_t`, `bool`, `int8_t`, `uint8_t` | Fixed width & signedness across compilers and platforms |
| `-2147483648` | `INT32_MIN` | Avoids C's `-` on unsigned/64-bit `2147483648` |
| `let x = expr;` | `const <type> u_x = expr;` | C compiler also enforces immutability |
| `let f = (int x) => expr;` | `static inline <ret> fc_lam_<fn>_<n>_<name>(<captures>, const int32_t u_x)` | Lifted in declaration order; direct & transitive captures passed as `const` params |
| Recursive call from lambda | Forward prototype of enclosing function emitted before its lifted lambdas | Handles `fact` (`lambdas.fc`) and `triangle` (`local_lambdas.fc`) |
| `+`, `-`, `*`, unary `-` | `fc_add`, `fc_sub`, `fc_mul`, `fc_neg` | Modular `uint32_t` arithmetic — zero signed-overflow UB |
| `/`, `%` | `fc_div`, `fc_rem` | Checks `b == 0` and `a == INT32_MIN && b == -1`; prints to `stderr` and calls `exit(1)` |
| Narrowing to `char`/`unsigned char` | `fc_char`, `fc_uchar` | Keeps low 8 bits without conversion warnings |
| Left-to-right evaluation | Statement-level `fc_tN` temporaries | Pure C17 (no GNU statement expressions); preserves short-circuiting in `?:`, `&&`, `||` and native `break`/`continue` in `while`/`do`/`for` |
| User / internal identifiers | `u_<name>` / `fc_<internal>` | Injective, collision-proof against C keywords, macros (`__LINE__`), `_` names, and libc |
| Unused params / locals | `(void)u_name;` emitted only when unread | Clean output with zero `-Wunused-*` warnings under `-Wall -Wextra -Werror` |
| `int main()` | `int32_t u_main(void)` + C `int main(void)` wrapper | Wrapper sets `_O_BINARY` on Windows stdout (`\n` exact match) and returns `(int)u_main()` |

---

## 4. Generated output example (`generated/sum_squares.c`)

Translating `examples/sum_squares.fc` with `fc2c`:

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

int32_t u_main(void)
{
    const int32_t u_result = u_sumSquares(5);
    fc_print(u_result);
    return 0;
}

int main(void)
{
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    return (int)u_main();
}
```

---

## 5. Verification results

Captured in `TEST-RESULTS.md` by `verify.ps1` on 2026-11-21 using **GCC 15.2.0** (MSYS2 UCRT64), with
zero `.NET` calls anywhere in the build or test pipeline:

- `fc2c.exe`: compiled with `gcc -std=c17 -Wall -Wextra -Werror -O2` — **0 warnings, 0 errors**.
- `examples/sum_squares.fc` → `generated/sum_squares.c` → `generated/sum_squares.exe`: stdout `55\n`, exit code `0`.
- Atomic write check: failing translation of `immutable_reassign.fc` exits `1` and leaves `generated/sum_squares.c` unchanged.
- `tests/test_runner.exe`: **126 passed, 0 failed, 0 skipped** (30.3 s):
  - 14 `examples/*.fc` end-to-end tests
  - 39 `tests/cases/run/*.fc` behavioral tests (including `c_name_collisions.fc` and `c_edge_cases.fc`)
  - 69 `tests/cases/reject/*.fc` diagnostic & no-output tests
  - 4 CLI/translator tests (atomic preservation, exit codes 0/2/3, determinism, `sum_squares` shape)
