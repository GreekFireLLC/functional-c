namespace FunctionalC.Compiler;

/// <summary>A 1-based line/column position in a Functional C source file.</summary>
public readonly record struct SourceLocation(string File, int Line, int Column)
{
    public string Short => $"{Line}:{Column}";

    public override string ToString() => $"{File}({Line},{Column})";
}

/// <summary>A translator error, formatted in the conventional "file(line,col): error CODE: message" style.</summary>
public sealed record Diagnostic(string Code, string Message, SourceLocation Location)
{
    public override string ToString() =>
        $"{Location.File}({Location.Line},{Location.Column}): error {Code}: {Message}";
}

/// <summary>Thrown by the lexer and parser, which stop at the first syntax error.</summary>
public sealed class CompileError(Diagnostic diagnostic) : Exception(diagnostic.ToString())
{
    public Diagnostic Diagnostic { get; } = diagnostic;
}

/// <summary>Diagnostic codes. FC1xxx = lexical, FC2xxx = syntax, FC3xxx = semantic.</summary>
public static class Codes
{
    public const string UnexpectedCharacter = "FC1001";
    public const string UnterminatedComment = "FC1002";
    public const string IntegerOutOfRange = "FC1003";
    public const string InvalidLiteral = "FC1004";
    public const string UnsupportedToken = "FC1005";

    public const string Syntax = "FC2001";
    public const string UnsupportedFeature = "FC2002";

    public const string UnknownName = "FC3001";
    public const string ImmutableBinding = "FC3002";
    public const string DuplicateName = "FC3003";
    public const string ForwardReference = "FC3004";
    public const string ReturnPlacement = "FC3005";
    public const string EntryPoint = "FC3006";
    public const string TypeMismatch = "FC3007";
    public const string Arity = "FC3008";
    public const string NotAValue = "FC3009";
    public const string NotCallable = "FC3010";
    public const string DiscardedExpression = "FC3011";
    public const string ReservedName = "FC3012";
    public const string PrintMisuse = "FC3013";
    public const string LambdaPosition = "FC3014";
    public const string SelfReference = "FC3015";
}
