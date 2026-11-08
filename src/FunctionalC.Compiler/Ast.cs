namespace FunctionalC.Compiler;

/// <summary>The value types of version 0.</summary>
public enum FcType
{
    Int,
    Bool,
}

public static class FcTypeNames
{
    public static string Name(this FcType type) => type == FcType.Int ? "int" : "bool";

    public static string Name(this FcType? type) => type is { } t ? t.Name() : "<error>";
}

public enum UnaryOp
{
    Negate,
    Not,
}

public enum BinaryOp
{
    Add,
    Subtract,
    Multiply,
    Divide,
    Remainder,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Equal,
    NotEqual,
    And,
    Or,
}

public static class OperatorText
{
    public static string Source(this BinaryOp op) => op switch
    {
        BinaryOp.Add => "+",
        BinaryOp.Subtract => "-",
        BinaryOp.Multiply => "*",
        BinaryOp.Divide => "/",
        BinaryOp.Remainder => "%",
        BinaryOp.Less => "<",
        BinaryOp.LessEqual => "<=",
        BinaryOp.Greater => ">",
        BinaryOp.GreaterEqual => ">=",
        BinaryOp.Equal => "==",
        BinaryOp.NotEqual => "!=",
        BinaryOp.And => "&&",
        BinaryOp.Or => "||",
        _ => throw new ArgumentOutOfRangeException(nameof(op)),
    };

    public static string Source(this UnaryOp op) => op == UnaryOp.Negate ? "-" : "!";
}

// ---------------------------------------------------------------- expressions
// Every expression's Location is the position where it starts in the source.

public abstract class Expr(SourceLocation location)
{
    public SourceLocation Location { get; } = location;
}

public sealed class IntLiteralExpr(long value, SourceLocation location) : Expr(location)
{
    /// <summary>Signed value in the 32-bit range (a leading minus on a literal is folded in by the parser).</summary>
    public long Value { get; } = value;
}

public sealed class BoolLiteralExpr(bool value, SourceLocation location) : Expr(location)
{
    public bool Value { get; } = value;
}

public sealed class NameExpr(string name, SourceLocation location) : Expr(location)
{
    public string Name { get; } = name;

    /// <summary>Set by the checker.</summary>
    public Symbol? Symbol { get; set; }
}

public sealed class UnaryExpr(UnaryOp op, Expr operand, SourceLocation location) : Expr(location)
{
    public UnaryOp Op { get; } = op;
    public Expr Operand { get; } = operand;
}

public sealed class BinaryExpr(BinaryOp op, Expr left, Expr right, SourceLocation operatorLocation)
    : Expr(left.Location)
{
    public BinaryOp Op { get; } = op;
    public Expr Left { get; } = left;
    public Expr Right { get; } = right;
    public SourceLocation OperatorLocation { get; } = operatorLocation;
}

public sealed class ConditionalExpr(Expr condition, Expr whenTrue, Expr whenFalse, SourceLocation questionLocation)
    : Expr(condition.Location)
{
    public Expr Condition { get; } = condition;
    public Expr WhenTrue { get; } = whenTrue;
    public Expr WhenFalse { get; } = whenFalse;
    public SourceLocation QuestionLocation { get; } = questionLocation;
}

public sealed class CallExpr(string callee, IReadOnlyList<Expr> arguments, SourceLocation location) : Expr(location)
{
    public string Callee { get; } = callee;
    public IReadOnlyList<Expr> Arguments { get; } = arguments;

    /// <summary>Set by the checker; null for the built-in print.</summary>
    public Symbol? Symbol { get; set; }

    /// <summary>True when this is the built-in print used as a statement.</summary>
    public bool IsPrint { get; set; }
}

public sealed class LambdaExpr(IReadOnlyList<Parameter> parameters, Expr body, SourceLocation location) : Expr(location)
{
    public IReadOnlyList<Parameter> Parameters { get; } = parameters;
    public Expr Body { get; } = body;
}

public sealed class Parameter(FcType type, string name, SourceLocation location)
{
    public FcType Type { get; } = type;
    public string Name { get; } = name;
    public SourceLocation Location { get; } = location;

    /// <summary>Set by the checker.</summary>
    public Symbol? Symbol { get; set; }
}

// ---------------------------------------------------------------- statements

public abstract class Stmt(SourceLocation location)
{
    public SourceLocation Location { get; } = location;
}

public sealed class LetStmt(string name, SourceLocation nameLocation, Expr initializer, SourceLocation location)
    : Stmt(location)
{
    public string Name { get; } = name;
    public SourceLocation NameLocation { get; } = nameLocation;
    public Expr Initializer { get; } = initializer;

    /// <summary>Set by the checker.</summary>
    public Symbol? Symbol { get; set; }
}

/// <summary>An expression statement; the checker only accepts calls.</summary>
public sealed class ExprStmt(Expr expression, SourceLocation location) : Stmt(location)
{
    public Expr Expression { get; } = expression;
}

public sealed class ReturnStmt(Expr value, SourceLocation location) : Stmt(location)
{
    public Expr Value { get; } = value;
}

/// <summary>"name = value;" is parsed so that the checker can reject it with a precise message.</summary>
public sealed class AssignStmt(string name, Expr value, SourceLocation location) : Stmt(location)
{
    public string Name { get; } = name;
    public Expr Value { get; } = value;
}

// ---------------------------------------------------------------- declarations

public sealed class FunctionDecl(
    FcType returnType,
    SourceLocation returnTypeLocation,
    string name,
    SourceLocation nameLocation,
    IReadOnlyList<Parameter> parameters,
    IReadOnlyList<Stmt> body,
    SourceLocation closeBraceLocation)
{
    public FcType ReturnType { get; } = returnType;
    public SourceLocation ReturnTypeLocation { get; } = returnTypeLocation;
    public string Name { get; } = name;
    public SourceLocation NameLocation { get; } = nameLocation;
    public IReadOnlyList<Parameter> Parameters { get; } = parameters;
    public IReadOnlyList<Stmt> Body { get; } = body;
    public SourceLocation CloseBraceLocation { get; } = closeBraceLocation;

    /// <summary>Set by the checker.</summary>
    public FunctionSymbol? Symbol { get; set; }
}

public sealed class ProgramNode(string file, IReadOnlyList<FunctionDecl> functions)
{
    public string File { get; } = file;
    public IReadOnlyList<FunctionDecl> Functions { get; } = functions;
}

// ---------------------------------------------------------------- symbols

public abstract class Symbol(string name, SourceLocation declaredAt)
{
    public string Name { get; } = name;
    public SourceLocation DeclaredAt { get; } = declaredAt;

    /// <summary>The identifier used for this symbol in the emitted F#.</summary>
    public string FSharpName { get; } = FSharpNames.ForSource(name);
}

/// <summary>Something that can be called: a top-level function or a let-bound lambda.</summary>
public interface ICallable
{
    IReadOnlyList<FcType> ParameterTypes { get; }

    /// <summary>Null when the lambda body had a type error.</summary>
    FcType? ReturnType { get; }
}

public sealed class FunctionSymbol(FunctionDecl declaration)
    : Symbol(declaration.Name, declaration.NameLocation), ICallable
{
    public FunctionDecl Declaration { get; } = declaration;
    public IReadOnlyList<FcType> ParameterTypes { get; } = declaration.Parameters.Select(p => p.Type).ToList();
    public FcType? ReturnType => Declaration.ReturnType;

    /// <summary>True when the body (including lambdas inside it) calls this function; emitted as "let rec".</summary>
    public bool IsRecursive { get; set; }
}

public sealed class LambdaSymbol(string name, SourceLocation declaredAt, IReadOnlyList<FcType> parameterTypes, FcType? returnType)
    : Symbol(name, declaredAt), ICallable
{
    public IReadOnlyList<FcType> ParameterTypes { get; } = parameterTypes;
    public FcType? ReturnType { get; } = returnType;
}

public sealed class ValueSymbol(string name, SourceLocation declaredAt, FcType? type, string kind)
    : Symbol(name, declaredAt)
{
    /// <summary>Null when the initializer had a type error (suppresses cascading errors).</summary>
    public FcType? Type { get; } = type;

    /// <summary>"parameter", "local binding" or "lambda parameter" (used in messages).</summary>
    public string Kind { get; } = kind;
}
