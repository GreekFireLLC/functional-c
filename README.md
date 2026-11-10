# Functional C (prototype, version 1.2)

Functional C is a small language with C syntax and functional features: immutable `let` bindings and
lambdas alongside C's typed variables, `if`/`else` and loops. Its translator turns a `.fc` file into
ordinary F# source, and the standard .NET SDK compiles and runs that F#.

```
examples/sum_squares.fc ──► FunctionalC.Compiler (C#) ──► Program.fs ──► dotnet build ──► .NET program
```

Version history:

- **Version 0:** expressions, functions, immutable `let`, lambdas, `?:`.
- **Version 1:** C-style mutable variables, assignment, `if`/`else`, `while`/`do`/`for`,
  `break`/`continue` and `return` anywhere.
- **Version 1.1:** C-style conditions (`if (n)`, `while (1)`, `!n`, `a && b` with ints) and a set of
  example programs.
- **Version 1.2:** a `bool` can be used wherever an `int` is expected, as 1 or 0 (`int found = a < b;`).

It is still **not** compatible with arbitrary C programs: there are no pointers, arrays, structures,
strings, `switch`, headers or preprocessor.

## Requirements

- The **.NET SDK** (not only the runtime): <https://dotnet.microsoft.com/en-us/download>
- Built and tested with **.NET SDK 10.0.300**; every project targets **`net10.0`**. To use an older
  SDK, change `<TargetFramework>` in the three project files (`src/FunctionalC.Compiler`,
  `tests/FunctionalC.Tests` and `generated/SampleApp`).
- No third-party packages. The translator uses neither Roslyn nor a modified F# compiler.

## Layout

| Path | Contents |
| --- | --- |
| `src/FunctionalC.Compiler/` | The translator (C# console app): `Lexer.cs`, `Parser.cs`, `Ast.cs`, `Checker.cs`, `Flow.cs`, `FSharpEmitter.cs`, `Translator.cs`, `Cli.cs` |
| `examples/*.fc` | Example programs (see [Examples](#examples)) |
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

To translate another file, see [Examples](#examples). After the first build you can call the compiled
translator directly: `src/FunctionalC.Compiler/bin/Debug/net10.0/FunctionalC.Compiler.exe <input.fc> --out <output.fs>`
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

## Examples

Each file in `examples/` is a complete program. Except for the handoff's `sum_squares.fc`, each one ends
with its expected output in `//! out:` lines. Those values were computed separately in Python, not copied
from the translator's output. The test suite translates, compiles and runs every example and compares
the output.

| Program | What it computes | Shows |
| --- | --- | --- |
| `sum_squares.fc` | 1² + … + 5² = 55 | The handoff sample: recursion, a lambda, `?:` |
| `primes.fc` | The primes below 50, and how many are below 1000 (168) | `for`, early `return`, `count++` |
| `gcd_lcm.fc` | GCD of 1071 and 462, and the LCM of 1..20 (232792560) | Recursion vs. a loop, `while (b)` in C style |
| `fibonacci.fc` | Fibonacci numbers, including fib(46), the largest that fits in an int | Naive recursion vs. a loop, a `bool` flag |
| `collatz.fc` | Collatz sequence lengths and the longest one below 10000 | `while`, `?:`, `if`/`else` on one line, `n /= 2` |
| `perfect_numbers.fc` | The perfect numbers up to 10000, an amicable pair, and how many abundant numbers there are up to 100 | Divisor sums, chained `?:` |
| `digits.fc` | Digit sums, reversed numbers, digital roots, and palindromes below 10000 | `/` and `%` loops, counting with `palindromes += isPalindrome(n);` |
| `integer_sqrt.fc` | Integer square roots up to `isqrt(2147483647)` = 46340 | Binary search that can't overflow |
| `modular_power.fc` | `base^exponent mod m`, and the Fermat pseudoprimes 341, 561 and 645 | Square-and-multiply with a loop and with recursion |
| `fizzbuzz_numbers.fc` | FizzBuzz, printing -3, -5 and -15 in place of the words | Early returns, `else if` chains |
| `local_lambdas.fc` | Clamped squares, triangle numbers and sums of cubes | `let` lambdas calling each other and the enclosing function, copying a loop counter before capturing it |
| `ackermann.fc` | Ackermann's function for m ≤ 3 and n ≤ 4 | Deep nested recursion |

To run one, translate it into the sample project:

```text
dotnet run --project src/FunctionalC.Compiler -- examples/collatz.fc --out generated/SampleApp/Program.fs
dotnet build generated/SampleApp/SampleApp.fsproj
dotnet run --no-build --project generated/SampleApp
```

## Running the tests

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

## The language

```c
// Immutable bindings, lambdas and recursion (version 0)...
int sumSquares(int n)
{
    let square = (int x) => x * x;
    return n <= 0 ? 0 : square(n) + sumSquares(n - 1);
}

// ...and C-style variables, if and loops (version 1).
bool isPrime(int n)
{
    if (n < 2) return false;
    for (int d = 2; d * d <= n; d++) {
        if (n % d == 0) return false;
    }
    return true;
}

int main()
{
    print(sumSquares(5));          // 55
    int count = 0;
    for (int n = 2; n < 1000; n++) {
        if (isPrime(n)) count++;
    }
    print(count);                  // 168
    return 0;
}
```

| Feature | Behavior |
| --- | --- |
| Types | `int` (signed 32-bit) and `bool`. Function parameters and return types are written explicitly. |
| Literals | Decimal integers, `true` and `false` |
| Functions | `type name(type a, type b) { ... }`. A function may call itself and any function defined **above** it. |
| Immutable bindings | `let name = expression;`. The type is inferred, and the binding can never be assigned. |
| Mutable variables | `int x = 0;` and `bool done = false, seen = true;`, written C style with an initializer that is **required**. Function parameters are mutable local copies, as in C. |
| Assignment | `x = e;`, `x += e;`, `-=`, `*=`, `/=`, `%=`, and `x++;`, `++x;`, `x--;`, `--x;`. These are **statements**, not expressions. |
| Lambdas | `(int x) => expr`, `(int a, bool b) => expr`, `() => expr`. A lambda can capture enclosing `let` bindings, parameters and variables that are never assigned, call other lambdas in scope, and call the enclosing function. |
| Calls | `f(a, b)` for functions and let-bound lambdas |
| Operators | Unary `-` and `!`. Binary `* / %`, `+ -`, `< <= > >=`, `== !=`, `&&`, `\|\|`, and `?:` |
| Truth values | As in C, an `int` used as a condition (`if (n)`, `while (1)`, `n ? a : b`) or as an operand of `!`, `&&` or `\|\|` is true when it is not 0, including negative numbers. A `bool` used where an `int` is expected counts as 1 or 0 (`int found = a < b;`, `count += x > 0;`, `return a == b;`). An `int` is not converted when it is stored in a `bool`. |
| `if` | `if (c) stmt`, `if (c) stmt else stmt`, `else if` chains. Braces are optional, and a dangling `else` binds to the nearest `if`. |
| Loops | `while (c) stmt`, `do stmt while (c);`, `for (init; cond; update) stmt`. The `for` initializer can be declarations (`int i = 0, j = 10`) or assignments, the update can be several statements separated by commas, and any part can be empty (`for (;;)`). |
| Jumps | `break;` and `continue;` (innermost loop), and `return expr;` anywhere |
| Blocks | `{ ... }` with its own scope, plus the empty statement `;` |
| Other statements | Call statements `f(x);` (the result is discarded and the call still runs) and `print(intExpr);` |
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

Assignment and `++`/`--` aren't in this table because they are statements, not operators.

### Typing rules

The translator checks these rules itself:

- **Int as a truth value.** The conditions of `if`, `while`, `do`, `for` and `?:`, and the operands of
  `&&`, `||` and `!`, may be `bool` or `int`. An `int` is true when it is not 0, as in C, so
  `while (1)`, `if (n)`, `if (!n)` and `if (n % 2 && ok)` all work. `&&`, `||`, `!` and the comparisons
  produce a `bool`.
- **Bool as an int.** As in C, a `bool` can be used wherever an `int` is expected, with `true` as 1 and
  `false` as 0:
  - initializing, assigning or compound-assigning an `int` variable (`int found = a < b;`,
    `count += n % 2 == 0;`)
  - passing it to an `int` parameter, or returning it from an `int` function (`return a == b;`)
  - `print(flag)`, which prints 1 or 0
  - an operand of `+ - * / %`, unary `-`, and `< <= > >=`, as in `(a < b) + (c < d)`
- **`==` and `!=`.** Two `bool`s compare as `bool`s. A `bool` compared with an `int` counts as 0 or 1,
  so `n == true` means `n == 1` and is false when `n` is 2. That is the same pitfall as in C; write
  `if (n)` instead.
- **`?:`.** Both branches have the same type, or one is `int` and the other `bool`, in which case the
  result is an `int`.
- **What isn't converted.** An `int` is never converted to `bool` when it is stored, passed or returned:
  `bool b = 1;`, `b = 0;`, `isSet(1)` for a `bool` parameter, and `return n;` from a `bool` function are
  rejected. C would accept them. Write `true`, `false` or `n != 0`.
- `+= -= *= /= %=` and `++`/`--` need an `int` variable.
- `print` takes one argument and produces no value, so it can only be used as a statement.
- Call arity must match.

### Scope and mutability rules

- **Blocks.** Each block (`{ }`, an `if` branch or a loop body) has its own scope. Variables declared in a
  `for` initializer belong to the loop.
- **No shadowing.** A name may not redeclare or shadow a visible name (an enclosing block's variable, a
  parameter, a function, or `print`). Sibling blocks may reuse names, so two consecutive
  `for (int i = ...)` loops are fine.
- **What can be assigned.** `let` bindings, lambda parameters, lambdas and functions can never be
  assigned. Typed variables (`int x = ...`) and function parameters can.
- **Lambda captures.** A lambda may not capture a variable that is assigned anywhere in the function,
  because F# closures can't capture mutable locals. Copy the value first: `let now = x;` and then use
  `now` in the lambda.
- **Self-reference.** A binding or variable is not in scope inside its own initializer.
- **Declaration order.** Forward references and mutual recursion are rejected with a dedicated message.
  Define a function before you use it.
- **Lambda placement.** A lambda may appear only as the entire initializer of a `let`. Functions and
  lambdas aren't first-class values.

### Control-flow rules

- Every path through a function must end in `return`. Falling off the end is an error, not C's
  undefined behavior. A loop like `while (true)`, `while (1)` or `for (;;)` with no `break` never
  finishes, so the checker doesn't require a `return` after it. Only a literal counts as always true:
  the checker doesn't evaluate other constant expressions such as `while (2 > 1)`.
- Code that can never run, such as a statement after `return`, `break` or `continue`, or after an
  infinite loop, is an **error**.
- `break` and `continue` must be inside a loop and apply to the innermost one. In a `for` loop,
  `continue` still runs the update. In a `do` loop, `continue` jumps to the condition.

### Evaluation semantics

- **Left to right.** Call arguments and binary operands are evaluated left to right. Where the emitted
  F# would otherwise depend on F#'s own ordering, meaning two or more operands that could print,
  recurse or throw, the translator binds the earlier operands to fresh temporaries in source order.
- **Conditional expressions.** `c ? a : b` evaluates only the selected branch.
- **Short-circuiting.** `&&` and `||` short-circuit.
- **Loop conditions.** A `while` or `for` condition is evaluated once before each iteration, including
  the final test that fails. It is not evaluated again after `break` or `return`.
- **`print(x)`** writes the decimal value in the invariant culture, followed by a single `\n`, on every
  OS. Program output goes to stdout; translator diagnostics go to stderr of a different process.

### Integer semantics

These are .NET's `int` semantics, not C's. They were verified by `tests/cases/run/integer_semantics.fc`,
`mutable_variables.fc` and the three runtime-exception tests. Compound assignments behave like the
matching operator.

| Case | Result |
| --- | --- |
| `+`, `-`, `*`, unary `-` on overflow | Wrap around (two's complement): `2147483647 + 1` is `-2147483648`, `-(-2147483648)` is `-2147483648`, and `2147483647 * 2` is `-2`. |
| `/` | Truncates toward zero: `-7 / 2` is `-3` and `7 / -2` is `-3`. |
| `%` | Takes the sign of the dividend: `-7 % 2` is `-1` and `7 % -2` is `1`. |
| `x / 0`, `x % 0` | Throws `System.DivideByZeroException`. The program dies with an unhandled exception. Observed exit code on Windows: `-1073741676` (`0xC0000094`). |
| `-2147483648 / -1`, `-2147483648 % -1` | Throws `System.OverflowException`. Observed exit code on Windows: `-1073741675` (`0xC0000095`). |
| Literals | `0` through `2147483647`. `-2147483648` is allowed only with the minus written directly before the digits, and `-(2147483648)` is rejected. Anything larger, and leading zeros (C octal), hex, suffixes and floating-point literals, are rejected with a diagnostic. |

## How the translation works

1. **Lexer** (`Lexer.cs`): turns the source into tokens carrying the file, line and column (a tab
   counts as one column). Multi-character operators are matched before their prefixes. Unsupported
   C tokens (`#`, `[`, `&`, `->`, `.`, `<<`, strings and so on) are rejected with a specific message.
2. **Parser** (`Parser.cs`): recursive descent, with one method per precedence level, building the AST
   (`Ast.cs`). Lambdas and parentheses are told apart on purpose: `(int`, `(bool` or `() =>` starts a
   lambda. Assignment and `++`/`--` are recognized only as statements; inside an expression they get a
   targeted error ("did you mean '=='?"). The parser stops at the first syntax error.
3. **Checker** (`Checker.cs`, `Flow.cs`): name resolution, block scopes, mutability and capture rules,
   declaration-before-use, `break`/`continue` placement, unreachable code, the "every path returns"
   rule, the `main` rules, arity and types. It reports every error it finds, sorted by position.
4. **Emitter** (`FSharpEmitter.cs`): produces deterministic F# with 4-space indentation and `\n` line
   endings, and parenthesizes every compound expression.

| Functional C | F# |
| --- | --- |
| `int f(int a, bool b) { ... }` | `let f (a: int) (b: bool) : int =` (curried, used the same way for functions, lambdas and calls) |
| a function that calls itself (also from inside a lambda) | `let rec f ...` |
| `let x = e;` | `let x = e` |
| `int x = e;` | `let mutable x = e` if `x` is ever assigned, otherwise `let x = e` |
| an assigned parameter `n` | `let mutable n = n` at the start of the body |
| `x = e;`, `x += e;`, `x++;` | `x <- e`, `x <- (x + e)`, `x <- (x + 1)` |
| `if (c) A else if (d) B else C` | `if c then A elif d then B else C` |
| `while (c) S` | `while c do S` |
| `do S while (c);` | `let mutable fc'againN = true` / `while fc'againN do S; fc'againN <- c` |
| `for (I; c; U) S` | `I` / `while c do S; U` |
| `(int x) => e`, `() => e` | `(fun (x: int) -> e)`, `(fun () -> e)` |
| `c ? a : b` | `(if c then a else b)` |
| `==`, `!=`, `!x` | `=`, `<>`, `(fc'not x)` |
| an `int` used as a truth value: `if (n)`, `!n`, `n && m` | `if (n <> 0) then`, `(n = 0)`, `((n <> 0) && (m <> 0))`. A literal becomes `true` or `false`, so `while (1)` becomes `while true do`. |
| a `bool` used as an `int`: `int found = a < b;`, `print(true);` | `let found = (if (a < b) then 1 else 0)`, `fc'print 1` |
| `f(x);` (call statement) | `fc'ignore (f x)` |
| `print(e);` | `fc'print e` |
| `int main()` | an ordinary `let main () : int =` plus a separate `[<EntryPoint>] let fc'entry (_args: string array) : int = main ()` |

**`return`, `break` and `continue`.** F# has none of these, so each function body is emitted in one
of two modes:

- **Expression mode** is used when every `return` is in a position F# can express directly. Either the
  code runs straight through to a final `return`, or an `if` has one branch that always returns, in
  which case the statements after the `if` move into the other branch without duplicating any code.
  Most functions look like hand-written F#:

  ```fsharp
  let sign (x: int) : int =
      if (x < 0) then
          (-1)
      elif (x = 0) then
          0
      else
          1
  ```

- **Flag mode** is used otherwise, meaning a `return` inside a loop or inside an `if` that only
  sometimes returns. The value goes into `fc'result`, and `fc'returned` records that a return happened.
  Statements that may follow a `return`, `break` or `continue` are guarded by these flags, and loop
  conditions test the flags first.

  ```fsharp
  let firstMultiple (start: int) (k: int) : int =
      let mutable start = start
      let mutable fc'result = 0
      let mutable fc'returned = false
      while (fc'not fc'returned) do
          if ((start % k) = 0) then
              fc'result <- start
              fc'returned <- true
          if (fc'not fc'returned) then
              start <- (start + 1)
      fc'result
  ```

`break` and `continue` use per-loop flags (`fc'brkN`, `fc'contN`) in both modes. Loops become real
.NET `while` loops, so a million iterations don't grow the stack (`tests/cases/run/million_iterations.fc`).

**Names.** Identifiers are mapped consistently:

- F# keywords and reserved words are quoted: `match` becomes ``` ``match`` ``` and `type` becomes
  ``` ``type`` ```.
- Names that start with an uppercase letter or `_` get a `u'` prefix: `None` becomes `u'None` and
  `__LINE__` becomes `u'__LINE__`. Without it, F# would read them as union cases or active patterns
  (`None`, `Some`, `Ok`, `Failure`, ...) or special identifiers.
- Translator-internal names (`fc'print`, `fc'ignore`, `fc'not`, `fc'entry`, `fc'result`, `fc'brk1`,
  temporaries `fc't1`, ...) use an apostrophe. Functional C identifiers can't contain an apostrophe,
  so internal names can never collide with source names.
- `print` is reserved for the built-in.

Every generated file starts with a small runtime prelude (`fc'print`, `fc'ignore`, `fc'not`). The
prelude is defined before any user code, so user names (even a user function named `not`) can't
shadow what the generated code relies on.

## Diagnostics

Diagnostics use the conventional format `file(line,column): error CODE: message`, for example:

```text
tests/cases/reject/immutable_reassign.fc(4,5): error FC3002: cannot assign to 'x': 'let' bindings are immutable ('x' was bound at 3:9); declare it as 'int x = ...;' if it needs to change
```

| Code | Meaning |
| --- | --- |
| FC1001–FC1005 | Lexical: unexpected character, unterminated comment, integer out of range, invalid literal, unsupported C token |
| FC2001 | Syntax error, for example a missing `;`, `)` or `}`, or an `else` without an `if`. The message includes the line and column. |
| FC2002 | Unsupported C feature: pointers, `switch`, `goto`, `void`, casts, uninitialized variables, assignment or `++` inside an expression, ... |
| FC3001 | Unknown name (including assignment to an undeclared name) |
| FC3002 | Assignment to something immutable: a `let` binding, lambda parameter, lambda or function |
| FC3003 | Duplicate name or shadowing |
| FC3004 | Forward reference or mutual recursion |
| FC3005 | A function can reach its end without returning a value |
| FC3006 | Entry point: `main` missing, duplicated, not `int`, or has parameters |
| FC3007 | Type mismatch: an `int` stored in, passed as or returned as a `bool`, or `+=` or `++` on a `bool` variable |
| FC3008 | Wrong number of arguments |
| FC3009 / FC3010 | A function used as a value / a value called as a function |
| FC3011 | An expression statement that isn't a call, assignment or increment |
| FC3012 | `print` used as a name |
| FC3013 | `print(...)` used as a value |
| FC3014 | A lambda that isn't a `let` initializer |
| FC3015 | A binding or variable used in its own initializer |
| FC3016 | Unreachable statement |
| FC3017 | A lambda capturing a variable that is assigned |
| FC3018 | `break` or `continue` outside a loop |

**Where checking happens.** The translator checks every rule above before writing anything. The F#
compiler then checks the generated code again when you run `dotnet build`. This is a check for the
supported subset, not a proven-complete independent type system. If F# ever reports an error, it
refers to line numbers in the **generated** file (for example `generated/SampleApp/Program.fs`), not
the `.fc` source. That would mean a translator bug, so please keep the `.fc` input that caused it.

## Limitations and known issues

- No pointers, arrays, structures, strings, chars, floating point, unsigned types, bitwise or shift
  operators, headers, preprocessor, `switch`, `goto`, global variables or function prototypes.
- Assignment and `++`/`--` are statements only, and there is no comma operator, so
  `while ((c = next()) != 0)` and `a[i++]`-style code isn't supported. The comma is allowed only between
  declarators and in a `for` header.
- Variables must be initialized when declared.
- An `int` is not converted to `bool` when it is stored, passed or returned (see
  [Typing rules](#typing-rules)). So `bool b = 1;` and `return n;` from a `bool` function are
  rejected, even though C accepts them. The other direction, `bool` to `int`, works as in C.
- No forward references or mutual recursion; a function can only call functions defined above it.
- No shadowing, recursive lambdas, or first-class functions (you can't pass or return a function). A
  lambda can't capture a variable that is assigned.
- Deep non-tail *recursion* (not loops) can overflow the .NET stack, for example `sumSquares(100000)`.
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
were available or verifiable, and none of that work is reused here. Version 1 then added C-style
mutable variables, `if` and loops, keeping `let` immutable. Version 1.1 added C-style int conditions
and the example programs, and version 1.2 let a `bool` be used as an `int`. The SDK, target framework
and results recorded in `TEST-RESULTS.md` come from actual runs on this machine.
