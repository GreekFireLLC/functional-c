namespace FunctionalC.Compiler;

/// <summary>Output language produced by the translator.</summary>
public enum EmitTarget
{
    FSharp,
    C,
}

/// <summary>The diagnostics hold the errors of a failed translation, and the warnings of any translation.</summary>
public sealed record TranslationResult(bool Success, string? Code, IReadOnlyList<Diagnostic> Diagnostics)
{
    /// <summary>Alias for <see cref="Code"/> kept for callers targeting F#.</summary>
    public string? FSharpCode => Code;
}

/// <summary>Lex → parse → check → emit, entirely in memory. Nothing is written unless every stage succeeds.</summary>
public static class Translator
{
    /// <param name="strict">Reject what C accepts but version 1.2 did not (see <see cref="Checker"/>).</param>
    /// <param name="target">Target language to emit: F# (default) or portable C17.</param>
    public static TranslationResult Translate(string source, string fileName, bool strict = false, EmitTarget target = EmitTarget.FSharp)
    {
        try
        {
            var tokens = Lexer.Tokenize(source, fileName);
            var program = new Parser(tokens, fileName).ParseProgram();
            var diagnostics = new Checker(strict).Check(program);
            if (diagnostics.Any(d => !d.IsWarning))
            {
                return new TranslationResult(false, null, diagnostics);
            }

            string sourceName = Path.GetFileName(fileName);
            string code = target switch
            {
                EmitTarget.FSharp => new FSharpEmitter().Emit(program, sourceName),
                EmitTarget.C => new CEmitter().Emit(program, sourceName),
                _ => throw new ArgumentOutOfRangeException(nameof(target)),
            };
            return new TranslationResult(true, code, diagnostics);
        }
        catch (CompileError error)
        {
            return new TranslationResult(false, null, [error.Diagnostic]);
        }
    }
}
