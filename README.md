# Runa — Toolchain (1.0)

Runa is a new programming language designed by *you*, the language designer.

**Runa 1.0** is the first complete release. On top of the baseline language
(variables, arithmetic, if/elif/else, loops, functions, classes, interfaces,
inheritance, statics, visibility, abstract, nested classes, collections,
packages, FFI) it adds the 1.0 contract (DECISIONS #106-#121):

- **Comments**: `#/` single-line; `*/ ... *\` multi-line (first `*\` closes,
  no nesting). Comments produce no tokens and survive verbatim in strings.
- **Float literals**: `1.5`, `2.0` — a second official numeric type. Any
  arithmetic with a float operand computes in double (`1.5 + 2` -> `3.5`),
  Float `%` is fmod, and Infinity/NaN are never produced silently: float
  overflow and float division/modulo by zero are clean RuntimeErrors.
- **Equality is strict**: `2.0 == 2` -> `false` (Integer and Float are two
  types). **Ordering is numeric**: `2.5 > 2` -> `true`.
- **Comparisons are full expressions**: `== != > < >= <= === is` work in
  assignments, arguments and conditions (one comparison per level;
  parenthesized comparisons nest).
- **Optional `;`** as a statement separator: `a = 1; b = 2`.
- **Unicode identifiers** (XID_Start / XID_Continue): `تعداد = 5` works.
  Error columns count characters, not bytes.
- **Escapes** in strings: `\n \t \" \\`; raw multiline strings with
  triple quotes `"""..."""`.
- **Overloads by arity**: top-level functions and constructors may share a
  name with different parameter counts (`fun f(x)` / `fun f(x, y)`).
- **Qualified nested construction**: `new Car::Engine()` from outside.
- **`printr` is core reserved**: it cannot be shadowed; `printr()` prints a
  newline. `none` is not a source literal (`x = none` is a CompileError).
- **Empty collections**: `sum({})` and friends raise clean RuntimeErrors;
  `count({})` is 0, `empty({})` is true.
- **Cycle-safe equality**: structural `==` tracks object pairs (cyclic
  graphs compare correctly) with a maximum depth of 64 (RuntimeError beyond).
- **Cycle-safe lifetime**: reference counting plus a runtime-controlled
  reachability collector reclaims unreachable object cycles (roots: globals,
  stack, frame locals, self, statics, active construction plans).
- **RPR is ZIP-based** (manifest.json + init.ru, read fully from memory);
  legacy RPRF archives are rejected with a clear message. Bytecode format is
  v7 (little-endian, magic RUNA).
- **Unary minus** (DECISIONS #126): `-5`, `-x`, `-(1 + 2)`, `2 * -3` — '-'
  binds tighter than `* / %`; `-Int64.min` is a clean RuntimeError; unary
  `+` stays rejected.
- **Logical operators** (DECISIONS #127): `and` / `or` / `not` on booleans
  with Python precedence (`or` < `and` < `not` < comparisons) and real
  short-circuit evaluation — `false and (1 / 0 == 1)` is `false`, no crash.
- **String concatenation** (DECISIONS #128): `+` with a string on either
  side glues text (`"Hello!" + "Welcome!"`, `"n=" + 5` -> `n=5`); every
  non-string operand contributes exactly its printr display form.
- **String ordering** (DECISIONS #129): `< <= > >=` on two strings compare
  UTF-8 bytes (code-point order) — `"abc" < "abd"`, `"سارا" < "محمود"`.
- **Compile-time operand checks** (DECISIONS #130): provably invalid
  literal combinations (`true + true`, `null + 1`, `"a" * 2`, `not 5`)
  are CompileErrors before the program ever runs.
This repository contains the working Runa toolchain, built exactly along the
baseline specification (`spec/RUNA_BASELINE_SPEC.md`):

```text
main.ru → Lexer → Tokens → Parser → AST → Compiler → Bytecode (.rbc) → VM → Runtime
```

**Current status:** the pipeline works for all confirmed syntax so far:

```runa
name = "Runa"          # assignment (strings, numbers, booleans)
printr("Hello", name)  # the first built-in function
a = 10
b = 3
printr(a + b * 2)      # arithmetic: + - * / % with precedence and ( )
if a > b [             # the first control flow: if + six comparisons
    printr("a wins!")
] elif a == b [
    printr("tie")
] else [
    printr("b wins!")
]
fun add(x, y) [        # the function system: declare, call, return
    result = x + y
    return result
]
printr(add(10, 20))    # 30 — calls are full expressions

class (Car) [          # the class system: fields, constructor, methods
    name = "pride"
    speed = 0
    static count = 0
    constructor(name) [
        self.name = name
        Car.count = Car.count + 1
    ]
    fun accelerate(amount) [
        self.speed = self.speed + amount
    ]
]
car = new Car("BMW")   # instances with independent state
car.accelerate(20)
printr(car.name, car.getSpeed())

x = 0
loop x < 5 [           # the loop system: conditional loop
    printr(x)
    x = x + 1
]
for i = 1 to 5 [       # counting loop, both bounds inclusive
    if i == 3 [
        continue       # skip this pass
    ]
    printr(i)
]
loop true [            # break exits the nearest loop
    break
]
```

Confirmed by the designer: assignments can hold string / number / boolean
values, variables can be read, `printr(...)` prints values joined by single
spaces, arithmetic expressions (`+` `-` `*` `/` `%`) work on numbers with
standard operator precedence, `if/elif/else` chains with the six comparison
operators, functions with per-invocation local scopes and `return`, the
class system: `class (Name) [ ... ]` declarations with fields, constructors,
methods, `self`, static members, `public`/`private` visibility, inheritance
(`class (Car : Vehicle)`), `super.name(...)` calls, `override`, abstract
classes, `interface` contracts with `implements`, nested classes, method
overloading by arity, `new Name(...)` instantiation, `null`, object identity
(`===`), object equality (`==`/`!=`) and type checks (`x is Car`), and the
loop system: `loop <condition> [ ... ]`, the counting loop
`for i = <start> to <end> [ ... ]` (both bounds inclusive, fixed step +1),
`break` and `continue` acting on the nearest loop.
Everything else (closures, generics, the rest of the stdlib, ...) is
still an open design decision owned by the designer. Provisional
implementation choices are listed in [DECISIONS.md](DECISIONS.md) and wait
for approval.

---

## Repository layout

```text
Runa/
├── src/
│   ├── main.cpp            combined `runa` tool (compile + run in one step)
│   ├── lexer/              .ru source → tokens
│   ├── parser/             tokens → AST
│   ├── ast/                AST node definitions + dump
│   ├── compiler/           AST → bytecode (never executes)
│   ├── bytecode/           instruction set + provisional .rbc container (v5)
│   ├── vm/                 stack-based bytecode VM (never parses .ru)
│   ├── runtime/            typed value model + globals storage
│   │   └── native/         native bridge: NativeLoader + FFI call dispatch
│   ├── import/             RPR reader/writer, manifest validation, importer
│   │                       (search paths, package resolution), JSON, platform
│   ├── stdlib/             built-in registry + printr
│   ├── errors/             structured error system
│   └── tools/              runac/runavm/rpr/nruna mains + tool_common.hpp
├── examples/               one runnable .ru per feature (hello, types, arithmetic,
│                           precedence, if, conditions, elifelse, functions,
│                           recursion, classes, inheritance, interfaces, statics,
│                           overloading, nested_class, null_and_identity, loops,
│                           for, collections) plus 09_library (Runa package),
│                           10_native (project-local native package),
│                           11_hybrid (a library with its own embedded native
│                           backend)
├── tests/                  integration suite + one directory per class-system
│                           category (class, object, fields, methods, constructor,
│                           self, static, visibility, inheritance, super, override,
│                           polymorphism, abstract, interface, equality, identity,
│                           null, nested, overload, lifetime, type_check, errors)
│                           one per loop-system category (loop, for, break,
│                           continue, integration), and tests/collections with
│                           one directory per collection-spec section
│                           (literals, empty, duplicates, equality, contains,
│                           aggregate, set_operations, ordering, functions,
│                           reassignment, errors)
├── spec/                   RUNA_BASELINE_SPEC.md (the designer's baseline)
├── build.sh                direct g++ build (Linux/macOS)
├── build.bat               direct g++ build (Windows / MinGW, static exes)
└── DECISIONS.md            provisional choices awaiting designer approval
```

Compiler and VM are separate components, as required. The combined `runa`
tool only chains them; it does not merge them.

## Building

**Windows (GCC / MinGW):**

```bat
build.bat
```

Requires `g++` in `PATH` (MinGW-w64 / MSYS2). Produces `build\runac.exe`,
`build\runavm.exe`, `build\runa.exe`, `build\rpr.exe`, `build\nruna.exe`.
The exes are linked statically, so they run without any MinGW DLLs.

**Linux / macOS:**

```bash
./build.sh
```

No external dependencies; standard C++17; direct `g++` invocation as the
baseline spec prescribes (CMake can come later if the project outgrows it).

## Running

The classic split flow:

```bash
build/runac examples/hello.ru          # → examples/hello.rbc
build/runavm examples/hello.rbc        # executes the program
```

Output:

```text
Hello Runa
```

Inspect global variables after a run (development aid — this prints the VM's
memory, it is not program output):

```bash
build/runavm examples/hello.rbc --dump-globals
name = "Runa"
```

Or the one-step convenience tool:

```bash
build/runa examples/types.ru
Welcome to Runa
version 2
active: true
bye Runa !
```

### The language so far

```runa
name = "Runa"        # string
count = 123          # number (integer, 64-bit signed)
flag = true          # boolean (true / false)
other = name         # reading a variable
total = count * 2    # expression on the right side of '='

printr("Hello")          # one argument
printr("Hello", name)    # many arguments → Hello Runa
printr(count, flag)      # any mix of values → 123 true

printr(10 + 5)           # arithmetic on literals → 15
printr(a - b)            # arithmetic on variables
printr(2 + 3 * 4)        # precedence → 14   (* / % bind tighter than + -)
printr((2 + 3) * 4)      # parentheses → 20

if a == 10 [             # the condition is one comparison
    printr("equal")
]
if a > 5 [               # also: !=  <  >=  <=
    printr("greater")    # several statements per block are fine
    printr("much greater")
] elif a == 5 [          # elif continues the chain
    printr("five")
] else [                 # else is optional and must be last
    printr("smaller")
]
if a == 999 [
    printr("never")      # false conditions skip the whole block
]
printr("done")           # execution continues after the closing ]

fun add(a, b) [          # declare a function: fun name(params) [ body ]
    result = a + b       # assignments inside create LOCAL variables
    return result        # return hands the value to the caller
]
fun hello() [            # empty parameter list is fine
    printr("Hello")
]
hello()                  # a bare call is a valid statement

x = add(10, 20)          # a call IS an expression → 30
y = add(10, 20) * 2      # calls compose with arithmetic → 60
z = add(add(1, 2), 3)    # nested calls → 6
if add(1, 2) == 3 [      # calls work in conditions
    printr("calls everywhere")
]
fun fact(n) [            # recursion: every invocation gets its own frame
    if n <= 1 [
        return 1
    ]
    return fact(n - 1) * n
]
printr(fact(5))          # 120
fun stop() [
    return               # bare return → the distinct "none" value
]
printr(stop())           # none — never 0, false or ""

class (Car) [            # class declarations: class (Name) [ members ]
    name = "Unknown"     # instance field with a default value
    speed = 0
    static count = 0     # static field — belongs to the CLASS

    constructor(name) [  # runs automatically on 'new'
        self.name = name
        Car.count = Car.count + 1
    ]

    fun accelerate(amount) [     # instance method — 'self' is the object
        self.speed = self.speed + amount
    ]

    fun getSpeed() [
        return self.speed        # return works as in functions
    ]

    static fun fleetSize() [     # static method — no 'self' allowed
        return Car.count
    ]
]

car = new Car("BMW")     # 'new' creates an instance, runs the constructor
car.accelerate(20)       # method call
printr(car.name)         # field access
printr(Car.count)        # static access through the class
printr(Car.fleetSize())  # static method

car.name = "Ferrari"     # member assignment (also self.x = ... inside methods)

class (SportCar : Car) [          # inheritance: Car is the parent
    override fun getSpeed() [     # override a parent method
        return super.getSpeed() * 2    # 'super' calls the parent version
    ]
]

interface (Printable) [           # interfaces declare a contract
    fun print()
]
class (Page) implements Printable [   # a concrete class must implement it
    fun print() [
        printr("page")
    ]
]

abstract class (Shape) [          # abstract classes cannot be instantiated
    abstract fun area()           # abstract methods have no body
    fun describe() [
        printr("area:", self.area())
    ]
]
class (Square : Shape) [
    side = 1
    fun area() [
        return self.side * self.side
    ]
]

class (Engine) [ ... ]            # (nested inside Car: class (Engine) [ ... ])

p1 = new Car("A")                 # two objects with equal fields...
p2 = new Car("A")
if p1 == p2 [ printr("structurally equal") ]     # ...compare structurally
if p1 === p2 [ printr("same") ] else [ printr("but distinct") ]   # identity

x = null                          # null: "no object"
if x == null [ printr("nothing") ]
if x is Car [ printr("no") ] else [ printr("null is not a Car") ]

x = 0
loop x < 5 [              # loop: condition checked BEFORE each pass
    printr(x)             # 0 1 2 3 4
    x = x + 1
]

for i = 1 to 5 [          # for: counting loop, BOTH bounds inclusive
    printr(i)             # 1 2 3 4 5, step is fixed at +1
]
for i = 5 to 3 [          # start > end: zero passes, no error
    printr("never")
]
for i = 1 to 10 [         # bounds may be any expression (evaluated ONCE)
    if i == 3 [
        continue          # skip this pass; the counter still advances
    ]
    if i == 7 [
        break             # exit the nearest loop immediately
    ]
    printr(i)
]
for i = 1 to 3 [          # loops nest; break/continue hit the NEAREST loop
    loop i > 0 [
        break
    ]
]
```

Class-system semantics in one look:

- `class (Name) [ ... ]` declares a class; `class (Name : Parent) [ ... ]`
  inherits; `interface (Name) [ ... ]` declares a contract of methods
  (no bodies); `abstract class` / `abstract fun` declare non-instantiable
  classes and body-less methods; `class (X) implements A, B` checks the
  contracts at compile time.
- Members: fields (`name = <default>`), methods (`fun name(params) [...]`),
  one optional `constructor(params) [...]`, and nested classes
  (`class (Inner) [ ... ]`, visible only inside the enclosing class).
  Modifiers: `public` / `private` / `static` / `abstract` / `override`.
- Each `new` produces an object with **independent** state: fields are
  initialized with their defaults (base class first), then the constructor
  runs with the arguments (constructors are not inherited; a class without
  one takes `new X()` with no arguments). Constructors return nothing
  (a bare `return` exits early).
- `self` references the current object inside instance methods and
  constructors (`self.field`, `self.method(...)`, and `self` itself is a
  value — `printr(self)` prints `Car#1`). It is unavailable in static
  methods and field initializers.
- Static members belong to the class: `Car.count`, `Car.make()`. Static
  fields initialize once at program start (declaration order) and are shared
  by all instances; inherited statics resolve to the declaring class.
- Visibility: `private` members are accessible only from the declaring
  class itself (including other instances of it); everything else is
  `public`. Illegal access is a compile error when statically checkable and
  a clean runtime error through dynamic receivers.
- Method calls on dynamically-typed receivers dispatch at **runtime** from
  the object's actual class (polymorphism): the (name, argument-count)
  overload is chosen walking from the runtime class up the parent chain.
  Overloads (same name, different arities) are allowed; a concrete method
  shadowing a parent method requires `override`; `super.name(...)` starts
  the lookup at the parent of the class where the current method was
  declared, keeping `self`.
- `==`/`!=` on two objects of the same class compare **structure** (field by
  field, recursively); different classes simply compare unequal.
  `===` is object **identity** (same reference — `car2 = car1` then
  `car1 === car2` is true). `is` checks type compatibility through the whole
  inheritance chain and against implemented interfaces (`null is X` is
  false).
- `null` is its own value (distinct from `none`): using a member on `null`
  is a clean `null dereference` runtime error, never a crash.
- Objects are reference-counted: assigning or passing an object shares the
  reference, and memory is reclaimed automatically when the last reference
  is gone. There is no `delete`/`free`/destructor syntax by design.
- Classes are not values: assigning to a class name, reading one, or calling
  one directly is a compile error; instantiate with `new` and access
  statics through `ClassName.member`.
- All class errors (unknown class, instantiating an abstract class or
  interface, constructor arity, unknown members, illegal private access,
  invalid override/super, incomplete abstract/interface implementations,
  duplicate members/classes, misplaced declarations, static/instance misuse,
  ambiguous overloads) are structured CompileErrors/RuntimeErrors with
  `file:line:column` — never crashes.

Loop-system semantics in one look:

- `loop <condition> [ ... ]` re-evaluates the condition **before** every
  pass (including the first) and runs the body while it holds true. The
  condition uses the same grammar as an `if` condition and must produce a
  boolean at runtime. `while` is deliberately not a Runa keyword.
- `for i = <start> to <end> [ ... ]` is a counting loop: both bounds are
  **inclusive**, the step is fixed at `+1`, and `start`/`end` are ordinary
  expressions evaluated **exactly once** when the loop is entered (the range
  is fixed at entry — side effects in the bounds happen once).
- `start > end` runs **zero** passes (the bound is checked before the first
  pass) — never an infinite loop. After a completed loop the counter is
  `end + 1`; after a zero-pass loop it keeps `start`.
- The counter is an ordinary variable in the current scope (a local inside
  functions/methods, a global at the top level) — assigning to it inside the
  body is allowed and affects the iteration. The loop's internal end-value
  slot is compiler-internal (`#for_endN#`) and can never collide with a user
  variable; it is hidden from `--dump-globals`.
- `break` exits the **nearest** enclosing `loop`/`for` immediately;
  `continue` ends only the **current** pass of the nearest loop (for `loop`
  it re-checks the condition, for `for` it first advances the counter — so a
  skipped pass can never cause an infinite loop). Nested loops are unaffected.
- `break`/`continue` outside any loop are **compile errors**, and they can
  never cross a function/method boundary. Loops inside functions, methods,
  constructors and recursion work as expected.
- Loops compile to plain `Jump`/`JumpIfFalse` bytecode — no new opcodes, no
  container change; the VM does not know loops exist.

- `printr` prints its arguments joined by **single spaces**, then a newline.
- Strings print **raw** (no quotes) inside `printr`; `--dump-globals` shows
  them quoted.
- Variables are dynamically typed: `x = "a"` then `x = 99` is allowed.
- Reading a variable that was never assigned is a **runtime error**.
- Misspelled function names or wrong argument counts are **compile errors**.
- `fun name(a, b) [ ... ]` declares a function at the top level of the
  program (nested declarations are compile errors). `fun` and `return` are
  reserved words. The body uses the same `[ ... ]` block syntax as `if`.
- **Calls are expressions**: use them in assignments, arithmetic, conditions
  and as bare statements (the value is then discarded). Argument count is
  checked at compile time against the declared parameter count.
- Arguments may be arbitrary expressions and are evaluated before the call;
  each invocation binds parameters to its own independent local scope, so
  recursion is safe.
- Inside a function, assignments create/update **local** variables (a
  same-named global is shadowed, never overwritten); reads check locals
  first, then globals. Locals vanish when the invocation ends.
- `return <expr>` evaluates, returns and stops the function — even from
  inside if/elif/else blocks. A bare `return` and falling off the end of
  the body produce the distinct **`none`** value: not `0`, not `false`,
  not `""`. Functions may be declared before or after their call sites
  (the whole program is compiled before anything runs).
- Function names are reserved: two functions with one name, a function
  named like a built-in, assigning to a function name, or using a function
  as a value are all **compile errors**. Runaway recursion is a clean
  runtime error (`call stack overflow`).
- Arithmetic (`+ - * / %`) works on **numbers only** (integers, 64-bit
  signed): `/` is integer division, `%` is the remainder, and division by
  zero / overflow are **runtime errors** — never crashes or silent wraparound.
- Unary minus (negative literals like `-5`) is **not** supported yet;
  subtraction can still produce negative results (`printr(3 - 10)` → `-7`).
- `if <condition> [ ... ]`: the block runs only when the condition is true.
  Blocks nest naturally. `elif <condition> [ ... ]` and `else [ ... ]` extend
  the chain — they must directly follow the closing `]` on the same line, and
  **at most one branch of a chain executes**. `else` is optional and final;
  `elif` may repeat but never after `else`.
- `loop` / `for` / `break` / `continue` are reserved words, and so is the
  `to` that separates the `for` bounds.
- Comparisons: `==` `!=` work on any two values (different kinds are simply
  unequal); `>` `<` `>=` `<=` work on numbers only. The condition must
  produce a **boolean** at runtime, otherwise it is a runtime error.
- `{}` is deliberately unused — reserved for future set/collection syntax.

### Tool reference

| Tool | Purpose | Flags |
|------|---------|-------|
| `runac` | compiles `.ru` → `.rbc` | `-o <out.rbc>`, `--dump-tokens`, `--dump-ast`, `--dump-bytecode` |
| `runavm` | executes `.rbc` | `--trace`, `--dump-globals` |
| `runa` | compile + run in one step | all of the above |

Exit codes: `0` success · `1` Runa error (lexical/syntax/compile/runtime/bad
bytecode) · `2` usage error or unreadable file.

The dump/trace flags are **development diagnostics** — tooling, not language
syntax. They exist so every layer can be inspected and tested.


## Collection System (1.0)

A collection is Runa's **set-like value**: the order of members carries no
meaning, duplicates are silently collapsed, and every collection holds
exactly one element type — number, float, string, or boolean (the scalar
kinds of the type model). `{}` is the empty collection — a valid value of its
own, distinct from `null` and from `none`.

```runa
numbers = {10, 20, 30, 40}      # braces: the reserved { } finally have a use
names   = {"Runa", "Tluxe", "Lumo"}
flags   = {true, false}
empty   = {}

printr(numbers)                 # {10, 20, 30, 40} — canonical ascending order
printr({1, 2, 2, 3, 3})         # {1, 2, 3}       — duplicates collapse
printr({x, y, x + y})           # elements are full expressions
```

**`{}` vs `[ ]` — the two brace pairs never mix.** Blocks are `[ ... ]`,
`{ ... }` is a collection literal in expression position, and the selective
import list (`as "math" import {sqrt, Vector}`) parses its own braces. There
is no ambiguity: a `{` in expression position is always a collection.

### Collection built-ins

| Call | Result |
|------|--------|
| `sum(c)` / `product(c)` | total / product of a **numeric** collection; empty → `0` / `1` |
| `average(c)` | arithmetic mean; exact integer means stay numbers (`{10,20,30,40}` → `25`), else a float (`{1,2}` → `1.5`); empty → error |
| `min(c)` / `max(c)` | smallest / largest member (numeric collections; empty → error) |
| `count(c)` | number of distinct members |
| `contains(c, x)` | membership; a value of another type is simply not a member |
| `empty(c)` | true exactly when the collection has no members |
| `union(a, b)` / `intersection(a, b)` / `difference(a, b)` | set algebra; both operands must share the element type |
| `sort(c)` / `reverse(c)` | a **visible ordered view** (a string: `10, 20, 30` / `40, 30, 20, 10`) — the collection itself stays unordered |
| `first(c)` / `last(c)` | smallest / largest member under the standard ordering (empty → error) |

```runa
a = {1, 2, 3}
b = {3, 4, 5}
printr(union(a, b))         # {1, 2, 3, 4, 5}
printr(intersection(a, b))  # {3}
printr(difference(a, b))    # {1, 2}
```

### Semantics

- **Equality ignores order**: `{1, 2, 3} == {3, 2, 1}` is true. Different
  element types are never equal.
- **Identity (`===`)** follows the object rule: `b = a` aliases the same
  collection (`a === b`), while two separately written literals with equal
  members are equal but NOT identical.
- **One type per collection**: `{10, "Runa"}` is a CompileError; mixing types
  through variables fails at run time with the same clean message.
- Collections are reference-counted like objects, passed to functions and
  returned from them, assignable/reassignable, and usable as object fields.

## Packages: import ≠ cimport (v0.9)

Runa has **two completely separate import systems**. They never merge:

```text
import            cimport
   ↓                 ↓
Package Resolver     Native package path (package-relative!)
   ↓                 ↓
math.rpr            native/graphics/manifest.json
   ↓                 ↓
RPR Reader          Native API manifest
   ↓                 ↓
init.ru             Native Loader (dlopen / LoadLibrary)
   ↓                 ↓
Runa API            DLL / SO function calls
```

### Normal import — Runa libraries

```runa
import math                      # namespace access
printr(math.sqrt(25))

as "math" import {sqrt, Vector}  # selective access
printr(sqrt(26))
w = new Vector(1, 2)
```

A library is an `.rpr` archive (RPR = Runa Package Archive, a versioned binary
container read directly from memory — the runtime never extracts Runa code):

```text
math.rpr  (built with: rpr pack mathlib math.rpr)
├── manifest.json     { "name": "math", "version": "1.0.0",
│                       "type": "runa", "entry": "init.ru" }
└── init.ru           the public API: top-level functions + classes
```

Search paths, in precedence order (first match wins):

1. `<project>/runalibs/` — next to the program's source/bytecode
2. `<executable dir>/runalibs/internal/`
3. `<executable dir>/runalibs/external/`

The package name in the manifest must match the import name and the file name
(`math.rpr` declares `"name": "math"`). Packages may import other packages
(cycles are CompileErrors). Imported classes fully participate in the language:
`new math.Vector(...)`, methods, statics, `is` checks, inheritance. Selective
imported symbols bind into the unit's scope; conflicts are CompileErrors.

### cimport — native packages

```runa
cimport native/graphics
printr(graphics.create_window(640, 480, "hi"))
```

The cimport path is **package-relative** (`/`-separated identifiers), never an
OS path. Resolution is context-aware:

- in the main program → `<project>/native/graphics/manifest.json`
- inside a library (its `init.ru`) → `native/graphics/...` **inside that
  package's own RPR** — a library carries its own native backend (hybrid)

Native manifest:

```json
{ "name": "graphics", "version": "1.0.0", "type": "native",
  "native": {
    "targets": { "linux-x86_64": "linux-x86_64/libgraphics.so",
                 "windows-x86_64": "windows-x86_64/graphics.dll" },
    "api": { "functions": [
      { "name": "create_window", "symbol": "gfx_create_window",
        "return": "pointer",
        "parameters": ["int32", "int32", "cstring"] } ] } } }
```

The runtime selects the target for the current platform deterministically
(`<os>-<arch>`: linux-x86_64, linux-arm64, windows-x86_64, ...) and loads the
binary lazily on first call. A native binary embedded in an RPR is extracted
once to the native root (dlopen needs a real file; Runa code is never
extracted).

**Security invariants:** `import` never loads a native binary (only `cimport`
does — enforced by tests); malformed archives/manifests fail with clean,
located errors; archive paths cannot escape the package (`..`, absolute paths
and backslashes are rejected).

### Native type mapping (v0.9)

| Manifest type  | Runa argument            | Runa result                       |
|----------------|--------------------------|-----------------------------------|
| int8..int64    | number (range checked)   | number                            |
| uint8..uint32  | number (range checked)   | number                            |
| uint64         | number (0..2^63-1)       | number (bit reinterpreted)        |
| bool           | boolean                  | boolean                           |
| char           | number 0..255            | number 0..255                     |
| pointer        | pointer value or null    | opaque `Pointer#n` / null         |
| cstring        | string (borrowed, UTF-8) or null | string (copied) / null    |
| float32/64     | *not supported as arguments yet* | **float** (new value kind) |
| void           | —                        | none                              |

Not yet supported (clean "not supported" errors, never fake): float
parameters, structs by value, writable buffers, callbacks. Ownership model:
strings crossing the boundary are copied or borrowed for the call duration;
the VM never frees native-owned memory and native code never touches VM
memory.

### Tooling

```bash
rpr pack <directory> <out.rpr>   # build a package (validates the manifest)
rpr list <package.rpr>           # list contents
rpr inspect <package.rpr>        # manifest + metadata
rpr extract <package.rpr> <dir>  # development aid only

nruna init <name>                # scaffold native/<name> (manifest + C template)
nruna build native/<name>        # compile the package for this platform
nruna check native/<name>        # validate manifest + symbols
```

## Example: what the pipeline produces

```text
$ build/runac examples/hello.ru --dump-bytecode
== Runa bytecode (format v3, provisional) ==
constants (2):
  [0] "Runa"
  [1] "Hello"
names (2):
  [0] name
  [1] printr
code (30 bytes):
  0000  LoadConst   0  ; "Runa"
  0005  StoreGlobal 0  ; name
  000a  LoadConst   1  ; "Hello"
  000f  LoadGlobal  0  ; name
  0014  Call        2  ; printr (argc=2)
  001d  Halt
```

```text
$ build/runa examples/hello.ru --trace
[0000] LoadConst   0  ; "Runa"
[0005] StoreGlobal 0  ; name
[000a] LoadConst   1  ; "Hello"
[000f] LoadGlobal  0  ; name
[0014] Call        2  ; printr (argc=2)
[001d] Halt
Hello Runa
```

## Error reporting

All four stages report through one structured format (file, line, column,
kind, message), with source context and a caret under the failing column:

```text
tests/cases/err_no_args.ru:1:1
CompileError: 'printr' expects at least 1 argument(s), got 0
 1 | printr()
  | ^
```

## Bytecode container (PROVISIONAL v5)

The format is intentionally minimal and **not locked** — it is versioned and
extended only as the instruction set grows. Version 1 added typed constants;
version 2 (the function system) added the `functions` table; version 3 (the
class system) added the `classes` table and a `null` constant kind; version 4
(the package system) added the `inits` and `natives` tables. Version 5 (the
collection system) appends exactly one opcode (`BuildCollection`) with the
layout UNCHANGED — the version still bumps so old VMs reject collection
programs with the clear "unsupported bytecode format version" message instead
of an unknown-opcode failure; recompile old `.ru` files.

| Field | Size | Meaning |
|-------|------|-------------|
| magic | 4 B | `'R' 'U' 'N' 'A'` |
| version | u32 LE | currently `5` |
| constants | u32 count + entries | typed pool: `u8 kind` (0 string / 1 number / 2 boolean / 3 null) + payload |
| names | u32 count + entries | name pool: variables + callee/member names (`u32 len + bytes` each) |
| functions | u32 count + entries | per function: name, parameter names, `u32` entry offset into code |
| classes | u32 count + entries | per class: name (qualified for nested), `u32` parent index, flags (abstract/interface), interfaces, fields (flags + name + initializer offset), methods (flags + name + parameters + entry offset) |
| inits | u32 count + entries | per unit (dependency order): package name, init body entry offset, class index range |
| natives | u32 count + entries | per cimport'ed function: namespace, API name, symbol, package dir, platform targets, typed signature |
| code | u32 count + bytes | main program (`Halt`), function bodies, class machinery (field initializers, static initializers, methods, constructors), package init bodies |

Instruction set (v5): `LoadConst <u32>`, `LoadGlobal <u32>`,
`StoreGlobal <u32>`, `Call <u32 name> <u32 argc>`, `Add`, `Sub`, `Mul`,
`Div`, `Mod`, `Halt`, `Eq`, `Ne`, `Gt`, `Lt`, `Ge`, `Le`, `JumpIfFalse <u32>`,
`Jump <u32>`, `ReturnValue`, `ReturnNone`, `Pop`, `LoadVar <u32>`,
`StoreLocal <u32>` — plus the class-system appends: `NewObject <u32 class>
<u32 argc>` (constructs: field initializers base→child, then the
constructor), `GetField <u32>` / `SetField <u32>` / `InitField <u32>`
(instance state), `GetSelf`, `InvokeVirtual <u32 name> <u32 argc>` (runtime
dispatch from the receiver's class), `InvokeStatic <u32 class> <u32 name>
<u32 argc>`, `InvokeSuper <u32 name> <u32 argc>` (dispatch from the parent,
same self), `GetStatic` / `StoreStatic <u32 class> <u32 name>`, `IsType
<u32 class>` (pushes a boolean), `EqIdent` (`===`), the package append
`CallNative <u32 fn> <u32 argc>` and the collection append
`BuildCollection <u32 count>` (pops the elements in source order, pushes the
set value).

Version-0/1/2/3/4 files are rejected with an explicit `unsupported bytecode
format version N` error (the format is provisional; recompile old `.ru`
files). The deserializer validates parent indices, initializer/method entry
offsets and crafted-table inconsistencies before the VM runs a single
instruction.

## Testing

```bash
./build.sh && ./tests/run_tests.sh
```

945 integration checks cover everything from the earlier versions
(compilation, VM execution, dumps, arithmetic, `if/elif/else`, the complete
function system, the class system, corrupt/old-bytecode rejection) plus the
loop system: exact-output programs in five test categories (`tests/loop`,
`tests/for`, `tests/break`, `tests/continue`, `tests/integration`) covering
zero/one/many passes, expression and call conditions, nested loops, `for`
inside `loop` and vice versa, start == end, start > end, bounds evaluated
once, counter-after-loop values, nearest-loop break/continue discipline,
`loop true` + `break`, loop-in-function (the designer's exact examples),
for-in-function, recursion + loops, loops in methods/constructors over object
state, static members, 10000-pass stress runs; every `err_*` negative case
(loop/for malformed forms, break/continue outside loops, inside functions or
methods, with a value, reserved names as counters) failing cleanly with its
exact message; dumps of every new token / AST node / opcode shape; hidden
for-slots kept out of `--dump-globals`; and split-tool parity.

## Design rule

**The language designer decides Runa's syntax.** Undefined features are not
implemented and not invented. When you are ready to grow the language, the
flow is: you pick/confirm the syntax → it gets recorded → then implemented.
See DECISIONS.md for the small provisional choices this toolchain had to make
to stay runnable.
