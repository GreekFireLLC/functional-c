namespace FunctionalC.Compiler;

public sealed record TranslationResult(bool Success, string? FSharpCode, IReadOnlyList<Diagnostic> Diagnostics);

/// <summary>Lex → parse → check → emit, entirely in memory. Nothing is written unless every stage succeeds.</summary>
public static class Translator
{
    public static TranslationResult Translate(string source, string fileName)
    {
        try
        {
            var tokens = Lexer.Tokenize(source, fileName);
            var program = new Parser(tokens, fileName).ParseProgram();
            var errors = new Checker().Check(program);
            if (errors.Count > 0)
            {
                return new TranslationResult(false, null, errors);
            }

            var code = new FSharpEmitter().Emit(program, Path.GetFileName(fileName));
            return new TranslationResult(true, code, []);
        }
        catch (CompileError error)
        {
            return new TranslationResult(false, null, [error.Diagnostic]);
        }
    }
}
