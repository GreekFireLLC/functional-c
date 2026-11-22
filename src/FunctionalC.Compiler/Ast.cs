namespace FunctionalC.Compiler;

/// <summary>The value types: int and bool (version 0), and C's two 8-bit integer types (version 1.3).</summary>
public enum FcType
{
    /// <summary>Signed 32-bit.</summary>
    Int,
    Bool,

    /// <summary>'char' and 'signed char': signed 8-bit, as on the common x86 and x64 C compilers.</summary>
    Char,

    /// <summary>'unsigned char': unsigned 8-bit.</summary>
    UChar,
}

public static class FcTypeNames
{
    public static string Name(this FcType type) => type switch
    {
        FcType.Int => "int",
        FcType.Bool => "bool",
        FcType.Char => "char",
        FcType.UChar => "unsigned char",
        _ => throw new ArgumentOutOfRangeException(nameof(type)),
    };

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

    /// <summary>Set by the checker; null if the expression had an error. The emitter uses it for C-style int conditions.</summary>
    public FcType? Type { get; set; }

    /// <summary>
    /// Set by the checker when this expression is used where another type is expected and C converts it
    /// implicitly: a bool used as an int (true is 1), an int stored in a bool (nonzero is true), a char
    /// promoted to int, an int narrowed to a char. F# never converts implicitly, so the emitter writes
    /// the conversion out.
    /// </summary>
    public FcType? ConvertTo { get; set; }
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

/// <summary>"(type) operand": an explicit conversion.</summary>
public sealed class CastExpr(FcType target, Expr operand, SourceLocation location) : Expr(location)
{
    public FcType Target { get; } = target;
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

    /// <summary>
    /// Set by the checker: false when control can never flow past this statement
    /// (it returns, breaks, continues, or loops forever on every path).
    /// </summary>
    public bool CanCompleteNormally { get; set; } = true;
}

/// <summary>"{ ... }": a nested block with its own scope.</summary>
public sealed class BlockStmt(IReadOnlyList<Stmt> statements, SourceLocation location) : Stmt(location)
{
    /// <summary>The checker removes statements that can never run, so the emitter only sees live code.</summary>
    public IReadOnlyList<Stmt> Statements { get; set; } = statements;
}

/// <summary>A lone ";".</summary>
public sealed class EmptyStmt(SourceLocation location) : Stmt(location);

/// <summary>One "name = initializer" part of a typed declaration.</summary>
public sealed class VarDeclarator(string name, SourceLocation nameLocation, Expr initializer, bool hasInitializer)
{
    public string Name { get; } = name;
    public SourceLocation NameLocation { get; } = nameLocation;

    /// <summary>For "int x;" (no initializer in the source) the parser supplies a literal 0 or false.</summary>
    public Expr Initializer { get; } = initializer;

    /// <summary>False for "int x;", which C leaves indeterminate and Functional C starts at zero.</summary>
    public bool HasInitializer { get; } = hasInitializer;

    /// <summary>Set by the checker.</summary>
    public ValueSymbol? Symbol { get; set; }
}

/// <summary>"int a = 1, b = 2;": C-style typed declarations, which are mutable variables. No new scope.</summary>
public sealed class DeclarationStmt(FcType type, IReadOnlyList<VarDeclarator> declarators, SourceLocation location)
    : Stmt(location)
{
    public FcType Type { get; } = type;
    public IReadOnlyList<VarDeclarator> Declarators { get; } = declarators;
}

public enum AssignOp
{
    Set,
    Add,
    Subtract,
    Multiply,
    Divide,
    Remainder,
    Increment,
    Decrement,
}

public static class AssignOpText
{
    public static string Source(this AssignOp op) => op switch
    {
        AssignOp.Set => "=",
        AssignOp.Add => "+=",
        AssignOp.Subtract => "-=",
        AssignOp.Multiply => "*=",
        AssignOp.Divide => "/=",
        AssignOp.Remainder => "%=",
        AssignOp.Increment => "++",
        AssignOp.Decrement => "--",
        _ => throw new ArgumentOutOfRangeException(nameof(op)),
    };
}

public sealed class IfStmt(Expr condition, Stmt then, Stmt? @else, SourceLocation location) : Stmt(location)
{
    public Expr Condition { get; } = condition;
    public Stmt Then { get; } = then;
    public Stmt? Else { get; } = @else;
}

/// <summary>Common shape of while / do-while / for, with flow facts filled in by the checker.</summary>
public abstract class LoopStmt(Stmt body, SourceLocation location) : Stmt(location)
{
    public Stmt Body { get; } = body;

    /// <summary>Set by the checker: a 'break' targets this loop.</summary>
    public bool UsesBreak { get; set; }

    /// <summary>Set by the checker: a 'continue' targets this loop.</summary>
    public bool UsesContinue { get; set; }
}

public sealed class WhileStmt(Expr condition, Stmt body, SourceLocation location) : LoopStmt(body, location)
{
    public Expr Condition { get; } = condition;
}

public sealed class DoWhileStmt(Stmt body, Expr condition, SourceLocation location) : LoopStmt(body, location)
{
    public Expr Condition { get; } = condition;
}

/// <summary>"for (init; condition; update) body". A missing condition means "true".</summary>
public sealed class ForStmt(IReadOnlyList<Stmt> init, Expr? condition, IReadOnlyList<Stmt> update, Stmt body, SourceLocation location)
    : LoopStmt(body, location)
{
    public IReadOnlyList<Stmt> Init { get; } = init;
    public Expr? Condition { get; } = condition;
    public IReadOnlyList<Stmt> Update { get; } = update;
}

public sealed class BreakStmt(SourceLocation location) : Stmt(location);

public sealed class ContinueStmt(SourceLocation location) : Stmt(location);

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

/// <summary>"x = e;", "x += e;" (and -= *= /= %=), "x++;", "++x;", "x--;", "--x;". Value is null for ++/--.</summary>
public sealed class AssignStmt(string name, AssignOp op, Expr? value, SourceLocation location) : Stmt(location)
{
    public string Name { get; } = name;
    public AssignOp Op { get; } = op;
    public Expr? Value { get; } = value;

    /// <summary>Set by the checker when the target is an assignable variable.</summary>
    public ValueSymbol? Symbol { get; set; }
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

    /// <summary>
    /// The checker removes statements that can never run, and adds a final "return 0;" (or false) to a
    /// function that can reach its end, so the emitter always sees a body whose every path returns.
    /// </summary>
    public IReadOnlyList<Stmt> Body { get; set; } = body;
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

    /// <summary>The identifier used for this symbol in the emitted C.</summary>
    public string CName { get; } = CNames.ForSource(name);
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

public sealed class ValueSymbol(string name, SourceLocation declaredAt, FcType? type, string kind, bool isMutable)
    : Symbol(name, declaredAt)
{
    /// <summary>Null when the initializer had a type error (suppresses cascading errors).</summary>
    public FcType? Type { get; } = type;

    /// <summary>"parameter", "variable", "let binding" or "lambda parameter" (used in messages).</summary>
    public string Kind { get; } = kind;

    /// <summary>True for C-style typed variables and function parameters; false for 'let' and lambda parameters.</summary>
    public bool IsMutable { get; } = isMutable;

    /// <summary>Set by the checker when some statement assigns to it; such variables are emitted as 'let mutable'.</summary>
    public bool IsAssigned { get; set; }
}
