namespace FunctionalC.Compiler;

/// <summary>
/// Recursive-descent parser with one function per C precedence level (lowest first):
///   conditional ?: (right-assoc) → || → &amp;&amp; → == != → &lt; &lt;= &gt; &gt;= → + - → * / % → unary - ! → call → primary.
/// Stops at the first syntax error.
/// </summary>
public sealed class Parser
{
    private static readonly (TokenKind Kind, BinaryOp Op)[][] BinaryLevels =
    [
        [(TokenKind.PipePipe, BinaryOp.Or)],
        [(TokenKind.AmpAmp, BinaryOp.And)],
        [(TokenKind.EqualEqual, BinaryOp.Equal), (TokenKind.BangEqual, BinaryOp.NotEqual)],
        [
            (TokenKind.Less, BinaryOp.Less), (TokenKind.LessEqual, BinaryOp.LessEqual),
            (TokenKind.Greater, BinaryOp.Greater), (TokenKind.GreaterEqual, BinaryOp.GreaterEqual),
        ],
        [(TokenKind.Plus, BinaryOp.Add), (TokenKind.Minus, BinaryOp.Subtract)],
        [(TokenKind.Star, BinaryOp.Multiply), (TokenKind.Slash, BinaryOp.Divide), (TokenKind.Percent, BinaryOp.Remainder)],
    ];

    private readonly List<Token> _tokens;
    private readonly string _file;
    private int _index;

    public Parser(List<Token> tokens, string file)
    {
        _tokens = tokens;
        _file = file;
    }

    private Token Current => _tokens[_index];

    private Token Previous => _tokens[Math.Max(0, _index - 1)];

    private Token PeekToken(int offset) => _tokens[Math.Min(_index + offset, _tokens.Count - 1)];

    private bool Check(TokenKind kind) => Current.Kind == kind;

    private Token Advance()
    {
        var token = Current;
        if (token.Kind != TokenKind.EndOfFile)
        {
            _index++;
        }

        return token;
    }

    private bool Match(TokenKind kind)
    {
        if (!Check(kind))
        {
            return false;
        }

        Advance();
        return true;
    }

    private static CompileError Error(string code, SourceLocation at, string message) =>
        new(new Diagnostic(code, message, at));

    private Token Expect(TokenKind kind, string what)
    {
        if (Check(kind))
        {
            return Advance();
        }

        if (kind == TokenKind.Identifier && Current.Kind is TokenKind.ReservedWord)
        {
            throw Error(Codes.UnsupportedFeature, Current.Location,
                $"'{Current.Text}' is a reserved C keyword and cannot be used as a name");
        }

        if (kind == TokenKind.Identifier && Current.Kind is >= TokenKind.KwInt and <= TokenKind.KwContinue)
        {
            throw Error(Codes.Syntax, Current.Location,
                $"expected {what} but found the keyword '{Current.Text}', which cannot be used as a name");
        }

        throw Error(Codes.Syntax, Current.Location, $"expected {what} but found {Current.Describe()}");
    }

    private void ExpectSemicolon()
    {
        if (Match(TokenKind.Semicolon))
        {
            return;
        }

        throw Error(Codes.Syntax, Previous.End,
            $"expected ';' at the end of the statement but found {Current.Describe()}");
    }

    private static CompileError Unsupported(Token token)
    {
        string w = token.Text;
        string message = w switch
        {
            "switch" or "case" or "default" =>
                $"'{w}' is not supported yet; use 'if (...) {{ ... }} else if (...) {{ ... }} else {{ ... }}'",
            "goto" =>
                "'goto' is not supported; use loops with 'break' and 'continue'",
            "struct" or "union" or "enum" or "typedef" =>
                $"'{w}' declarations are not supported in version 0",
            "void" =>
                "'void' is not supported in version 0: functions take typed parameters and return 'int' or 'bool'",
            "char" or "short" or "long" or "float" or "double" or "unsigned" or "signed" or "_Bool" =>
                $"type '{w}' is not supported in version 0: only 'int' and 'bool' are available",
            "sizeof" =>
                "'sizeof' is not supported in version 0",
            _ =>
                $"'{w}' is not supported in version 0",
        };
        return Error(Codes.UnsupportedFeature, token.Location, message);
    }

    private static CompileError PointerError(Token star) =>
        Error(Codes.UnsupportedFeature, star.Location, "pointers are not supported in version 0");

    // ------------------------------------------------------------ declarations

    public ProgramNode ParseProgram()
    {
        var functions = new List<FunctionDecl>();
        while (!Check(TokenKind.EndOfFile))
        {
            functions.Add(ParseFunction());
        }

        return new ProgramNode(_file, functions);
    }

    private FcType ParseType(string context)
    {
        var token = Current;
        switch (token.Kind)
        {
            case TokenKind.KwInt:
                Advance();
                return FcType.Int;
            case TokenKind.KwBool:
                Advance();
                return FcType.Bool;
            case TokenKind.ReservedWord:
                throw Unsupported(token);
            default:
                throw Error(Codes.Syntax, token.Location,
                    $"expected a type ('int' or 'bool') for {context} but found {token.Describe()}");
        }
    }

    private FunctionDecl ParseFunction()
    {
        var start = Current;
        if (start.Kind == TokenKind.KwLet)
        {
            throw Error(Codes.UnsupportedFeature, start.Location,
                "top-level 'let' bindings are not supported in version 0; only function definitions may appear at top level");
        }

        var returnType = ParseType("a function's return type");
        if (Check(TokenKind.Star))
        {
            throw PointerError(Current);
        }

        var nameToken = Expect(TokenKind.Identifier, "a function name");
        if (Check(TokenKind.Assign) || Check(TokenKind.Semicolon))
        {
            throw Error(Codes.UnsupportedFeature, nameToken.Location,
                "global variables are not supported in version 0; only function definitions may appear at top level");
        }

        Expect(TokenKind.LParen, $"'(' after the function name '{nameToken.Text}'");
        var parameters = new List<Parameter>();
        if (!Check(TokenKind.RParen))
        {
            do
            {
                parameters.Add(ParseParameter("a parameter"));
            }
            while (Match(TokenKind.Comma));
        }

        Expect(TokenKind.RParen, "')' to close the parameter list");
        if (Check(TokenKind.Semicolon))
        {
            throw Error(Codes.UnsupportedFeature, Current.Location,
                $"function prototypes are not supported in version 0; define '{nameToken.Text}' (with a body) before it is used");
        }

        var open = Expect(TokenKind.LBrace, $"'{{' to begin the body of '{nameToken.Text}'");
        var body = new List<Stmt>();
        while (!Check(TokenKind.RBrace))
        {
            if (Check(TokenKind.EndOfFile))
            {
                throw Error(Codes.Syntax, Current.Location,
                    $"expected '}}' to close the body of '{nameToken.Text}' (opened at {open.Location.Short}) but reached the end of the file");
            }

            body.Add(ParseStatement(nameToken.Text, open));
        }

        var close = Advance();
        return new FunctionDecl(returnType, start.Location, nameToken.Text, nameToken.Location, parameters, body, close.Location);
    }

    private Parameter ParseParameter(string context)
    {
        var type = ParseType(context);
        if (Check(TokenKind.Star))
        {
            throw PointerError(Current);
        }

        var name = Expect(TokenKind.Identifier, "a parameter name");
        return new Parameter(type, name.Text, name.Location);
    }

    // ------------------------------------------------------------ statements

    private Stmt ParseStatement(string functionName, Token functionOpen)
    {
        var token = Current;
        switch (token.Kind)
        {
            case TokenKind.LBrace:
                return ParseBlock(functionName, functionOpen);

            case TokenKind.Semicolon:
                Advance();
                return new EmptyStmt(token.Location);

            case TokenKind.KwIf:
            {
                Advance();
                var condition = ParseParenthesizedCondition("if");
                var then = ParseStatement(functionName, functionOpen);
                Stmt? @else = null;
                if (Match(TokenKind.KwElse)) // binds to the nearest 'if' (the C "dangling else" rule)
                {
                    @else = ParseStatement(functionName, functionOpen);
                }

                return new IfStmt(condition, then, @else, token.Location);
            }

            case TokenKind.KwElse:
                throw Error(Codes.Syntax, token.Location, "'else' without a matching 'if'");

            case TokenKind.KwWhile:
            {
                Advance();
                var condition = ParseParenthesizedCondition("while");
                var body = ParseStatement(functionName, functionOpen);
                return new WhileStmt(condition, body, token.Location);
            }

            case TokenKind.KwDo:
            {
                Advance();
                var body = ParseStatement(functionName, functionOpen);
                Expect(TokenKind.KwWhile, "'while' after the body of 'do'");
                var condition = ParseParenthesizedCondition("while");
                ExpectSemicolon();
                return new DoWhileStmt(body, condition, token.Location);
            }

            case TokenKind.KwFor:
                return ParseFor(functionName, functionOpen);

            case TokenKind.KwBreak:
                Advance();
                ExpectSemicolon();
                return new BreakStmt(token.Location);

            case TokenKind.KwContinue:
                Advance();
                ExpectSemicolon();
                return new ContinueStmt(token.Location);

            case TokenKind.KwReturn:
            {
                Advance();
                if (Check(TokenKind.Semicolon))
                {
                    throw Error(Codes.Syntax, token.Location, "'return' requires a value: every function returns 'int' or 'bool'");
                }

                var value = ParseExpression();
                ExpectSemicolon();
                return new ReturnStmt(value, token.Location);
            }

            case TokenKind.KwLet:
            {
                var let = ParseLet();
                ExpectSemicolon();
                return let;
            }

            case TokenKind.KwInt or TokenKind.KwBool:
            {
                if (PeekToken(1).Kind == TokenKind.Identifier && PeekToken(2).Kind == TokenKind.LParen)
                {
                    throw Error(Codes.Syntax, token.Location,
                        $"missing '}}' to close the body of '{functionName}' (opened at {functionOpen.Location.Short}) before this function definition; nested functions are not supported");
                }

                var declaration = ParseDeclaration();
                ExpectSemicolon();
                return declaration;
            }

            case TokenKind.ReservedWord:
                throw Unsupported(token);

            default:
            {
                var statement = ParseSimpleStatement();
                ExpectSemicolon();
                return statement;
            }
        }
    }

    private BlockStmt ParseBlock(string functionName, Token functionOpen)
    {
        var open = Advance(); // '{'
        var statements = new List<Stmt>();
        while (!Check(TokenKind.RBrace))
        {
            if (Check(TokenKind.EndOfFile))
            {
                throw Error(Codes.Syntax, Current.Location,
                    $"expected '}}' to close the block opened at {open.Location.Short} but reached the end of the file");
            }

            statements.Add(ParseStatement(functionName, functionOpen));
        }

        Advance();
        return new BlockStmt(statements, open.Location);
    }

    private Expr ParseParenthesizedCondition(string keyword)
    {
        Expect(TokenKind.LParen, $"'(' after '{keyword}'");
        var condition = ParseExpression();
        Expect(TokenKind.RParen, $"')' to close the '{keyword}' condition");
        return condition;
    }

    private ForStmt ParseFor(string functionName, Token functionOpen)
    {
        var forToken = Advance();
        Expect(TokenKind.LParen, "'(' after 'for'");

        var init = new List<Stmt>();
        if (Check(TokenKind.KwInt) || Check(TokenKind.KwBool))
        {
            init.Add(ParseDeclaration());
        }
        else if (Check(TokenKind.KwLet))
        {
            init.Add(ParseLet());
        }
        else if (!Check(TokenKind.Semicolon))
        {
            do
            {
                init.Add(ParseSimpleStatement());
            }
            while (Match(TokenKind.Comma));
        }

        Expect(TokenKind.Semicolon, "';' after the 'for' initializer");
        Expr? condition = Check(TokenKind.Semicolon) ? null : ParseExpression();
        Expect(TokenKind.Semicolon, "';' after the 'for' condition");

        var update = new List<Stmt>();
        if (!Check(TokenKind.RParen))
        {
            do
            {
                update.Add(ParseSimpleStatement());
            }
            while (Match(TokenKind.Comma));
        }

        Expect(TokenKind.RParen, "')' to close the 'for' header");
        var body = ParseStatement(functionName, functionOpen);
        return new ForStmt(init, condition, update, body, forToken.Location);
    }

    private LetStmt ParseLet()
    {
        var letToken = Advance();
        var name = Expect(TokenKind.Identifier, "a name after 'let'");
        if (Check(TokenKind.Colon))
        {
            throw Error(Codes.UnsupportedFeature, Current.Location,
                "type annotations on 'let' are not supported; the binding's type is inferred from its initializer");
        }

        Expect(TokenKind.Assign, $"'=' after 'let {name.Text}'");
        var initializer = ParseExpression();
        return new LetStmt(name.Text, name.Location, initializer, letToken.Location);
    }

    /// <summary>"int a = 1, b = 2" (without the ';'). Every variable must be initialized.</summary>
    private DeclarationStmt ParseDeclaration()
    {
        var typeToken = Current;
        var type = ParseType("a variable declaration");
        if (Check(TokenKind.Star))
        {
            throw PointerError(Current);
        }

        var declarators = new List<VarDeclarator>();
        do
        {
            var name = Expect(TokenKind.Identifier, "a variable name");
            if (!Check(TokenKind.Assign))
            {
                throw Error(Codes.UnsupportedFeature, name.Location,
                    $"variable '{name.Text}' must be initialized, as in '{typeToken.Text} {name.Text} = ...;' (uninitialized variables are not supported)");
            }

            Advance();
            var initializer = ParseExpression();
            declarators.Add(new VarDeclarator(name.Text, name.Location, initializer));
        }
        while (Match(TokenKind.Comma));

        return new DeclarationStmt(type, declarators, typeToken.Location);
    }

    /// <summary>
    /// An assignment ("x = e", "x += e", ...), an increment/decrement ("x++", "++x", "x--", "--x"),
    /// or an expression (only calls are accepted by the checker). No trailing ';'.
    /// </summary>
    private Stmt ParseSimpleStatement()
    {
        var token = Current;
        if (token.Kind is TokenKind.PlusPlus or TokenKind.MinusMinus)
        {
            Advance();
            var target = Expect(TokenKind.Identifier, $"a variable name after '{token.Text}'");
            return new AssignStmt(target.Text, token.Kind == TokenKind.PlusPlus ? AssignOp.Increment : AssignOp.Decrement, null, token.Location);
        }

        if (token.Kind == TokenKind.Identifier)
        {
            var next = PeekToken(1);
            AssignOp? op = next.Kind switch
            {
                TokenKind.Assign => AssignOp.Set,
                TokenKind.PlusAssign => AssignOp.Add,
                TokenKind.MinusAssign => AssignOp.Subtract,
                TokenKind.StarAssign => AssignOp.Multiply,
                TokenKind.SlashAssign => AssignOp.Divide,
                TokenKind.PercentAssign => AssignOp.Remainder,
                TokenKind.PlusPlus => AssignOp.Increment,
                TokenKind.MinusMinus => AssignOp.Decrement,
                _ => null,
            };
            if (op is { } assignOp)
            {
                Advance();
                Advance();
                Expr? value = assignOp is AssignOp.Increment or AssignOp.Decrement ? null : ParseExpression();
                return new AssignStmt(token.Text, assignOp, value, token.Location);
            }
        }

        var expression = ParseExpression();
        return new ExprStmt(expression, token.Location);
    }

    // ------------------------------------------------------------ expressions

    private Expr ParseExpression()
    {
        var expression = ParseConditional();
        if (Current.Kind is TokenKind.Assign or TokenKind.PlusAssign or TokenKind.MinusAssign or TokenKind.StarAssign
            or TokenKind.SlashAssign or TokenKind.PercentAssign)
        {
            throw Error(Codes.UnsupportedFeature, Current.Location,
                $"'{Current.Text}' cannot be used here: assignment is a statement of its own ('x = e;') and only a plain variable name can be assigned (did you mean '==' to compare?)");
        }

        return expression;
    }

    private static CompileError IncrementInExpression(Token token) =>
        Error(Codes.UnsupportedFeature, token.Location,
            $"'{token.Text}' can only be used as a statement of its own, as in 'x{token.Text};', not inside an expression");

    private Expr ParseConditional()
    {
        var condition = ParseBinary(0);
        if (!Check(TokenKind.Question))
        {
            return condition;
        }

        var question = Advance();
        var whenTrue = ParseConditional();
        Expect(TokenKind.Colon, "':' in the conditional expression");
        var whenFalse = ParseConditional(); // right-associative
        return new ConditionalExpr(condition, whenTrue, whenFalse, question.Location);
    }

    private Expr ParseBinary(int level)
    {
        if (level == BinaryLevels.Length)
        {
            return ParseUnary();
        }

        var left = ParseBinary(level + 1);
        while (true)
        {
            var match = Array.FindIndex(BinaryLevels[level], entry => entry.Kind == Current.Kind);
            if (match < 0)
            {
                return left;
            }

            var opToken = Advance();
            var right = ParseBinary(level + 1); // left-associative
            left = new BinaryExpr(BinaryLevels[level][match].Op, left, right, opToken.Location);
        }
    }

    private Expr ParseUnary()
    {
        var token = Current;
        switch (token.Kind)
        {
            case TokenKind.Minus:
                Advance();
                if (Check(TokenKind.IntLiteral))
                {
                    // A minus directly before a literal forms a negative literal, so -2147483648 is representable.
                    var literal = Advance();
                    var value = -literal.IntValue;
                    if (value < int.MinValue)
                    {
                        throw Error(Codes.IntegerOutOfRange, token.Location,
                            $"integer literal -{literal.Text} is out of range for a 32-bit int (-2147483648 to 2147483647)");
                    }

                    return ParsePostfixTail(new IntLiteralExpr(value, token.Location));
                }

                return new UnaryExpr(UnaryOp.Negate, ParseUnary(), token.Location);

            case TokenKind.Bang:
                Advance();
                return new UnaryExpr(UnaryOp.Not, ParseUnary(), token.Location);

            case TokenKind.PlusPlus or TokenKind.MinusMinus:
                throw IncrementInExpression(token);

            case TokenKind.Plus:
                throw Error(Codes.UnsupportedFeature, token.Location, "unary '+' is not supported in version 0");

            case TokenKind.Star:
                throw Error(Codes.UnsupportedFeature, token.Location, "pointer dereference '*' is not supported in version 0");

            default:
                return ParsePostfixTail(ParsePrimary());
        }
    }

    private Expr ParsePostfixTail(Expr expression)
    {
        while (Check(TokenKind.LParen))
        {
            var paren = Current;
            if (expression is NameExpr name)
            {
                Advance();
                var arguments = new List<Expr>();
                if (!Check(TokenKind.RParen))
                {
                    do
                    {
                        arguments.Add(ParseExpression());
                    }
                    while (Match(TokenKind.Comma));
                }

                Expect(TokenKind.RParen, $"')' to close the argument list of '{name.Name}'");
                expression = new CallExpr(name.Name, arguments, name.Location);
            }
            else if (expression is CallExpr)
            {
                throw Error(Codes.UnsupportedFeature, paren.Location, "calling the result of a call is not supported in version 0");
            }
            else
            {
                throw Error(Codes.UnsupportedFeature, paren.Location,
                    "only named functions and let-bound lambdas can be called, as in 'name(arguments)'");
            }
        }

        if (Current.Kind is TokenKind.PlusPlus or TokenKind.MinusMinus)
        {
            throw IncrementInExpression(Current);
        }

        return expression;
    }

    private Expr ParsePrimary()
    {
        var token = Current;
        switch (token.Kind)
        {
            case TokenKind.IntLiteral:
                Advance();
                if (token.IntValue > int.MaxValue)
                {
                    throw Error(Codes.IntegerOutOfRange, token.Location,
                        $"integer literal {token.Text} is out of range for a 32-bit int (maximum 2147483647); -2147483648 may only be written with a leading '-' directly before the digits");
                }

                return new IntLiteralExpr(token.IntValue, token.Location);

            case TokenKind.KwTrue:
                Advance();
                return new BoolLiteralExpr(true, token.Location);

            case TokenKind.KwFalse:
                Advance();
                return new BoolLiteralExpr(false, token.Location);

            case TokenKind.Identifier:
                Advance();
                return new NameExpr(token.Text, token.Location);

            case TokenKind.LParen:
                return ParseParenthesizedOrLambda();

            case TokenKind.ReservedWord:
                throw Unsupported(token);

            case TokenKind.EndOfFile:
                throw Error(Codes.Syntax, token.Location, "expected an expression but reached the end of the file");

            default:
                throw Error(Codes.Syntax, token.Location, $"expected an expression but found {token.Describe()}");
        }
    }

    /// <summary>
    /// Disambiguates '(' deliberately: a lambda starts with "(int", "(bool" or "() =>"; anything else is a
    /// parenthesized expression. "(x) => ..." is rejected with a hint that lambda parameters need types.
    /// </summary>
    private Expr ParseParenthesizedOrLambda()
    {
        var open = Current;
        var next = PeekToken(1);
        if (next.Kind is TokenKind.KwInt or TokenKind.KwBool)
        {
            return ParseLambda();
        }

        if (next.Kind == TokenKind.RParen)
        {
            if (PeekToken(2).Kind == TokenKind.Arrow)
            {
                return ParseLambda();
            }

            throw Error(Codes.Syntax, open.Location, "empty parentheses '()' are not an expression");
        }

        if (next.Kind == TokenKind.ReservedWord)
        {
            throw Unsupported(next);
        }

        Advance();
        var inner = ParseExpression();
        if (inner is NameExpr && Check(TokenKind.Comma))
        {
            throw Error(Codes.Syntax, open.Location, "lambda parameters must have explicit types, for example '(int x, int y) => x + y'");
        }

        Expect(TokenKind.RParen, "')' to close the parenthesized expression");
        if (Check(TokenKind.Arrow))
        {
            throw Error(Codes.Syntax, open.Location, "lambda parameters must have explicit types, for example '(int x) => x + 1'");
        }

        return inner;
    }

    private Expr ParseLambda()
    {
        var open = Advance(); // '('
        var parameters = new List<Parameter>();
        if (!Check(TokenKind.RParen))
        {
            do
            {
                var typeToken = Current;
                var type = ParseType("a lambda parameter");
                if (Check(TokenKind.RParen) && PeekToken(1).Kind != TokenKind.Arrow && parameters.Count == 0)
                {
                    throw Error(Codes.UnsupportedFeature, open.Location,
                        $"type casts such as '({typeToken.Text}) x' are not supported in version 0");
                }

                if (Check(TokenKind.Star))
                {
                    throw PointerError(Current);
                }

                var name = Expect(TokenKind.Identifier, "a lambda parameter name");
                parameters.Add(new Parameter(type, name.Text, name.Location));
            }
            while (Match(TokenKind.Comma));
        }

        Expect(TokenKind.RParen, "')' to close the lambda parameter list");
        Expect(TokenKind.Arrow, "'=>' after the lambda parameter list");
        if (Check(TokenKind.LBrace))
        {
            throw Error(Codes.UnsupportedFeature, Current.Location,
                "lambda block bodies '{ ... }' are not supported in version 0; use an expression body");
        }

        var body = ParseExpression();
        return new LambdaExpr(parameters, body, open.Location);
    }
}
