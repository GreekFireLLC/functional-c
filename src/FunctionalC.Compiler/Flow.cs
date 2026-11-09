namespace FunctionalC.Compiler;

/// <summary>Syntactic control-flow queries shared by the checker and the emitter.</summary>
public static class Flow
{
    /// <summary>The statements of a block, or the single statement itself.</summary>
    public static IReadOnlyList<Stmt> Flatten(Stmt statement) =>
        statement is BlockStmt block ? block.Statements : [statement];

    /// <summary>True if a 'return' appears anywhere inside (including nested loops).</summary>
    public static bool ContainsReturn(Stmt statement) => statement switch
    {
        ReturnStmt => true,
        BlockStmt block => block.Statements.Any(ContainsReturn),
        IfStmt branch => ContainsReturn(branch.Then) || (branch.Else is { } e && ContainsReturn(e)),
        LoopStmt loop => ContainsReturn(loop.Body),
        _ => false,
    };

    /// <summary>True if a 'break' inside targets the enclosing loop (breaks inside nested loops do not count).</summary>
    public static bool ContainsBreak(Stmt statement) => statement switch
    {
        BreakStmt => true,
        BlockStmt block => block.Statements.Any(ContainsBreak),
        IfStmt branch => ContainsBreak(branch.Then) || (branch.Else is { } e && ContainsBreak(e)),
        _ => false,
    };

    /// <summary>True if a 'continue' inside targets the enclosing loop (nested loops do not count).</summary>
    public static bool ContainsContinue(Stmt statement) => statement switch
    {
        ContinueStmt => true,
        BlockStmt block => block.Statements.Any(ContainsContinue),
        IfStmt branch => ContainsContinue(branch.Then) || (branch.Else is { } e && ContainsContinue(e)),
        _ => false,
    };

    /// <summary>A loop condition written literally as 'true' (or omitted in 'for'), which never ends the loop.</summary>
    public static bool IsAlwaysTrue(Expr? condition) => condition is null or BoolLiteralExpr { Value: true };
}
