using System.Diagnostics;
using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;
using FunctionalC.Compiler;

namespace FunctionalC.Tests;

/// <summary>
/// Acceptance-test harness (no test framework). Behavioral tests go through the whole pipeline:
/// .fc file → translator CLI → generated .fs → `dotnet build` (warnings are errors) → run the program.
/// Rejection tests check the CLI's exit code, diagnostics (code, line, column, message) and that no output was written.
///
/// Usage: dotnet run --project tests/FunctionalC.Tests -- [--report path.md] [--filter text] [--parallel n]
/// </summary>
internal static partial class Program
{
    private static string _root = "";
    private static string _work = "";
    private static string _targetFramework = "";

    private static async Task<int> Main(string[] args)
    {
        string? reportPath = null;
        string? filter = null;
        int parallel = Math.Clamp(Environment.ProcessorCount / 2, 1, 4);
        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--report": reportPath = Path.GetFullPath(args[++i]); break;
                case "--filter": filter = args[++i]; break;
                case "--parallel": parallel = int.Parse(args[++i], CultureInfo.InvariantCulture); break;
                default:
                    Console.Error.WriteLine($"unknown argument '{args[i]}'");
                    return 2;
            }
        }

        _root = FindRoot();
        Directory.SetCurrentDirectory(_root); // diagnostics then show paths such as tests/cases/reject/x.fc
        _work = Path.Combine(_root, "tests", "work");
        _targetFramework = $"net{Environment.Version.Major}.{Environment.Version.Minor}";
        if (Directory.Exists(_work))
        {
            Directory.Delete(_work, recursive: true);
        }

        Directory.CreateDirectory(_work);

        var tests = DiscoverTests()
            .Where(t => filter is null || t.Name.Contains(filter, StringComparison.OrdinalIgnoreCase))
            .ToList();

        Console.WriteLine($"Running {tests.Count} tests (parallelism {parallel}) from {_root}");
        var results = new TestResult[tests.Count];
        var started = Stopwatch.StartNew();
        await Parallel.ForEachAsync(
            Enumerable.Range(0, tests.Count),
            new ParallelOptions { MaxDegreeOfParallelism = parallel },
            async (index, _) =>
            {
                results[index] = await RunTest(tests[index]);
                var r = results[index];
                Console.WriteLine($"[{r.Status,-4}] {r.Test.Name} ({r.Duration.TotalSeconds:0.0}s){(r.Status == "FAIL" ? " -- " + FirstLine(r.Detail) : "")}");
            });
        started.Stop();

        int passed = results.Count(r => r.Status == "PASS");
        int failed = results.Count(r => r.Status == "FAIL");
        int skipped = results.Count(r => r.Status == "SKIP");
        Console.WriteLine();
        Console.WriteLine($"Passed: {passed}  Failed: {failed}  Skipped: {skipped}  Total: {results.Length}  ({started.Elapsed.TotalSeconds:0.0}s)");
        foreach (var failure in results.Where(r => r.Status == "FAIL"))
        {
            Console.WriteLine();
            Console.WriteLine($"FAILED: {failure.Test.Name}");
            Console.WriteLine(failure.Detail);
        }

        if (reportPath is not null)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(reportPath)!);
            File.WriteAllText(reportPath, BuildReport(results, passed, failed, skipped, started.Elapsed));
            Console.WriteLine($"Report written to {reportPath}");
        }

        return failed == 0 ? 0 : 1;
    }

    private static string FirstLine(string text) => text.Split('\n')[0].TrimEnd('\r');

    private static string FindRoot()
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
        {
            if (File.Exists(Path.Combine(dir.FullName, "examples", "sum_squares.fc")))
            {
                return dir.FullName;
            }
        }

        throw new InvalidOperationException("could not find the FunctionalC root (examples/sum_squares.fc)");
    }

    // ------------------------------------------------------------ test model

    private sealed record TestCase(string Name, string Category, Func<Task<string>> Body);

    private sealed record TestResult(TestCase Test, string Status, string Detail, TimeSpan Duration);

    private sealed class TestFailure(string message) : Exception(message);

    private sealed class TestSkipped(string message) : Exception(message);

    private static async Task<TestResult> RunTest(TestCase test)
    {
        var clock = Stopwatch.StartNew();
        try
        {
            string detail = await test.Body();
            return new TestResult(test, "PASS", detail, clock.Elapsed);
        }
        catch (TestSkipped skip)
        {
            return new TestResult(test, "SKIP", skip.Message, clock.Elapsed);
        }
        catch (Exception e)
        {
            string detail = e is TestFailure ? e.Message : $"harness exception: {e}";
            return new TestResult(test, "FAIL", detail, clock.Elapsed);
        }
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new TestFailure(message);
        }
    }

    private static IEnumerable<TestCase> DiscoverTests()
    {
        yield return new TestCase("example: examples/sum_squares.fc prints 55", "end-to-end",
            () => RunProgramCase("examples/sum_squares.fc", "example-sum_squares",
                new CaseSpec { Out = ["55"], Exit = "0" }));

        foreach (var file in Directory.GetFiles(Path.Combine(_root, "tests", "cases", "run"), "*.fc").Order(StringComparer.Ordinal))
        {
            string relative = Relative(file);
            string name = Path.GetFileNameWithoutExtension(file);
            yield return new TestCase($"run: {name}", "end-to-end",
                () => RunProgramCase(relative, "run-" + name, CaseSpec.Parse(File.ReadAllLines(file))));
        }

        foreach (var file in Directory.GetFiles(Path.Combine(_root, "tests", "cases", "reject"), "*.fc").Order(StringComparer.Ordinal))
        {
            string relative = Relative(file);
            string name = Path.GetFileNameWithoutExtension(file);
            yield return new TestCase($"reject: {name}", "translator rejects",
                () => Task.FromResult(RunRejectCase(relative, "reject-" + name, CaseSpec.Parse(File.ReadAllLines(file)))));
        }

        yield return new TestCase("cli: failed translation leaves the existing output file untouched", "cli", FailedTranslationPreservesOutput);
        yield return new TestCase("cli: compiler run as a separate process (dotnet FunctionalC.Compiler.dll)", "end-to-end", CompilerAsSeparateProcess);
        yield return new TestCase("cli: usage and I/O errors give distinct exit codes", "cli", () => Task.FromResult(UsageErrors()));
        yield return new TestCase("translator: output is deterministic", "translator", () => Task.FromResult(DeterministicOutput()));
        yield return new TestCase("translator: sum_squares emits let rec, a curried lambda, and a separate entry point", "translator", () => Task.FromResult(SumSquaresShape()));
        yield return new TestCase("lexer: multi-character operators win over their prefixes", "translator", () => Task.FromResult(LexerOperators()));
        yield return new TestCase("reference: hand-written examples/sum_squares.reference.fs (not translator output)", "reference", ReferenceFixture);
    }

    private static string Relative(string path) => Path.GetRelativePath(_root, path).Replace('\\', '/');

    // ------------------------------------------------------------ case files

    private sealed partial class CaseSpec
    {
        public List<string> Out { get; init; } = [];
        public string Exit { get; set; } = "0";
        public List<string> StderrContains { get; } = [];
        public List<string> FsContains { get; } = [];
        public List<ExpectedError> Errors { get; } = [];

        public static CaseSpec Parse(string[] lines)
        {
            var spec = new CaseSpec();
            foreach (var raw in lines)
            {
                var line = raw.Trim();
                if (!line.StartsWith("//!", StringComparison.Ordinal))
                {
                    continue;
                }

                int colon = line.IndexOf(':');
                string key = line[3..colon].Trim();
                string value = line[(colon + 1)..].Trim();
                switch (key)
                {
                    case "out": spec.Out.Add(value); break;
                    case "exit": spec.Exit = value; break;
                    case "stderr-contains": spec.StderrContains.Add(value); break;
                    case "fs-contains": spec.FsContains.Add(value); break;
                    case "error":
                        var m = ErrorDirective().Match(value);
                        if (!m.Success)
                        {
                            throw new InvalidOperationException($"bad error directive: {value}");
                        }

                        spec.Errors.Add(new ExpectedError(m.Groups[1].Value, int.Parse(m.Groups[2].Value, CultureInfo.InvariantCulture),
                            int.Parse(m.Groups[3].Value, CultureInfo.InvariantCulture), m.Groups[4].Value));
                        break;
                    default:
                        throw new InvalidOperationException($"unknown directive '{key}'");
                }
            }

            return spec;
        }

        [GeneratedRegex(@"^(FC\d{4}) (\d+):(\d+) (.+)$")]
        private static partial Regex ErrorDirective();
    }

    private sealed record ExpectedError(string Code, int Line, int Column, string Contains)
    {
        public override string ToString() => $"{Code} at {Line}:{Column} containing \"{Contains}\"";
    }

    private sealed record ParsedDiagnostic(string File, int Line, int Column, string Code, string Message);

    [GeneratedRegex(@"^(?<file>.+)\((?<line>\d+),(?<col>\d+)\): error (?<code>FC\d{4}): (?<msg>.*)$")]
    private static partial Regex DiagnosticLine();

    private static List<ParsedDiagnostic> ParseDiagnostics(string stderr) =>
        stderr.Split('\n')
            .Select(l => DiagnosticLine().Match(l.TrimEnd('\r')))
            .Where(m => m.Success)
            .Select(m => new ParsedDiagnostic(m.Groups["file"].Value, int.Parse(m.Groups["line"].Value, CultureInfo.InvariantCulture),
                int.Parse(m.Groups["col"].Value, CultureInfo.InvariantCulture), m.Groups["code"].Value, m.Groups["msg"].Value))
            .ToList();

    // ------------------------------------------------------------ pipeline helpers

    private static (int Exit, string Stdout, string Stderr) TranslateWithCli(string input, string output)
    {
        var stdout = new StringWriter();
        var stderr = new StringWriter();
        int exit = Cli.Run([input, "--out", output], stdout, stderr);
        return (exit, stdout.ToString(), stderr.ToString());
    }

    private static string CreateFSharpProject(string directory)
    {
        Directory.CreateDirectory(directory);
        string project = Path.Combine(directory, "App.fsproj");
        File.WriteAllText(project,
            $"""
            <Project Sdk="Microsoft.NET.Sdk">
              <PropertyGroup>
                <OutputType>Exe</OutputType>
                <TargetFramework>{_targetFramework}</TargetFramework>
                <AssemblyName>App</AssemblyName>
                <TreatWarningsAsErrors>true</TreatWarningsAsErrors>
                <SatelliteResourceLanguages>en</SatelliteResourceLanguages>
              </PropertyGroup>
              <ItemGroup>
                <Compile Include="Program.fs" />
              </ItemGroup>
            </Project>
            """);
        return project;
    }

    private static async Task<(int Exit, string Stdout, string Stderr)> BuildAndRun(string directory, string project)
    {
        var build = await RunProcess("dotnet", ["build", project, "--nologo", "-v", "q", "-nodeReuse:false"], directory, TimeSpan.FromMinutes(5));
        Assert(build.Exit == 0, $"F# build failed (exit {build.Exit}):\n{build.Stdout}\n{build.Stderr}\n--- generated F# ---\n{File.ReadAllText(Path.Combine(directory, "Program.fs"))}");
        string dll = Path.Combine(directory, "bin", "Debug", _targetFramework, "App.dll");
        return await RunProcess("dotnet", [dll], directory, TimeSpan.FromSeconds(60));
    }

    private static async Task<(int Exit, string Stdout, string Stderr)> RunProcess(string fileName, IEnumerable<string> arguments, string workingDirectory, TimeSpan timeout)
    {
        var info = new ProcessStartInfo(fileName)
        {
            WorkingDirectory = workingDirectory,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
        };
        foreach (var argument in arguments)
        {
            info.ArgumentList.Add(argument);
        }

        info.Environment["DOTNET_NOLOGO"] = "1";
        info.Environment["DOTNET_CLI_TELEMETRY_OPTOUT"] = "1";
        info.Environment["MSBUILDDISABLENODEREUSE"] = "1";

        using var process = Process.Start(info) ?? throw new InvalidOperationException($"could not start {fileName}");
        var stdoutTask = process.StandardOutput.ReadToEndAsync();
        var stderrTask = process.StandardError.ReadToEndAsync();
        using var cts = new CancellationTokenSource(timeout);
        try
        {
            await process.WaitForExitAsync(cts.Token);
        }
        catch (OperationCanceledException)
        {
            process.Kill(entireProcessTree: true);
            throw new TestFailure($"'{fileName} {string.Join(' ', arguments)}' timed out after {timeout.TotalSeconds}s");
        }

        return (process.ExitCode, await stdoutTask, await stderrTask);
    }

    private static string Normalize(string text) => text.Replace("\r\n", "\n");

    private static string Block(string label, string text) =>
        $"{label}:\n```text\n{(text.Length == 0 ? "(empty)" : text.TrimEnd('\n', '\r'))}\n```";

    // ------------------------------------------------------------ behavioral (end-to-end) cases

    private static async Task<string> RunProgramCase(string input, string workName, CaseSpec spec)
    {
        string directory = Path.Combine(_work, workName);
        string project = CreateFSharpProject(directory);
        string output = Path.Combine(directory, "Program.fs");

        var translate = TranslateWithCli(input, output);
        Assert(translate.Exit == Cli.Success, $"translation of {input} failed (exit {translate.Exit}):\n{translate.Stderr}");

        string fsharp = File.ReadAllText(output);
        foreach (var fragment in spec.FsContains)
        {
            Assert(fsharp.Contains(fragment, StringComparison.Ordinal), $"generated F# does not contain '{fragment}':\n{fsharp}");
        }

        var run = await BuildAndRun(directory, project);
        string expectedStdout = string.Concat(spec.Out.Select(l => l + "\n"));
        string summary = $"exit code {run.Exit}; stdout {(run.Stdout.Length == 0 ? "(empty)" : string.Join(" | ", run.Stdout.TrimEnd('\n').Split('\n')))}";

        // Exact comparison: print writes "\n" (not the platform newline), so stdout must match byte for byte.
        Assert(run.Stdout == expectedStdout,
            $"stdout mismatch for {input}\n{Block("expected", expectedStdout)}\n{Block("actual", run.Stdout)}\n{Block("stderr", run.Stderr)}");

        if (spec.Exit == "nonzero")
        {
            Assert(run.Exit != 0, $"expected a nonzero exit code but got 0");
        }
        else
        {
            int expectedExit = int.Parse(spec.Exit, CultureInfo.InvariantCulture);
            Assert(run.Exit == expectedExit, $"expected exit code {expectedExit} but got {run.Exit}\n{Block("stderr", run.Stderr)}");
        }

        foreach (var fragment in spec.StderrContains)
        {
            Assert(run.Stderr.Contains(fragment, StringComparison.Ordinal), $"stderr does not contain '{fragment}':\n{run.Stderr}");
        }

        if (spec.StderrContains.Count > 0)
        {
            summary += $"; stderr: {FirstLine(run.Stderr)}";
        }

        return summary;
    }

    // ------------------------------------------------------------ rejection cases

    private static string RunRejectCase(string input, string workName, CaseSpec spec)
    {
        Assert(spec.Errors.Count > 0, $"{input} has no '//! error:' directive");
        string directory = Path.Combine(_work, workName);
        Directory.CreateDirectory(directory);
        string output = Path.Combine(directory, "Program.fs");

        var translate = TranslateWithCli(input, output);
        Assert(translate.Exit == Cli.SourceErrors, $"expected exit code {Cli.SourceErrors} but got {translate.Exit}\n{translate.Stdout}{translate.Stderr}");
        Assert(!File.Exists(output), "the output file was written even though translation failed");
        Assert(Directory.GetFiles(directory).Length == 0, $"translation left files behind: {string.Join(", ", Directory.GetFiles(directory))}");

        var diagnostics = ParseDiagnostics(translate.Stderr);
        Assert(diagnostics.Count > 0, $"no diagnostics in the expected 'file(line,col): error FCnnnn: message' format:\n{translate.Stderr}");
        foreach (var d in diagnostics)
        {
            Assert(d.File == input, $"diagnostic names file '{d.File}' instead of '{input}'");
        }

        foreach (var expected in spec.Errors)
        {
            bool found = diagnostics.Any(d => d.Code == expected.Code && d.Line == expected.Line && d.Column == expected.Column
                                              && d.Message.Contains(expected.Contains, StringComparison.Ordinal));
            Assert(found, $"expected {expected}\nactual stderr:\n{translate.Stderr}");
        }

        return string.Join("\n", translate.Stderr.TrimEnd().Split('\n').Select(l => l.TrimEnd('\r')).Where(l => DiagnosticLine().IsMatch(l)));
    }

    // ------------------------------------------------------------ other tests

    private static Task<string> FailedTranslationPreservesOutput()
    {
        string directory = Path.Combine(_work, "cli-preserve");
        Directory.CreateDirectory(directory);
        string output = Path.Combine(directory, "Program.fs");

        var good = TranslateWithCli("examples/sum_squares.fc", output);
        Assert(good.Exit == Cli.Success, $"good translation failed:\n{good.Stderr}");
        string before = File.ReadAllText(output);
        var stampBefore = File.GetLastWriteTimeUtc(output);

        string bad = Path.Combine(directory, "bad.fc");
        File.WriteAllText(bad, "int main()\n{\n    let x = 1;\n    x = 2;\n    return x;\n}\n");
        var failed = TranslateWithCli(bad, output);
        Assert(failed.Exit == Cli.SourceErrors, $"bad translation should exit {Cli.SourceErrors} but exited {failed.Exit}");
        Assert(File.ReadAllText(output) == before, "the existing output file was modified by a failed translation");
        Assert(File.GetLastWriteTimeUtc(output) == stampBefore, "the existing output file's timestamp changed");

        string syntaxError = Path.Combine(directory, "syntax.fc");
        File.WriteAllText(syntaxError, "int main()\n{\n    return 0\n}\n");
        var failed2 = TranslateWithCli(syntaxError, output);
        Assert(failed2.Exit == Cli.SourceErrors, $"syntax error translation should exit {Cli.SourceErrors} but exited {failed2.Exit}");
        Assert(File.ReadAllText(output) == before, "the existing output file was modified by a failed (syntax error) translation");

        var leftovers = Directory.GetFiles(directory).Select(Path.GetFileName).Where(f => f!.EndsWith(".tmp", StringComparison.Ordinal)).ToList();
        Assert(leftovers.Count == 0, $"temporary files were left behind: {string.Join(", ", leftovers)}");
        return Task.FromResult($"output unchanged after two failed translations; stderr of the first:\n{failed.Stderr.TrimEnd()}");
    }

    private static async Task<string> CompilerAsSeparateProcess()
    {
        // The compiler built next to this harness (ProjectReference copies it with its runtimeconfig).
        string dll = Path.Combine(AppContext.BaseDirectory, "FunctionalC.Compiler.dll");
        string runtimeConfig = Path.Combine(AppContext.BaseDirectory, "FunctionalC.Compiler.runtimeconfig.json");
        if (!File.Exists(runtimeConfig))
        {
            throw new TestSkipped($"{runtimeConfig} not found next to the harness; cannot launch the compiler as its own process");
        }

        string directory = Path.Combine(_work, "cli-process");
        string project = CreateFSharpProject(directory);
        string output = Path.Combine(directory, "Program.fs");
        var translate = await RunProcess("dotnet", [dll, "examples/sum_squares.fc", "--out", output], _root, TimeSpan.FromMinutes(1));
        Assert(translate.Exit == 0, $"compiler process exited {translate.Exit}:\n{translate.Stdout}\n{translate.Stderr}");

        var run = await BuildAndRun(directory, project);
        Assert(Normalize(run.Stdout) == "55\n" && run.Exit == 0, $"expected 55 / exit 0 but got exit {run.Exit}:\n{run.Stdout}\n{run.Stderr}");

        var bad = await RunProcess("dotnet", [dll, "tests/cases/reject/immutable_reassign.fc", "--out", Path.Combine(directory, "bad.fs")], _root, TimeSpan.FromMinutes(1));
        Assert(bad.Exit == Cli.SourceErrors, $"compiler process should exit {Cli.SourceErrors} on a bad program but exited {bad.Exit}");
        Assert(bad.Stdout.Length == 0, $"diagnostics must go to stderr, but stdout was: {bad.Stdout}");
        Assert(bad.Stderr.Contains("error FC3002", StringComparison.Ordinal), $"missing FC3002 diagnostic on stderr: {bad.Stderr}");
        return $"compiler stdout: {translate.Stdout.Trim()}; program stdout: 55; exit 0; bad program -> exit {bad.Exit}, stderr: {FirstLine(bad.Stderr)}";
    }

    private static string UsageErrors()
    {
        string directory = Path.Combine(_work, "cli-usage");
        Directory.CreateDirectory(directory);
        var cases = new (string[] Args, int Expected)[]
        {
            ([], Cli.UsageError),
            (["examples/sum_squares.fc"], Cli.UsageError),
            (["examples/sum_squares.fc", "--out"], Cli.UsageError),
            (["--bogus"], Cli.UsageError),
            (["a.fc", "b.fc", "--out", "c.fs"], Cli.UsageError),
            (["tests/does-not-exist.fc", "--out", Path.Combine(directory, "x.fs")], Cli.IoError),
            (["--help"], Cli.Success),
        };
        var lines = new List<string>();
        foreach (var (arguments, expected) in cases)
        {
            var stdout = new StringWriter();
            var stderr = new StringWriter();
            int exit = Cli.Run(arguments, stdout, stderr);
            Assert(exit == expected, $"'{string.Join(' ', arguments)}' exited {exit}, expected {expected}\n{stderr}");
            lines.Add($"[{string.Join(' ', arguments)}] -> {exit}");
        }

        Assert(!File.Exists(Path.Combine(directory, "x.fs")), "an output file was created for a missing input");
        return string.Join("; ", lines);
    }

    private static string DeterministicOutput()
    {
        string source = File.ReadAllText(Path.Combine(_root, "tests", "cases", "run", "evaluation_order.fc"));
        var first = Translator.Translate(source, "evaluation_order.fc");
        var second = Translator.Translate(source, "evaluation_order.fc");
        Assert(first.Success && second.Success, "translation failed");
        Assert(first.FSharpCode == second.FSharpCode, "two translations of the same source differ");
        return $"{first.FSharpCode!.Length} characters, identical across runs";
    }

    private static string SumSquaresShape()
    {
        var result = Translator.Translate(File.ReadAllText(Path.Combine(_root, "examples", "sum_squares.fc")), "sum_squares.fc");
        Assert(result.Success, "translation failed");
        string code = result.FSharpCode!;
        string[] required =
        [
            "let rec sumSquares (n: int) : int =",
            "let square = (fun (x: int) -> (x * x))",
            "let main () : int =",
            "[<EntryPoint>]",
            "let fc'entry (_args: string array) : int =",
            "    main ()",
        ];
        foreach (var fragment in required)
        {
            Assert(code.Contains(fragment, StringComparison.Ordinal), $"missing '{fragment}' in:\n{code}");
        }

        Assert(!code.Contains("let rec main", StringComparison.Ordinal), "main is not recursive and should not be 'let rec'");
        Assert(code.Contains("(if (n <= 0) then 0 else", StringComparison.Ordinal), "?: should become an if/then/else expression");
        return "found let rec, curried lambda, if/then/else, separate [<EntryPoint>] wrapper calling main ()";
    }

    private static string LexerOperators()
    {
        var tokens = Lexer.Tokenize("a<=b>=c==d!=e&&f||!g=>h=i<j>k", "t.fc");
        var kinds = tokens.Where(t => t.Kind != TokenKind.Identifier).Select(t => t.Kind).ToList();
        TokenKind[] expected =
        [
            TokenKind.LessEqual, TokenKind.GreaterEqual, TokenKind.EqualEqual, TokenKind.BangEqual, TokenKind.AmpAmp,
            TokenKind.PipePipe, TokenKind.Bang, TokenKind.Arrow, TokenKind.Assign, TokenKind.Less, TokenKind.Greater,
            TokenKind.EndOfFile,
        ];
        Assert(kinds.SequenceEqual(expected), $"got {string.Join(", ", kinds)}");

        var located = Lexer.Tokenize("let x\n  = 42;", "t.fc");
        Assert(located[2].Kind == TokenKind.Assign && located[2].Location.Line == 2 && located[2].Location.Column == 3,
            $"'=' should be at 2:3 but was at {located[2].Location.Short}");
        Assert(located[3].IntValue == 42, "literal value");
        return "<= >= == != && || ! => = < > tokenized correctly; line/column tracking verified";
    }

    private static async Task<string> ReferenceFixture()
    {
        string directory = Path.Combine(_work, "reference-fixture");
        string project = CreateFSharpProject(directory);
        File.Copy(Path.Combine(_root, "examples", "sum_squares.reference.fs"), Path.Combine(directory, "Program.fs"));
        var run = await BuildAndRun(directory, project);
        Assert(Normalize(run.Stdout) == "55\n" && run.Exit == 0, $"expected 55 / exit 0 but got exit {run.Exit}: {run.Stdout}");
        return "hand-written reference compiles and prints 55 (sanity check of the fixture only)";
    }

    // ------------------------------------------------------------ report

    private static string BuildReport(TestResult[] results, int passed, int failed, int skipped, TimeSpan elapsed)
    {
        var sb = new StringBuilder();
        sb.AppendLine($"Passed: **{passed}**, failed: **{failed}**, skipped: **{skipped}**, total: {results.Length} ({elapsed.TotalSeconds:0.0}s, .NET runtime {Environment.Version}, target {_targetFramework}).");
        sb.AppendLine();
        foreach (var group in results.GroupBy(r => r.Status).OrderBy(g => g.Key == "FAIL" ? 0 : g.Key == "SKIP" ? 1 : 2))
        {
            string title = group.Key switch { "FAIL" => "Failed", "SKIP" => "Skipped", _ => "Passed" };
            sb.AppendLine($"#### {title} ({group.Count()})");
            sb.AppendLine();
            foreach (var r in group)
            {
                sb.AppendLine($"- **{r.Test.Name}** [{r.Test.Category}, {r.Duration.TotalSeconds:0.0}s]");
                sb.AppendLine();
                sb.AppendLine("  ```text");
                foreach (var line in r.Detail.TrimEnd().Split('\n'))
                {
                    sb.AppendLine("  " + line.TrimEnd('\r'));
                }

                sb.AppendLine("  ```");
                sb.AppendLine();
            }
        }

        return sb.ToString();
    }
}
