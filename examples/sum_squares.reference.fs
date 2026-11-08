// Hand-written reference translation of sum_squares.fc (from the handoff document).
// It is NOT translator output and is not part of any compiled project; it documents the intended behavior.
module FunctionalCProgram

let rec sumSquares (n: int) : int =
    let square = fun (x: int) -> x * x
    if n <= 0 then
        0
    else
        square n + sumSquares (n - 1)

let sourceMain () : int =
    let result = sumSquares 5
    printfn "%d" result
    0

[<EntryPoint>]
let main (_args: string array) : int =
    sourceMain ()
