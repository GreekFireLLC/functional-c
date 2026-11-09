namespace FunctionalC.Compiler;

/// <summary>
/// Semantic validation: name resolution, block scoping (no shadowing), mutability rules, declaration-before-use,
/// control flow (break/continue placement, unreachable code, every path returns), the entry point, arity,
/// and int/bool types. Collects every error it finds instead of stopping at the first one.
/// </summary>
public sealed class Checker
{
    private const string PrintName = "print";

    private readonly List<Diagnostic> _errors = [];
    private readonly Dictionary<string, FunctionSymbol> _functions = new(StringComparer.Ordinal);
    private Dictionary<string, FunctionDecl> _allDeclarations = new(StringComparer.Ordinal);
    private string _file = "";
    private FunctionSymbol? _current;

    /// <summary>The binding whose initializer is being checked (it is not yet in scope).</summary>
    private string? _pendingName;

    /// <summary>Enclosing loops of the statement being checked, innermost on top.</summary>
    private readonly Stack<LoopStmt> _loops = new();

    /// <summary>True while checking a lambda body.</summary>
    private bool _inLambda;

    /// <summary>Variables referenced from lambda bodies in the current function, checked once all assignments are known.</summary>
    private readonly List<(ValueSymbol Symbol, SourceLocation At)> _captures = [];

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

    /// <summary>
    /// Checks that a new name is legal. A name may not shadow anything visible (enclosing blocks, parameters,
    /// functions, print); sibling blocks may reuse a name.
    /// </summary>
    private bool CheckNewName(string name, SourceLocation at, Scope scope, string kind)
    {
        if (name == PrintName)
        {
            Error(Codes.ReservedName, at, $"'print' is a built-in function and cannot be used as a {kind} name");
            return false;
        }

        if (name == _pendingName)
        {
            Error(Codes.DuplicateName, at, $"{kind} '{name}' has the same name as the binding being defined; shadowing is not allowed");
            return false;
        }

        if (scope.Lookup(name) is { } existing)
        {
            Error(Codes.DuplicateName, at,
                $"'{name}' is already defined (at {existing.DeclaredAt.Short}) in this block or an enclosing one; redeclaring or shadowing a name is not allowed, so choose another name");
            return false;
        }

        var function = _functions.GetValueOrDefault(name) ?? (_current?.Name == name ? _current : null);
        if (function is not null)
        {
            Error(Codes.DuplicateName, at,
                $"{kind} '{name}' would shadow the function '{name}' defined at {function.DeclaredAt.Short}; shadowing is not allowed");
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
                $"'{name}' cannot be used inside its own initializer; recursive bindings are not supported");
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
                $"'{name}' is defined later (at {later.NameLocation.Short}); forward references and mutual recursion are not supported yet, so define '{name}' before this use");
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
        _captures.Clear();
        var scope = new Scope(null);
        foreach (var parameter in function.Parameters)
        {
            if (CheckNewName(parameter.Name, parameter.Location, scope, "parameter"))
            {
                // Parameters are C-style typed declarations, so (as in C) they are mutable local copies.
                parameter.Symbol = new ValueSymbol(parameter.Name, parameter.Location, parameter.Type, "parameter", isMutable: true);
                scope.Add(parameter.Symbol);
            }
        }

        // The body shares the parameters' scope, as in C: a local cannot redeclare a parameter.
        bool fallsOffEnd = CheckStatements(function.Body, scope);
        if (fallsOffEnd)
        {
            Error(Codes.ReturnPlacement, function.CloseBraceLocation,
                $"function '{function.Name}' can reach its end without returning a value; every path must end with 'return <expression>;'");
        }

        foreach (var (captured, at) in _captures)
        {
            if (captured.IsAssigned)
            {
                Error(Codes.MutableCapture, at,
                    $"a lambda cannot capture '{captured.Name}' (declared at {captured.DeclaredAt.Short}) because it is a mutable variable that is assigned; copy its current value into an immutable binding first, e.g. 'let {captured.Name}Value = {captured.Name};'");
            }
        }

        _captures.Clear();
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
            Error(Codes.EntryPoint, main.Parameters[0].Location, "'main' must not take parameters");
        }
    }

    // ------------------------------------------------------------ statements

    /// <summary>Checks a statement sequence in the given scope. Returns true if control can flow past its end.</summary>
    private bool CheckStatements(IReadOnlyList<Stmt> statements, Scope scope)
    {
        bool reachable = true;
        bool reported = false;
        foreach (var statement in statements)
        {
            if (!reachable && !reported && statement is not EmptyStmt)
            {
                Error(Codes.Unreachable, statement.Location,
                    "unreachable statement: the statement before it never finishes normally (it returns, breaks, continues, or loops forever)");
                reported = true;
            }

            if (!CheckStatement(statement, scope))
            {
                reachable = false;
            }
        }

        return reachable;
    }

    /// <summary>Checks one statement. Returns (and records) whether control can flow past it.</summary>
    private bool CheckStatement(Stmt statement, Scope scope)
    {
        bool completes = statement switch
        {
            EmptyStmt => true,
            BlockStmt block => CheckStatements(block.Statements, new Scope(scope)),
            LetStmt let => CheckLet(let, scope),
            DeclarationStmt declaration => CheckDeclaration(declaration, scope),
            AssignStmt assign => CheckAssignment(assign, scope),
            ExprStmt expression => CheckExpressionStatement(expression, scope),
            ReturnStmt ret => CheckReturn(ret, scope),
            IfStmt branch => CheckIf(branch, scope),
            WhileStmt loop => CheckWhile(loop, scope),
            DoWhileStmt loop => CheckDoWhile(loop, scope),
            ForStmt loop => CheckFor(loop, scope),
            BreakStmt jump => CheckJump(jump, "break"),
            ContinueStmt jump => CheckJump(jump, "continue"),
            _ => throw new InvalidOperationException($"unexpected statement node {statement.GetType().Name}"),
        };
        statement.CanCompleteNormally = completes;
        return completes;
    }

    private bool CheckReturn(ReturnStmt ret, Scope scope)
    {
        var type = CheckExpression(ret.Value, scope);
        var function = _current!.Declaration;
        if (type is not null && type != function.ReturnType)
        {
            Error(Codes.TypeMismatch, ret.Value.Location,
                $"function '{function.Name}' returns {function.ReturnType.Name()} but this expression has type {type.Name()}");
        }

        return false;
    }

    /// <summary>
    /// A condition may be bool or, as in C, int: an int condition is true when it is not 0.
    /// (Those are the only two value types, so every well-typed expression is a valid condition.)
    /// </summary>
    private void CheckCondition(Expr condition, Scope scope) => CheckExpression(condition, scope);

    private bool CheckIf(IfStmt branch, Scope scope)
    {
        CheckCondition(branch.Condition, scope);
        bool thenCompletes = CheckStatement(branch.Then, new Scope(scope));
        bool elseCompletes = branch.Else is null || CheckStatement(branch.Else, new Scope(scope));
        return thenCompletes || elseCompletes;
    }

    private bool CheckLoopBody(LoopStmt loop, Scope scope)
    {
        _loops.Push(loop);
        bool completes = CheckStatement(loop.Body, new Scope(scope));
        _loops.Pop();
        return completes;
    }

    private bool CheckWhile(WhileStmt loop, Scope scope)
    {
        CheckCondition(loop.Condition, scope);
        CheckLoopBody(loop, scope);
        return !Flow.IsAlwaysTrue(loop.Condition) || loop.UsesBreak;
    }

    private bool CheckDoWhile(DoWhileStmt loop, Scope scope)
    {
        bool bodyCompletes = CheckLoopBody(loop, scope);
        CheckCondition(loop.Condition, scope); // body declarations are not visible here, as in C
        bool conditionReachable = bodyCompletes || loop.UsesContinue;
        return loop.UsesBreak || (conditionReachable && !Flow.IsAlwaysTrue(loop.Condition));
    }

    private bool CheckFor(ForStmt loop, Scope scope)
    {
        var loopScope = new Scope(scope); // variables declared in the initializer belong to the loop
        foreach (var init in loop.Init)
        {
            CheckStatement(init, loopScope);
        }

        if (loop.Condition is not null)
        {
            CheckCondition(loop.Condition, loopScope);
        }

        CheckLoopBody(loop, loopScope);
        foreach (var update in loop.Update)
        {
            CheckStatement(update, loopScope);
        }

        return !Flow.IsAlwaysTrue(loop.Condition) || loop.UsesBreak;
    }

    private bool CheckJump(Stmt jump, string keyword)
    {
        if (_loops.Count == 0)
        {
            Error(Codes.JumpOutsideLoop, jump.Location, $"'{keyword}' can only be used inside a loop (while, do ... while, or for)");
            return true; // already an error; do not also report the following statements as unreachable
        }

        if (jump is BreakStmt)
        {
            _loops.Peek().UsesBreak = true;
        }
        else
        {
            _loops.Peek().UsesContinue = true;
        }

        return false;
    }

    private bool CheckLet(LetStmt let, Scope scope)
    {
        bool nameOk = CheckNewName(let.Name, let.NameLocation, scope, "let binding");
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
            symbol = new ValueSymbol(let.Name, let.NameLocation, type, "let binding", isMutable: false);
        }

        _pendingName = null;
        let.Symbol = symbol;
        if (nameOk)
        {
            scope.Add(symbol);
        }

        return true;
    }

    private bool CheckDeclaration(DeclarationStmt declaration, Scope scope)
    {
        foreach (var declarator in declaration.Declarators)
        {
            bool nameOk = CheckNewName(declarator.Name, declarator.NameLocation, scope, "variable");
            _pendingName = declarator.Name;
            var type = CheckExpression(declarator.Initializer, scope);
            _pendingName = null;
            if (type is not null && type != declaration.Type)
            {
                Error(Codes.TypeMismatch, declarator.Initializer.Location,
                    $"cannot initialize the {declaration.Type.Name()} variable '{declarator.Name}' with a value of type {type.Name()}");
            }

            declarator.Symbol = new ValueSymbol(declarator.Name, declarator.NameLocation, declaration.Type, "variable", isMutable: true);
            if (nameOk)
            {
                scope.Add(declarator.Symbol);
            }
        }

        return true;
    }

    private bool CheckAssignment(AssignStmt assign, Scope scope)
    {
        var valueType = assign.Value is null ? null : CheckExpression(assign.Value, scope);
        var symbol = scope.Lookup(assign.Name) ?? _functions.GetValueOrDefault(assign.Name);
        string op = assign.Op.Source();
        switch (symbol)
        {
            case ValueSymbol { IsMutable: true } variable:
                variable.IsAssigned = true;
                assign.Symbol = variable;
                if (assign.Op == AssignOp.Set)
                {
                    if (valueType is not null && variable.Type is not null && valueType != variable.Type)
                    {
                        Error(Codes.TypeMismatch, assign.Value!.Location,
                            $"cannot assign a value of type {valueType.Name()} to the {variable.Type.Name()} {variable.Kind} '{assign.Name}'");
                    }
                }
                else
                {
                    if (variable.Type is FcType.Bool)
                    {
                        Error(Codes.TypeMismatch, assign.Location, $"'{op}' needs an int variable, but '{assign.Name}' is bool");
                    }

                    if (valueType is FcType.Bool)
                    {
                        Error(Codes.TypeMismatch, assign.Value!.Location, $"'{op}' needs an int value but this expression has type bool");
                    }
                }

                break;

            case ValueSymbol value:
                Error(Codes.ImmutableBinding, assign.Location,
                    value.Kind == "lambda parameter"
                        ? $"cannot assign to '{assign.Name}': lambda parameters are immutable"
                        : $"cannot assign to '{assign.Name}': 'let' bindings are immutable ('{assign.Name}' was bound at {value.DeclaredAt.Short}); declare it as '{value.Type.Name()} {assign.Name} = ...;' if it needs to change");
                break;

            case LambdaSymbol lambda:
                Error(Codes.ImmutableBinding, assign.Location,
                    $"cannot assign to '{assign.Name}': it is a 'let'-bound lambda (bound at {lambda.DeclaredAt.Short}) and bindings are immutable");
                break;

            case FunctionSymbol function:
                Error(Codes.ImmutableBinding, assign.Location,
                    $"cannot assign to the function '{assign.Name}' (defined at {function.DeclaredAt.Short})");
                break;

            default:
                Error(Codes.UnknownName, assign.Location,
                    $"cannot assign to unknown name '{assign.Name}'; declare it first, e.g. 'int {assign.Name} = 0;'");
                break;
        }

        return true;
    }

    private bool CheckExpressionStatement(ExprStmt statement, Scope scope)
    {
        if (statement.Expression is CallExpr { Callee: PrintName } print)
        {
            print.IsPrint = true;
            CheckPrintArguments(print, scope);
            return true;
        }

        if (statement.Expression is CallExpr call)
        {
            CheckCall(call, scope);
            return true;
        }

        CheckExpression(statement.Expression, scope);
        Error(Codes.DiscardedExpression, statement.Location,
            "only calls, assignments and increments can be used as statements; this expression's value would be discarded");
        return true;
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
                parameter.Symbol = new ValueSymbol(parameter.Name, parameter.Location, parameter.Type, "lambda parameter", isMutable: false);
                lambdaScope.Add(parameter.Symbol);
            }
        }

        _inLambda = true;
        var type = CheckExpression(lambda.Body, lambdaScope);
        _inLambda = false;
        return type;
    }

    /// <summary>Returns the expression's type, or null if an error was reported (to avoid cascading errors).</summary>
    private FcType? CheckExpression(Expr expression, Scope scope)
    {
        var type = CheckExpressionCore(expression, scope);
        expression.Type = type;
        return type;
    }

    private FcType? CheckExpressionCore(Expr expression, Scope scope)
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
                        if (_inLambda && value.Kind != "lambda parameter")
                        {
                            _captures.Add((value, name.Location));
                        }

                        return value.Type;
                    case null:
                        return null;
                    default:
                        Error(Codes.NotAValue, name.Location,
                            $"'{name.Name}' is a function; it can only be called, as in '{name.Name}(...)' (functions are not values yet)");
                        return null;
                }
            }

            case UnaryExpr { Op: UnaryOp.Not } negation:
                // As in C, '!' accepts an int ('!n' is true when n is 0); the result is always bool.
                CheckExpression(negation.Operand, scope);
                return FcType.Bool;

            case UnaryExpr unary:
            {
                var operand = CheckExpression(unary.Operand, scope);
                if (operand is not null && operand != FcType.Int)
                {
                    Error(Codes.TypeMismatch, unary.Location,
                        $"operator '{unary.Op.Source()}' expects an int operand but got {operand.Name()}");
                }

                return FcType.Int;
            }

            case BinaryExpr binary:
                return CheckBinary(binary, scope);

            case ConditionalExpr conditional:
            {
                CheckCondition(conditional.Condition, scope);
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
                    "a lambda can only be the entire initializer of a 'let' binding, as in 'let f = (int x) => x + 1;'");
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
                    $"operator '{op}' expects {expected.Name()} operands but the {side} operand has type {actual.Name()}");
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
                // As in C, each operand may be bool or int (an int is true when it is not 0); the result is bool.
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
