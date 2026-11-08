using System.Globalization;

namespace FunctionalC.Compiler;

public enum TokenKind
{
    Identifier,
    IntLiteral,
    /// <summary>A C keyword that version 0 does not support (if, while, void, struct, ...).</summary>
    ReservedWord,

    KwInt,
    KwBool,
    KwLet,
    KwReturn,
    KwTrue,
    KwFalse,

    LParen,
    RParen,
    LBrace,
    RBrace,
    Comma,
    Semicolon,
    Question,
    Colon,
    Arrow,       // =>
    Assign,      // =

    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    EqualEqual,
    BangEqual,
    AmpAmp,
    PipePipe,
    Bang,

    EndOfFile,
}

public sealed class Token(TokenKind kind, string text, SourceLocation location, long intValue = 0)
{
    public TokenKind Kind { get; } = kind;
    public string Text { get; } = text;
    public SourceLocation Location { get; } = location;

    /// <summary>Magnitude of an integer literal (always non-negative; at most 2^31).</summary>
    public long IntValue { get; } = intValue;

    /// <summary>The position just after this token (tokens never span lines).</summary>
    public SourceLocation End => Location with { Column = Location.Column + Text.Length };

    public string Describe() => Kind == TokenKind.EndOfFile ? "end of file" : $"'{Text}'";

    public override string ToString() => $"{Kind} '{Text}' at {Location.Short}";
}

/// <summary>
/// Converts Functional C source text into tokens. Columns count characters, so a tab is one column.
/// Unsupported C operators are rejected here with a specific message instead of being dropped.
/// </summary>
public sealed class Lexer
{
    /// <summary>Largest literal magnitude the lexer accepts. 2147483648 is only valid after a unary minus.</summary>
    public const long MaxLiteralMagnitude = 2147483648L;

    private static readonly Dictionary<string, TokenKind> Keywords = new(StringComparer.Ordinal)
    {
        ["int"] = TokenKind.KwInt,
        ["bool"] = TokenKind.KwBool,
        ["let"] = TokenKind.KwLet,
        ["return"] = TokenKind.KwReturn,
        ["true"] = TokenKind.KwTrue,
        ["false"] = TokenKind.KwFalse,
    };

    /// <summary>C keywords that are reserved but not supported in version 0.</summary>
    public static readonly IReadOnlySet<string> ReservedCWords = new HashSet<string>(StringComparer.Ordinal)
    {
        "if", "else", "while", "for", "do", "switch", "case", "default", "break", "continue", "goto",
        "struct", "union", "enum", "typedef", "void", "char", "short", "long", "float", "double",
        "unsigned", "signed", "_Bool", "const", "static", "extern", "auto", "register", "volatile",
        "inline", "restrict", "sizeof",
    };

    private readonly string _text;
    private readonly string _file;
    private int _pos;
    private int _line = 1;
    private int _column = 1;

    private Lexer(string text, string file)
    {
        _text = text;
        _file = file;
    }

    public static List<Token> Tokenize(string text, string file) => new Lexer(text, file).Run();

    private char Peek(int offset = 0) => _pos + offset < _text.Length ? _text[_pos + offset] : '\0';

    private SourceLocation Here => new(_file, _line, _column);

    private void Advance()
    {
        if (_text[_pos] == '\n')
        {
            _line++;
            _column = 1;
        }
        else
        {
            _column++;
        }

        _pos++;
    }

    private static CompileError Error(string code, SourceLocation at, string message) =>
        new(new Diagnostic(code, message, at));

    private List<Token> Run()
    {
        var tokens = new List<Token>();
        while (true)
        {
            SkipWhitespaceAndComments();
            if (_pos >= _text.Length)
            {
                tokens.Add(new Token(TokenKind.EndOfFile, "", Here));
                return tokens;
            }

            char c = Peek();
            if (char.IsAsciiDigit(c))
            {
                tokens.Add(LexNumber());
            }
            else if (char.IsAsciiLetter(c) || c == '_')
            {
                tokens.Add(LexWord());
            }
            else
            {
                tokens.Add(LexOperator());
            }
        }
    }

    private void SkipWhitespaceAndComments()
    {
        while (_pos < _text.Length)
        {
            char c = Peek();
            if (c is ' ' or '\t' or '\r' or '\n' or '\f' or '\v')
            {
                Advance();
            }
            else if (c == '/' && Peek(1) == '/')
            {
                while (_pos < _text.Length && Peek() != '\n')
                {
                    Advance();
                }
            }
            else if (c == '/' && Peek(1) == '*')
            {
                var start = Here;
                Advance();
                Advance();
                while (true)
                {
                    if (_pos >= _text.Length)
                    {
                        throw Error(Codes.UnterminatedComment, start, "unterminated block comment: '/*' has no matching '*/'");
                    }

                    if (Peek() == '*' && Peek(1) == '/')
                    {
                        Advance();
                        Advance();
                        break;
                    }

                    Advance();
                }
            }
            else
            {
                return;
            }
        }
    }

    private Token LexNumber()
    {
        var start = Here;
        int begin = _pos;
        while (char.IsAsciiDigit(Peek()))
        {
            Advance();
        }

        string digits = _text[begin.._pos];

        if (char.IsAsciiLetter(Peek()) || Peek() == '_')
        {
            if (digits == "0" && Peek() is 'x' or 'X')
            {
                throw Error(Codes.InvalidLiteral, start, "hexadecimal literals are not supported in version 0; use decimal");
            }

            int suffixStart = _pos;
            while (char.IsAsciiLetterOrDigit(Peek()) || Peek() == '_')
            {
                Advance();
            }

            throw Error(Codes.InvalidLiteral, start,
                $"invalid integer literal '{_text[begin.._pos]}': only plain decimal integers are supported (no suffixes such as '{_text[suffixStart.._pos]}')");
        }

        if (Peek() == '.')
        {
            throw Error(Codes.InvalidLiteral, start, "floating-point literals are not supported in version 0; only 'int' and 'bool' exist");
        }

        if (digits.Length > 1 && digits[0] == '0')
        {
            throw Error(Codes.InvalidLiteral, start,
                $"integer literal '{digits}' has a leading zero; C would read it as octal, so it is rejected (write it without leading zeros)");
        }

        if (digits.Length > 10 || long.Parse(digits, CultureInfo.InvariantCulture) > MaxLiteralMagnitude)
        {
            throw Error(Codes.IntegerOutOfRange, start,
                $"integer literal {digits} is out of range for a 32-bit int (-2147483648 to 2147483647)");
        }

        return new Token(TokenKind.IntLiteral, digits, start, long.Parse(digits, CultureInfo.InvariantCulture));
    }

    private Token LexWord()
    {
        var start = Here;
        int begin = _pos;
        while (char.IsAsciiLetterOrDigit(Peek()) || Peek() == '_')
        {
            Advance();
        }

        string word = _text[begin.._pos];
        if (Keywords.TryGetValue(word, out var kind))
        {
            return new Token(kind, word, start);
        }

        return new Token(ReservedCWords.Contains(word) ? TokenKind.ReservedWord : TokenKind.Identifier, word, start);
    }

    private Token LexOperator()
    {
        var start = Here;
        char c = Peek();
        char n = Peek(1);
        string three = $"{c}{n}{Peek(2)}";
        string two = $"{c}{n}";

        if (three is "<<=" or ">>=")
        {
            throw Error(Codes.UnsupportedToken, start, $"compound assignment '{three}' is not supported: bindings are immutable");
        }

        switch (two)
        {
            case "<=": return Take(TokenKind.LessEqual, two, start);
            case ">=": return Take(TokenKind.GreaterEqual, two, start);
            case "==": return Take(TokenKind.EqualEqual, two, start);
            case "!=": return Take(TokenKind.BangEqual, two, start);
            case "&&": return Take(TokenKind.AmpAmp, two, start);
            case "||": return Take(TokenKind.PipePipe, two, start);
            case "=>": return Take(TokenKind.Arrow, two, start);
            case "->":
                throw Error(Codes.UnsupportedToken, start, "'->' member access is not supported: version 0 has no pointers or structures");
            case "++" or "--":
                throw Error(Codes.UnsupportedToken, start, $"'{two}' is not supported: bindings are immutable (there is no increment or decrement)");
            case "+=" or "-=" or "*=" or "/=" or "%=" or "&=" or "|=" or "^=":
                throw Error(Codes.UnsupportedToken, start, $"compound assignment '{two}' is not supported: bindings are immutable");
            case "<<" or ">>":
                throw Error(Codes.UnsupportedToken, start, $"shift operator '{two}' is not supported in version 0");
        }

        switch (c)
        {
            case '(': return Take(TokenKind.LParen, "(", start);
            case ')': return Take(TokenKind.RParen, ")", start);
            case '{': return Take(TokenKind.LBrace, "{", start);
            case '}': return Take(TokenKind.RBrace, "}", start);
            case ',': return Take(TokenKind.Comma, ",", start);
            case ';': return Take(TokenKind.Semicolon, ";", start);
            case '?': return Take(TokenKind.Question, "?", start);
            case ':': return Take(TokenKind.Colon, ":", start);
            case '=': return Take(TokenKind.Assign, "=", start);
            case '+': return Take(TokenKind.Plus, "+", start);
            case '-': return Take(TokenKind.Minus, "-", start);
            case '*': return Take(TokenKind.Star, "*", start);
            case '/': return Take(TokenKind.Slash, "/", start);
            case '%': return Take(TokenKind.Percent, "%", start);
            case '<': return Take(TokenKind.Less, "<", start);
            case '>': return Take(TokenKind.Greater, ">", start);
            case '!': return Take(TokenKind.Bang, "!", start);
            case '#':
                throw Error(Codes.UnsupportedToken, start, "preprocessor directives ('#') are not supported: there are no headers or macros in version 0");
            case '[' or ']':
                throw Error(Codes.UnsupportedToken, start, "arrays and indexing ('[' ']') are not supported in version 0");
            case '&':
                throw Error(Codes.UnsupportedToken, start, "'&' (address-of / bitwise and) is not supported; use '&&' for logical and");
            case '|':
                throw Error(Codes.UnsupportedToken, start, "bitwise '|' is not supported; use '||' for logical or");
            case '^' or '~':
                throw Error(Codes.UnsupportedToken, start, $"bitwise operator '{c}' is not supported in version 0");
            case '.':
                throw Error(Codes.UnsupportedToken, start, "'.' is not supported: version 0 has no structures or floating-point numbers");
            case '"':
                throw Error(Codes.UnsupportedToken, start, "string literals are not supported in version 0");
            case '\'':
                throw Error(Codes.UnsupportedToken, start, "character literals are not supported in version 0");
        }

        string shown = char.IsControl(c) || c > '~' ? $"U+{(int)c:X4}" : $"'{c}'";
        throw Error(Codes.UnexpectedCharacter, start, $"unexpected character {shown}");
    }

    private Token Take(TokenKind kind, string text, SourceLocation start)
    {
        for (int i = 0; i < text.Length; i++)
        {
            Advance();
        }

        return new Token(kind, text, start);
    }
}
