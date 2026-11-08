using System.Text;

namespace FunctionalC.Compiler;

/// <summary>
/// Command line: <c>FunctionalC.Compiler &lt;input.fc&gt; --out &lt;output.fs&gt;</c>.
/// Exit codes: 0 = translated, 1 = errors in the source, 2 = bad command line, 3 = file I/O failure.
/// Diagnostics go to standard error. The output file is replaced atomically and only after translation succeeds.
/// </summary>
public static class Cli
{
    public const int Success = 0;
    public const int SourceErrors = 1;
    public const int UsageError = 2;
    public const int IoError = 3;

    private const string Usage =
        """
        Functional C -> F# translator (version 0)

        Usage:
          FunctionalC.Compiler <input.fc> --out <output.fs>

        Translates a Functional C source file into an F# source file. Build the F# with the .NET SDK, e.g.
          dotnet run --project src/FunctionalC.Compiler -- examples/sum_squares.fc --out generated/SampleApp/Program.fs
          dotnet build generated/SampleApp/SampleApp.fsproj
          dotnet run --no-build --project generated/SampleApp

        Exit codes: 0 translated, 1 source errors, 2 bad command line, 3 file I/O failure.
        """;

    public static int Run(string[] args, TextWriter stdout, TextWriter stderr)
    {
        if (args.Length == 1 && args[0] is "-h" or "--help" or "/?")
        {
            stdout.WriteLine(Usage);
            return Success;
        }

        string? input = null;
        string? output = null;
        for (int i = 0; i < args.Length; i++)
        {
            if (args[i] == "--out")
            {
                if (i + 1 >= args.Length)
                {
                    return UsageFailure(stderr, "'--out' must be followed by an output file path");
                }

                if (output is not null)
                {
                    return UsageFailure(stderr, "'--out' was given more than once");
                }

                output = args[++i];
            }
            else if (args[i].StartsWith('-'))
            {
                return UsageFailure(stderr, $"unknown option '{args[i]}'");
            }
            else if (input is null)
            {
                input = args[i];
            }
            else
            {
                return UsageFailure(stderr, $"unexpected extra argument '{args[i]}'; only one input file is supported");
            }
        }

        if (input is null)
        {
            return UsageFailure(stderr, "no input file was given");
        }

        if (output is null)
        {
            return UsageFailure(stderr, "no output file was given; use '--out <output.fs>'");
        }

        string source;
        try
        {
            source = File.ReadAllText(input);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException)
        {
            stderr.WriteLine($"error FC0001: cannot read input file '{input}': {e.Message}");
            return IoError;
        }

        var result = Translator.Translate(source, input);
        if (!result.Success)
        {
            foreach (var diagnostic in result.Diagnostics)
            {
                stderr.WriteLine(diagnostic);
            }

            stderr.WriteLine($"translation failed with {result.Diagnostics.Count} error(s); '{output}' was not written (an existing file is left unchanged)");
            return SourceErrors;
        }

        string? temp = null;
        try
        {
            string fullOutput = Path.GetFullPath(output);
            string directory = Path.GetDirectoryName(fullOutput)!;
            Directory.CreateDirectory(directory);

            // Write beside the target, then rename over it, so a crash never leaves a half-written file.
            temp = Path.Combine(directory, $".{Path.GetFileName(fullOutput)}.{Guid.NewGuid():N}.tmp");
            File.WriteAllText(temp, result.FSharpCode, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
            File.Move(temp, fullOutput, overwrite: true);
            temp = null;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException)
        {
            stderr.WriteLine($"error FC0002: cannot write output file '{output}': {e.Message}");
            return IoError;
        }
        finally
        {
            if (temp is not null)
            {
                try
                {
                    File.Delete(temp);
                }
                catch (IOException)
                {
                    // best effort cleanup
                }
            }
        }

        stdout.WriteLine($"Translated {input} -> {output}");
        return Success;
    }

    private static int UsageFailure(TextWriter stderr, string message)
    {
        stderr.WriteLine($"error: {message}");
        stderr.WriteLine("usage: FunctionalC.Compiler <input.fc> --out <output.fs>   (use --help for details)");
        return UsageError;
    }
}
