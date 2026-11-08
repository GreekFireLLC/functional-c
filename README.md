# Functional C (version 0 prototype)

Functional C is a small functional language with C-style syntax: braces, semicolons, typed C-style
function declarations, and expressions. Its translator turns a `.fc` file into ordinary F# source, and
the standard .NET SDK compiles and runs that F#.

```
examples/sum_squares.fc ──► FunctionalC.Compiler (C#) ──► Program.fs ──► dotnet build ──► .NET program
```

This prototype is a deliberately small C-like language. It is **not** compatible with arbitrary C
programs: there are no pointers, arrays, structures, loops, mutation, headers or preprocessor.

## Requirements

- The **.NET SDK** (not only the runtime): <https://dotnet.microsoft.com/en-us/download>
- Built and tested with **.NET SDK 10.0.300**; every project targets **`net10.0`**. To use an older
  SDK, change `<TargetFramework>` in the three project files (`src/FunctionalC.Compiler`,
  `tests/FunctionalC.Tests` and `generated/SampleApp`).
- No third-party packages. The translator uses neither Roslyn nor a modified F# compiler.

## Layout

| Path | Contents |
| --- | --- |
| `src/FunctionalC.Compiler/` | The translator (C# console app): `Lexer.cs`, `Parser.cs`, `Ast.cs`, `Checker.cs`, `FSharpEmitter.cs`, `Translator.cs`, `Cli.cs` |
| `examples/sum_squares.fc` | The sample program |
| `examples/sum_squares.reference.fs` | The hand-written F# from the handoff, kept for comparison. It is not translator output and not part of any project. |
| `generated/SampleApp/` | F# console project. Its `Program.fs` is **generated** by the translator. |
| `tests/FunctionalC.Tests/` | Acceptance-test harness (a plain C# console app, no test framework) |
| `tests/cases/run/*.fc` | Behavioral tests that are translated, compiled with F# and executed |
| `tests/cases/reject/*.fc` | Invalid programs that the translator must reject with a specific diagnostic |
| `verify.ps1` | Runs the whole workflow and all tests, then writes `TEST-RESULTS.md` |
| `TEST-RESULTS.md` | Results of the latest `verify.ps1` run |

## Quick start

Run these from this folder in PowerShell or any shell:

```text
dotnet build src/FunctionalC.Compiler/FunctionalC.Compiler.csproj
dotnet run --project src/FunctionalC.Compiler -- examples/sum_squares.fc --out generated/SampleApp/Program.fs
dotnet build generated/SampleApp/SampleApp.fsproj
dotnet run --no-build --project generated/SampleApp
```

The last command prints `55` and the process exits with code 0.

### Translating another file

```text
dotnet run --project src/FunctionalC.Compiler -- path/to/program.fc --out generated/SampleApp/Program.fs
dotnet build generated/SampleApp/SampleApp.fsproj
dotnet run --no-build --project generated/SampleApp
```

After the first build you can call the compiled translator directly:
`src/FunctionalC.Compiler/bin/Debug/net10.0/FunctionalC.Compiler.exe <input.fc> --out <output.fs>`
(on any OS: `dotnet src/FunctionalC.Compiler/bin/Debug/net10.0/FunctionalC.Compiler.dll ...`).

Translator exit codes:

| Code | Meaning |
| --- | --- |
| 0 | Translated |
| 1 | The source has errors. Diagnostics go to stderr and **the output file is left untouched**. |
| 2 | Bad command line |
| 3 | The input couldn't be read or the output couldn't be written |

The translator builds the whole F# file in memory and writes it only after every stage succeeds. It
writes to a temporary file next to the target and then renames that file over the target, so a failed
or interrupted translation never leaves a partial or corrupted `Program.fs`.

### Running the tests

```text
dotnet run --project tests/FunctionalC.Tests
```

Or run everything (workflow and tests) and save a results file:

```text
powershell -ExecutionPolicy Bypass -File .\verify.ps1
```

Each behavioral test goes through the full pipeline: the `.fc` file is translated by the translator CLI
code, the result is placed in its own F# project under `tests/work/`, built with `dotnet build` (with
**warnings treated as errors**, so the generated F# must compile cleanly), and then run. The test
checks stdout byte for byte, the exit code, and stderr where relevant. Rejection tests check the exit
code, the diagnostic code, the line and column, the message, and that no output file was written. The
harness also runs the compiler as a separate process. Options: `--filter <text>`, `--parallel <n>`
and `--report <file.md>`.

## The language (version 0)

```c
int sumSquares(int n)
{
    // Immutable binding to a lambda function.
    let square = (int x) => x * x;

    return n <= 0
        ? 0
        : square(n) + sumSquares(n - 1);
}

int main()
{
    let result = sumSquares(5);

    print(result);  // Expected output: 55
    return 0;
}
```

| Feature | Version 0 behavior |
| --- | --- |
| Types | `int` (signed 32-bit) and `bool`. Function parameters and return types are written explicitly. |
| Literals | Decimal integers, `true` and `false` |
| Functions | `type name(type a, type b) { ... }`. A function may call itself and any function defined **above** it. |
| Bindings | `let name = expression;`. The type is inferred and the binding is immutable. |
| Lambdas | `(int x) => expr`, `(int a, bool b) => expr`, `() => expr`. A lambda can capture enclosing bindings and parameters, call other lambdas in scope, and call the enclosing function. |
| Calls | `f(a, b)` for functions and let-bound lambdas |
| Operators | Unary `-` and `!`. Binary `* / %`, `+ -`, `< <= > >=`, `== !=`, `&&`, `\|\|`, and `?:` |
| Statements | `let`; a call statement `f(x);` (the result is discarded and the call still runs); `print(intExpr);`; and exactly one `return expr;`, which must come last |
| Comments | `// line` and `/* block */` (block comments don't nest) |
| Entry point | Exactly one `int main()` with no parameters. Its return value becomes the process exit code. |

### Precedence and associativity

Precedence follows C, from tightest to loosest:

| Level | Operators | Associativity |
| --- | --- | --- |
| 1 | call `f(...)` | — |
| 2 | unary `-`, `!` | right |
| 3 | `*` `/` `%` | left |
| 4 | `+` `-` | left |
| 5 | `<` `<=` `>` `>=` | left |
| 6 | `==` `!=` | left |
| 7 | `&&` | left |
| 8 | `\|\|` | left |
| 9 | `? :` | right |

### Typing rules

The translator checks these rules itself:

- Arithmetic and relational operators take `int` operands.
- `&&`, `||` and `!` take `bool` operands.
- `==` and `!=` need both operands to have the same type.
- A `?:` condition must be `bool`, and both branches must have the same type.
- There is **no implicit int-to-bool conversion**. Write `x != 0` instead of `x`.
- `print` takes one `int` argument and produces no value, so it can only be used as a statement.
- Call arity and argument types must match.
- A `return` value must match the declared return type.

### Scope rules

- Names must be unique within a function. In version 0 a local, parameter or lambda parameter may
  **not** shadow another visible name, including a function name.
- A `let` binding is not in scope inside its own initializer, so recursive lambdas aren't supported.
  Use a function instead.
- Forward references and mutual recursion are rejected with a dedicated message. Define a function
  before you use it.
- A lambda may appear only as the entire initializer of a `let`. Functions and lambdas aren't
  first-class values: they can be called but not passed or stored.

### Evaluation semantics

- **Left to right.** Call arguments and binary operands are evaluated left to right. Where the emitted
  F# would otherwise depend on F#'s own ordering, meaning two or more operands that could print,
  recurse or throw, the translator binds the earlier operands to fresh temporaries in source order.
  The tests cover argument, operand and comparison order.
- **Conditional expressions.** `c ? a : b` becomes `if c then a else b`, so only the selected branch is
  evaluated.
- **Short-circuiting.** `&&` and `||` become F#'s `&&` and `||`, which short-circuit. The right operand
  is never hoisted out.
- **`print(x)`** writes the decimal value in the invariant culture, followed by a single `\n`, on every
  OS. Program output goes to stdout; translator diagnostics go to stderr of a different process.

### Integer semantics

These are .NET's `int` semantics, not C's. They were verified by `tests/cases/run/integer_semantics.fc`
and the three runtime-exception tests.

| Case | Result |
| --- | --- |
| `+`, `-`, `*`, unary `-` on overflow | Wrap around (two's complement): `2147483647 + 1` is `-2147483648`, `-(-2147483648)` is `-2147483648`, and `2147483647 * 2` is `-2`. |
| `/` | Truncates toward zero: `-7 / 2` is `-3` and `7 / -2` is `-3`. |
| `%` | Takes the sign of the dividend: `-7 % 2` is `-1` and `7 % -2` is `1`. |
| `x / 0`, `x % 0` | Throws `System.DivideByZeroException`. The program dies with an unhandled exception. Observed exit code on Windows: `-1073741676` (`0xC0000094`). |
| `-2147483648 / -1`, `-2147483648 % -1` | Throws `System.OverflowException`. Observed exit code on Windows: `-1073741675` (`0xC0000095`). |
| Literals | `0` through `2147483647`. `-2147483648` is allowed only with the minus written directly before the digits, and `-(2147483648)` is rejected. Anything larger, and leading zeros (C octal), hex, suffixes and floating-point literals, are rejected with a diagnostic. |

C's undefined behavior is not modeled. There is no unsigned arithmetic and nothing from the C library.

## How the translation works

1. **Lexer** (`Lexer.cs`): turns the source into tokens carrying the file, line and column (a tab
   counts as one column). Multi-character operators are matched before their prefixes. Unsupported
   C tokens (`#`, `[`, `&`, `++`, `+=`, `->`, `.`, strings and so on) are rejected with a specific
   message instead of being skipped.
2. **Parser** (`Parser.cs`): recursive descent, with one method per precedence level, building the AST
   (`Ast.cs`). Lambdas and parentheses are told apart on purpose: `(int`, `(bool` or `() =>` starts a
   lambda, and anything else is a parenthesized expression. `(x) => ...` gets a hint that lambda
   parameters need types. C keywords such as `while`, `if`, `void` and `struct` produce
   "not supported in version 0" errors. The parser stops at the first syntax error.
3. **Checker** (`Checker.cs`): name resolution, scopes, immutability, declaration-before-use, the
   single final `return`, the `main` rules, arity and `int`/`bool` types. It reports every error it
   finds, sorted by position.
4. **Emitter** (`FSharpEmitter.cs`): produces deterministic F# with 4-space indentation and `\n` line
   endings, and parenthesizes every compound expression.

| Functional C | F# |
| --- | --- |
| `int f(int a, bool b) { ... }` | `let f (a: int) (b: bool) : int =` (curried, used the same way for functions, lambdas and calls) |
| a function that calls itself (also from inside a lambda) | `let rec f ...` |
| `int g() { ... }`, `g()` | `let g () : int =`, `(g ())` |
| `let x = e;` | `let x = e` |
| `(int x) => e`, `() => e` | `(fun (x: int) -> e)`, `(fun () -> e)` |
| `c ? a : b` | `(if c then a else b)` |
| `==`, `!=`, `!x` | `=`, `<>`, `(fc'not x)` |
| `f(x);` (call statement) | `fc'ignore (f x)` |
| `print(e);` | `fc'print e` |
| final `return e;` | `e` as the last line of the body |
| `int main()` | an ordinary `let main () : int =` plus a separate `[<EntryPoint>] let fc'entry (_args: string array) : int = main ()` |

**Names.** Identifiers are mapped consistently:

- F# keywords and reserved words are quoted: `match` becomes ``` ``match`` ``` and `type` becomes
  ``` ``type`` ```.
- Names that start with an uppercase letter or `_` get a `u'` prefix: `None` becomes `u'None` and
  `__LINE__` becomes `u'__LINE__`. Without it, F# would read them as union cases or active patterns
  (`None`, `Some`, `Ok`, `Failure`, ...) or special identifiers.
- Translator-internal names (`fc'print`, `fc'ignore`, `fc'not`, `fc'entry`, temporaries `fc't1`, ...)
  use an apostrophe. Functional C identifiers can't contain an apostrophe, so internal names can never
  collide with source names.
- `print` is reserved for the built-in.

Every generated file starts with a small runtime prelude (`fc'print`, `fc'ignore`, `fc'not`). The
prelude is defined before any user code, so user names can't shadow the .NET or FSharp.Core names it
uses.

## Diagnostics

Diagnostics use the conventional format `file(line,column): error CODE: message`, for example:

```text
tests/cases/reject/immutable_reassign.fc(4,5): error FC3002: cannot assign to 'x': local bindings are immutable ('x' was bound at 3:9)
```

| Code | Meaning |
| --- | --- |
| FC1001–FC1005 | Lexical: unexpected character, unterminated comment, integer out of range, invalid literal, unsupported C token |
| FC2001 | Syntax error, for example a missing `;`, `)` or `}`. The message includes the line and column. |
| FC2002 | Unsupported C feature: pointers, loops, `if`, `void`, casts, typed locals, nested blocks, ... |
| FC3001 | Unknown name |
| FC3002 | Assignment to an immutable binding |
| FC3003 | Duplicate name or shadowing |
| FC3004 | Forward reference or mutual recursion |
| FC3005 | `return` missing or not the last statement |
| FC3006 | Entry point: `main` missing, duplicated, not `int`, or has parameters |
| FC3007 | Type mismatch |
| FC3008 | Wrong number of arguments |
| FC3009 / FC3010 | A function used as a value / a value called as a function |
| FC3011 | An expression statement that isn't a call |
| FC3012 | `print` used as a name |
| FC3013 | `print(...)` used as a value |
| FC3014 | A lambda that isn't a `let` initializer |
| FC3015 | A `let` binding used in its own initializer |

**Where checking happens.** The translator checks every rule above, including the `int`/`bool` types
of the version 0 subset, before writing anything. The F# compiler then checks the generated code again
when you run `dotnet build`. This is a check for the v0 subset, not a proven-complete independent type
system. If F# ever reports an error, it refers to line numbers in the **generated** file (for example
`generated/SampleApp/Program.fs`), not the `.fc` source. That would mean a translator bug, so please
keep the `.fc` input that caused it.

## Limitations and known issues

- No pointers, arrays, structures, strings, chars, floating point, unsigned types, headers,
  preprocessor, loops, `if` statements, mutation (`=`, `++`, `+=`), early returns, global variables,
  nested blocks or function prototypes.
- No forward references or mutual recursion; a function can only call functions defined above it.
- No shadowing, recursive lambdas, or first-class functions (you can't pass or return a function).
- Recursion uses the .NET stack. Very deep non-tail recursion, such as `sumSquares(100000)`, can
  overflow it. The F# compiler may turn some tail calls into loops, but this isn't guaranteed and isn't
  tested.
- Columns count characters, so a tab is one column. Identifiers are ASCII only.
- The exit code is the value `main` returns. On Linux and macOS only the low 8 bits are visible to
  the shell.
- Runtime errors (division by zero, `MinValue / -1`) end the program with an unhandled .NET
  exception. There is no error handling in the language.
- `generated/SampleApp/Program.fs` is overwritten by every translation into it.
- Known translation bugs: none found by the current test suite (see `TEST-RESULTS.md`).

## Background

This folder was built afresh from the handoff document `Functional_C_Desktop_Handoff.md` one level up.
An earlier conversation claimed that .NET SDK 8.0.415, a translator and test runs existed. None of those
were available or verifiable, and none of that work is reused here. The SDK, target framework and
results recorded in `TEST-RESULTS.md` come from actual runs on this machine.
