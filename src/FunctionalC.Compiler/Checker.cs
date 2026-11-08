namespace FunctionalC.Compiler;

/// <summary>
/// Semantic validation for version 0: name resolution, scoping (no shadowing), immutability,
/// declaration-before-use, the single final return, the entry point, arity, and int/bool types.
/// Collects every error it finds instead of stopping at the first one.
/// </summary>
public sealed class Checker
{
    private const string PrintName = "print";

    private readonly List<Diagnostic> _errors = [];
    private readonly Dictionary<string, FunctionSymbol> _functions = new(StringComparer.Ordinal);
    private Dictionary<string, FunctionDecl> _allDeclarations = new(StringComparer.Ordinal);
    private string _file = "";
    private FunctionSymbol? _current;

    /// <summary>The let binding whose initializer is being checked (it is not yet in scope).</summary>
    private string? _pendingName;

    public IReadOnlyList<Diagnostic> Check(ProgramNode program)
    {
        _file = program.File;
        _allDeclarations = program.Functions
            .GroupBy(f => f.Name, StringComparer.Ordinal)
            .ToDictionary(g => g.Key, g => g.First(), StringComparer.Ordinal);

        foreach (var function in program.Functions)
        {
            CheckFunction(function);
        }

        CheckEntryPoint(program);
        return _errors
            .OrderBy(d => d.Location.Line)
            .ThenBy(d => d.Location.Column)
            .ToList();
    }

    private void Error(string code, SourceLocation at, string message) => _errors.Add(new Diagnostic(code, message, at));

    // ------------------------------------------------------------ scopes

    private sealed class Scope(Scope? parent)
    {
        private readonly Dictionary<string, Symbol> _symbols = new(StringComparer.Ordinal);

        public Symbol? Lookup(string name) =>
            _symbols.TryGetValue(name, out var symbol) ? symbol : parent?.Lookup(name);

        public void Add(Symbol symbol) => _symbols[symbol.Name] = symbol;
    }

    /// <summary>Checks that a new local/parameter name is legal. Shadowing of any visible name is rejected in version 0.</summary>
    private bool CheckNewName(string name, SourceLocation at, Scope scope, string kind)
    {
        if (name == PrintName)
        {
            Error(Codes.ReservedName, at, $"'print' is a built-in function and cannot be used as a {kind} name");
            return false;
        }

        if (name == _pendingName)
        {
            Error(Codes.DuplicateName, at, $"{kind} '{name}' has the same name as the binding being defined; shadowing is not allowed in version 0");
            return false;
        }

        if (scope.Lookup(name) is { } existing)
        {
            Error(Codes.DuplicateName, at,
                $"'{name}' is already defined in this scope (at {existing.DeclaredAt.Short}); names must be unique within a function and shadowing is not allowed in version 0");
            return false;
        }

        var function = _functions.GetValueOrDefault(name) ?? (_current?.Name == name ? _current : null);
        if (function is not null)
        {
            Error(Codes.DuplicateName, at,
                $"{kind} '{name}' would shadow the function '{name}' defined at {function.DeclaredAt.Short}; shadowing is not allowed in version 0");
            return false;
        }

        return true;
    }

    /// <summary>Resolves a name; reports unknown names, forward references and self-references. Returns null after an error.</summary>
    private Symbol? Resolve(string name, SourceLocation at, Scope scope)
    {
        if (scope.Lookup(name) is { } local)
        {
            return local;
        }

        if (name == _pendingName)
        {
            Error(Codes.SelfReference, at,
                $"'{name}' cannot be used inside its own initializer; recursive 'let' bindings are not supported in version 0");
            return null;
        }

        if (_functions.TryGetValue(name, out var function))
        {
            return function;
        }

        if (_current?.Name == name)
        {
            return _current;
        }

        if (_allDeclarations.TryGetValue(name, out var later))
        {
            Error(Codes.ForwardReference, at,
                $"'{name}' is defined later (at {later.NameLocation.Short}); forward references and mutual recursion are not supported in version 0, so define '{name}' before this use");
            return null;
        }

        Error(Codes.UnknownName, at, $"unknown name '{name}'");
        return null;
    }

    // ------------------------------------------------------------ declarations

    private void CheckFunction(FunctionDecl function)
    {
        var symbol = new FunctionSymbol(function);
        function.Symbol = symbol;

        if (function.Name == PrintName)
        {
            Error(Codes.ReservedName, function.NameLocation, "'print' is a built-in function and cannot be redefined");
        }
        else if (_functions.TryGetValue(function.Name, out var previous))
        {
            if (function.Name != "main") // duplicate main is reported by CheckEntryPoint
            {
                Error(Codes.DuplicateName, function.NameLocation,
                    $"function '{function.Name}' is already defined at {previous.DeclaredAt.Short}");
            }
        }
        else
        {
            _functions.Add(function.Name, symbol); // registered before the body so self-recursion resolves
        }

        _current = symbol;
        var scope = new Scope(null);
        foreach (var parameter in function.Parameters)
        {
            if (CheckNewName(parameter.Name, parameter.Location, scope, "parameter"))
            {
                parameter.Symbol = new ValueSymbol(parameter.Name, parameter.Location, parameter.Type, "parameter");
                scope.Add(parameter.Symbol);
            }
        }

        for (int i = 0; i < function.Body.Count; i++)
        {
            bool isLast = i == function.Body.Count - 1;
            switch (function.Body[i])
            {
                case ReturnStmt ret:
                    if (!isLast)
                    {
                        Error(Codes.ReturnPlacement, ret.Location,
                            "'return' must be the last statement of a function body; early returns are not supported in version 0");
                    }

                    var type = CheckExpression(ret.Value, scope);
                    if (type is not null && type != function.ReturnType)
                    {
                        Error(Codes.TypeMismatch, ret.Value.Location,
                            $"function '{function.Name}' returns {function.ReturnType.Name()} but this expression has type {type.Name()}");
                    }

                    break;

                case LetStmt let:
                    CheckLet(let, scope);
                    break;

                case AssignStmt assign:
                    CheckAssignment(assign, scope);
                    break;

                case ExprStmt statement:
                    CheckExpressionStatement(statement, scope);
                    break;
            }
        }

        if (function.Body.Count == 0 || function.Body[^1] is not ReturnStmt)
        {
            Error(Codes.ReturnPlacement, function.CloseBraceLocation,
                $"function '{function.Name}' must end with 'return <expression>;'");
        }

        _current = null;
    }

    private void CheckEntryPoint(ProgramNode program)
    {
        var mains = program.Functions.Where(f => f.Name == "main").ToList();
        if (mains.Count == 0)
        {
            Error(Codes.EntryPoint, new SourceLocation(_file, 1, 1), "the program must define exactly one 'int main()'; no 'main' function was found");
            return;
        }

        foreach (var duplicate in mains.Skip(1))
        {
            Error(Codes.EntryPoint, duplicate.NameLocation,
                $"duplicate 'main': the program must define exactly one 'int main()' (first defined at {mains[0].NameLocation.Short})");
        }

        var main = mains[0];
        if (main.ReturnType != FcType.Int)
        {
            Error(Codes.EntryPoint, main.ReturnTypeLocation, $"'main' must return 'int' (its value is the process exit code), not '{main.ReturnType.Name()}'");
        }

        if (main.Parameters.Count > 0)
        {
            Error(Codes.EntryPoint, main.Parameters[0].Location, "'main' must not take parameters in version 0");
        }
    }

    // ------------------------------------------------------------ statements

    private void CheckLet(LetStmt let, Scope scope)
    {
        bool nameOk = CheckNewName(let.Name, let.NameLocation, scope, "local binding");
        _pendingName = let.Name;
        Symbol symbol;
        if (let.Initializer is LambdaExpr lambda)
        {
            var returnType = CheckLambda(lambda, scope);
            symbol = new LambdaSymbol(let.Name, let.NameLocation, lambda.Parameters.Select(p => p.Type).ToList(), returnType);
        }
        else
        {
            var type = CheckExpression(let.Initializer, scope);
            symbol = new ValueSymbol(let.Name, let.NameLocation, type, "local binding");
        }

        _pendingName = null;
        let.Symbol = symbol;
        if (nameOk)
        {
            scope.Add(symbol);
        }
    }

    private void CheckAssignment(AssignStmt assign, Scope scope)
    {
        CheckExpression(assign.Value, scope);
        var symbol = scope.Lookup(assign.Name) ?? _functions.GetValueOrDefault(assign.Name);
        switch (symbol)
        {
            case ValueSymbol value:
                Error(Codes.ImmutableBinding, assign.Location,
                    $"cannot assign to '{assign.Name}': {value.Kind}s are immutable ('{assign.Name}' was bound at {value.DeclaredAt.Short})");
                break;
            case LambdaSymbol lambda:
                Error(Codes.ImmutableBinding, assign.Location,
                    $"cannot assign to '{assign.Name}': bindings are immutable ('{assign.Name}' was bound at {lambda.DeclaredAt.Short})");
                break;
            case FunctionSymbol function:
                Error(Codes.ImmutableBinding, assign.Location,
                    $"cannot assign to the function '{assign.Name}' (defined at {function.DeclaredAt.Short})");
                break;
            default:
                Error(Codes.UnknownName, assign.Location,
                    $"cannot assign to unknown name '{assign.Name}'; assignment is not supported (use 'let' to bind a new immutable name)");
                break;
        }
    }

    private void CheckExpressionStatement(ExprStmt statement, Scope scope)
    {
        if (statement.Expression is CallExpr { Callee: PrintName } print)
        {
            print.IsPrint = true;
            CheckPrintArguments(print, scope);
            return;
        }

        if (statement.Expression is CallExpr call)
        {
            CheckCall(call, scope);
            return;
        }

        CheckExpression(statement.Expression, scope);
        Error(Codes.DiscardedExpression, statement.Location,
            "only calls can be used as statements; this expression's value would be discarded");
    }

    private void CheckPrintArguments(CallExpr print, Scope scope)
    {
        var types = print.Arguments.Select(a => CheckExpression(a, scope)).ToList();
        if (print.Arguments.Count != 1)
        {
            Error(Codes.Arity, print.Location, $"'print' expects exactly 1 argument (an int) but was given {print.Arguments.Count}");
            return;
        }

        if (types[0] is FcType.Bool)
        {
            Error(Codes.TypeMismatch, print.Arguments[0].Location, "'print' expects an int argument but this expression has type bool");
        }
    }

    // ------------------------------------------------------------ expressions

    private FcType? CheckLambda(LambdaExpr lambda, Scope scope)
    {
        var lambdaScope = new Scope(scope);
        foreach (var parameter in lambda.Parameters)
        {
            if (CheckNewName(parameter.Name, parameter.Location, lambdaScope, "lambda parameter"))
            {
                parameter.Symbol = new ValueSymbol(parameter.Name, parameter.Location, parameter.Type, "lambda parameter");
                lambdaScope.Add(parameter.Symbol);
            }
        }

        return CheckExpression(lambda.Body, lambdaScope);
    }

    /// <summary>Returns the expression's type, or null if an error was reported (to avoid cascading errors).</summary>
    private FcType? CheckExpression(Expr expression, Scope scope)
    {
        switch (expression)
        {
            case IntLiteralExpr:
                return FcType.Int;

            case BoolLiteralExpr:
                return FcType.Bool;

            case NameExpr name:
            {
                if (name.Name == PrintName)
                {
                    Error(Codes.PrintMisuse, name.Location, "'print' is a built-in statement, not a value; call it as 'print(value);'");
                    return null;
                }

                var symbol = Resolve(name.Name, name.Location, scope);
                name.Symbol = symbol;
                switch (symbol)
                {
                    case ValueSymbol value:
                        return value.Type;
                    case null:
                        return null;
                    default:
                        Error(Codes.NotAValue, name.Location,
                            $"'{name.Name}' is a function; it can only be called, as in '{name.Name}(...)' (functions are not values in version 0)");
                        return null;
                }
            }

            case UnaryExpr unary:
            {
                var expected = unary.Op == UnaryOp.Negate ? FcType.Int : FcType.Bool;
                var operand = CheckExpression(unary.Operand, scope);
                if (operand is not null && operand != expected)
                {
                    Error(Codes.TypeMismatch, unary.Location,
                        $"operator '{unary.Op.Source()}' expects a {expected.Name()} operand but got {operand.Name()}"
                        + (expected == FcType.Bool ? "; there is no implicit int-to-bool conversion" : ""));
                }

                return expected;
            }

            case BinaryExpr binary:
                return CheckBinary(binary, scope);

            case ConditionalExpr conditional:
            {
                var condition = CheckExpression(conditional.Condition, scope);
                if (condition is FcType.Int)
                {
                    Error(Codes.TypeMismatch, conditional.Condition.Location,
                        "the condition of '?:' must be bool but has type int; there is no implicit int-to-bool conversion (compare explicitly, e.g. 'x != 0')");
                }

                var whenTrue = CheckExpression(conditional.WhenTrue, scope);
                var whenFalse = CheckExpression(conditional.WhenFalse, scope);
                if (whenTrue is not null && whenFalse is not null && whenTrue != whenFalse)
                {
                    Error(Codes.TypeMismatch, conditional.QuestionLocation,
                        $"both branches of '?:' must have the same type, but they are {whenTrue.Name()} and {whenFalse.Name()}");
                    return null;
                }

                return whenTrue ?? whenFalse;
            }

            case CallExpr { Callee: PrintName } print:
                foreach (var argument in print.Arguments)
                {
                    CheckExpression(argument, scope);
                }

                Error(Codes.PrintMisuse, print.Location, "'print(...)' produces no value; it can only be used as a statement, as in 'print(x);'");
                return null;

            case CallExpr call:
                return CheckCall(call, scope);

            case LambdaExpr lambda:
                Error(Codes.LambdaPosition, lambda.Location,
                    "a lambda can only be the entire initializer of a 'let' binding in version 0, as in 'let f = (int x) => x + 1;'");
                return null;

            default:
                throw new InvalidOperationException($"unexpected expression node {expression.GetType().Name}");
        }
    }

    private FcType? CheckBinary(BinaryExpr binary, Scope scope)
    {
        var left = CheckExpression(binary.Left, scope);
        var right = CheckExpression(binary.Right, scope);
        string op = binary.Op.Source();

        void Require(FcType expected, FcType? actual, string side)
        {
            if (actual is not null && actual != expected)
            {
                Error(Codes.TypeMismatch, binary.OperatorLocation,
                    $"operator '{op}' expects {expected.Name()} operands but the {side} operand has type {actual.Name()}"
                    + (expected == FcType.Bool ? "; there is no implicit int-to-bool conversion" : ""));
            }
        }

        switch (binary.Op)
        {
            case BinaryOp.Add or BinaryOp.Subtract or BinaryOp.Multiply or BinaryOp.Divide or BinaryOp.Remainder:
                Require(FcType.Int, left, "left");
                Require(FcType.Int, right, "right");
                return FcType.Int;

            case BinaryOp.Less or BinaryOp.LessEqual or BinaryOp.Greater or BinaryOp.GreaterEqual:
                Require(FcType.Int, left, "left");
                Require(FcType.Int, right, "right");
                return FcType.Bool;

            case BinaryOp.Equal or BinaryOp.NotEqual:
                if (left is not null && right is not null && left != right)
                {
                    Error(Codes.TypeMismatch, binary.OperatorLocation,
                        $"operator '{op}' needs operands of the same type, but they are {left.Name()} and {right.Name()}");
                }

                return FcType.Bool;

            case BinaryOp.And or BinaryOp.Or:
                Require(FcType.Bool, left, "left");
                Require(FcType.Bool, right, "right");
                return FcType.Bool;

            default:
                throw new InvalidOperationException($"unexpected operator {binary.Op}");
        }
    }

    private FcType? CheckCall(CallExpr call, Scope scope)
    {
        var symbol = Resolve(call.Callee, call.Location, scope);
        var argumentTypes = call.Arguments.Select(a => CheckExpression(a, scope)).ToList();
        call.Symbol = symbol;

        switch (symbol)
        {
            case null:
            case ValueSymbol { Type: null }: // its initializer already reported an error
                return null;

            case ValueSymbol value:
                Error(Codes.NotCallable, call.Location,
                    $"'{call.Callee}' is a {value.Kind} of type {value.Type.Name()}, not a function, so it cannot be called");
                return null;
        }

        var callable = (ICallable)symbol;
        if (ReferenceEquals(symbol, _current))
        {
            _current!.IsRecursive = true;
        }

        if (call.Arguments.Count != callable.ParameterTypes.Count)
        {
            Error(Codes.Arity, call.Location,
                $"'{call.Callee}' expects {callable.ParameterTypes.Count} argument(s) but was given {call.Arguments.Count}");
            return callable.ReturnType;
        }

        for (int i = 0; i < argumentTypes.Count; i++)
        {
            if (argumentTypes[i] is { } actual && actual != callable.ParameterTypes[i])
            {
                Error(Codes.TypeMismatch, call.Arguments[i].Location,
                    $"argument {i + 1} of '{call.Callee}' must be {callable.ParameterTypes[i].Name()} but has type {actual.Name()}");
            }
        }

        return callable.ReturnType;
    }
}
