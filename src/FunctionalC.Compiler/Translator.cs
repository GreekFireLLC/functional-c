namespace FunctionalC.Compiler;

/// <summary>The diagnostics hold the errors of a failed translation, and the warnings of any translation.</summary>
public sealed record TranslationResult(bool Success, string? FSharpCode, IReadOnlyList<Diagnostic> Diagnostics);

/// <summary>Lex → parse → check → emit, entirely in memory. Nothing is written unless every stage succeeds.</summary>
public static class Translator
{
    /// <param name="strict">Reject what C accepts but version 1.2 did not (see <see cref="Checker"/>).</param>
    public static TranslationResult Translate(string source, string fileName, bool strict = false)
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

            var code = new FSharpEmitter().Emit(program, Path.GetFileName(fileName));
            return new TranslationResult(true, code, diagnostics);
        }
        catch (CompileError error)
        {
            return new TranslationResult(false, null, [error.Diagnostic]);
        }
    }
}
