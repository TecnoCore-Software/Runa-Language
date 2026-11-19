// Runa — Abstract Syntax Tree.
//
// The AST represents the logical structure of Runa source code. Nodes are
// independent from the VM and contain no bytecode (baseline spec, section 10).
// Nodes for the syntax confirmed so far:
//
//     Program
//     ├── AssignmentStatement          <target> = <expression>
//     │   ├── target Identifier | MemberAccess
//     │   └── <expression>
//     ├── ExpressionStatement          <expression>   (a bare call statement)
//     │   └── <expression>
//     ├── IfStatement                  if <condition> [ ... ] elif ... else [ ... ]
//     │   ├── if condition <expression>
//     │   │   └── body <statements>*
//     │   ├── elif condition <expression>   (zero or more)
//     │   │   └── body <statements>*
//     │   └── else                          (optional, must be last)
//     │       └── body <statements>*
//     ├── FunctionDeclaration          fun name(param, ...) [ ... ]
//     │   ├── parameters (names + locations)
//     │   └── body <statements>*
//     ├── ReturnStatement          return <expression>?   (inside functions)
//     │   └── value <expression>?      (absent for a bare `return`)
//     ├── LoopStatement                loop <condition> [ ... ]
//     │   ├── condition <expression>   (re-evaluated before every iteration)
//     │   └── body <statements>*
//     ├── ForStatement                 for <var> = <start> to <end> [ ... ]
//     │   ├── start <expression>       (evaluated once at loop entry)
//     │   ├── end <expression>         (evaluated once at loop entry, inclusive)
//     │   └── body <statements>*
//     ├── BreakStatement               break    (exits the innermost loop)
//     ├── ContinueStatement            continue (skips to the next iteration)
//     ├── ImportStatement              import math            (package system)
//     ├── SelectiveImportStatement     as "math" import {sqrt, Vector}
//     ├── CimportStatement             cimport native/graphics (native bridge)
//     └── ClassDeclaration             class (Name) [ members ]
//         (also represents `interface (Name) [ ... ]` via isInterface —
//          abstract classes carry the isAbstract flag)
//
// Expressions:
//     Identifier | StringLiteral | NumberLiteral | BoolLiteral | NullLiteral
//     FunctionCallExpr  callee(argument, ...)        (a call IS an expression)
//     MethodCallExpr    object.name(argument, ...)   (runtime dispatch)
//     MemberAccessExpr  object.name                  (field / static member)
//     NewExpr           new Name(argument, ...)
//     SelfExpr          self      SuperCallExpr  super.name(argument, ...)
//     IsExpr            <expression> is Name
//     BinaryExpr        <expression> <op> <expression>
//                       (+ - * / % == != > < >= <= === and or)
//     UnaryExpr         -<expression>   not <expression>
#pragma once

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "errors/errors.hpp"

namespace runa::ast {

enum class NodeKind {
    Program,
    AssignmentStatement,
    ExpressionStatement,
    IfStatement,
    FunctionDeclaration,
    ReturnStatement,
    LoopStatement,
    ForStatement,
    BreakStatement,
    ContinueStatement,
    ImportStatement,
    SelectiveImportStatement,
    CimportStatement,
    ClassDeclaration,
    Identifier,
    StringLiteral,
    NumberLiteral,
    FloatLiteral,
    BoolLiteral,
    NullLiteral,
    SelfExpr,
    FunctionCallExpr,
    MethodCallExpr,
    MemberAccessExpr,
    NewExpr,
    SuperCallExpr,
    IsExpr,
    BinaryExpr,
    UnaryExpr,
    CollectionLiteral,
};

// Binary operators. Arithmetic (+ - * / %) was confirmed by the designer;
// the comparison operators (== != > < >= <=) were confirmed for use in
// `if` conditions; IdentEq is the object identity operator '==='.
// LogicalAnd/LogicalOr are the keywords 'and'/'or' (DECISIONS #127) — they
// compile to short-circuit control flow, never to an eager binary opcode.
enum class BinaryOp { Add, Sub, Mul, Div, Mod, Eq, Ne, Gt, Lt, Ge, Le, IdentEq,
                      LogicalAnd, LogicalOr };

// Unary operators (DECISIONS #126/#127): Negate is '-<expr>' (numbers),
// Not is 'not <expr>' (booleans).
enum class UnaryOp { Negate, Not };

// Source symbol of an operator, e.g. "+" — used in dumps and error messages.
const char* binaryOpSymbol(BinaryOp op);
const char* unaryOpSymbol(UnaryOp op);

struct Node {
    NodeKind kind;
    errors::SourceLocation location;

    Node(NodeKind nodeKind, errors::SourceLocation loc)
        : kind(nodeKind), location(std::move(loc)) {}
    virtual ~Node() = default;

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;
    Node(Node&&) = default;
    Node& operator=(Node&&) = default;
};

struct Statement : Node {
    using Node::Node;
};

struct Expression : Node {
    using Node::Node;
};

struct Identifier final : Expression {
    std::string name;

    Identifier(std::string name_, errors::SourceLocation loc)
        : Expression(NodeKind::Identifier, std::move(loc)), name(std::move(name_)) {}
};

struct StringLiteral final : Expression {
    std::string value;

    StringLiteral(std::string value_, errors::SourceLocation loc)
        : Expression(NodeKind::StringLiteral, std::move(loc)), value(std::move(value_)) {}
};

struct NumberLiteral final : Expression {
    std::int64_t value;

    NumberLiteral(std::int64_t value_, errors::SourceLocation loc)
        : Expression(NodeKind::NumberLiteral, std::move(loc)), value(value_) {}
};

// A decimal literal (1.5, 2.0). Floats are official in Runa 1.0 (spec 1.0
// section 5): a distinct runtime type from number, no implicit conversions.
struct FloatLiteral final : Expression {
    double value;

    FloatLiteral(double value_, errors::SourceLocation loc)
        : Expression(NodeKind::FloatLiteral, std::move(loc)), value(value_) {}
};

struct BoolLiteral final : Expression {
    bool value;

    BoolLiteral(bool value_, errors::SourceLocation loc)
        : Expression(NodeKind::BoolLiteral, std::move(loc)), value(value_) {}
};

// The null literal — a valid runtime value distinct from every other kind
// (including the function-level "none").
struct NullLiteral final : Expression {
    explicit NullLiteral(errors::SourceLocation loc)
        : Expression(NodeKind::NullLiteral, std::move(loc)) {}
};

// 'self' — a reference to the object the current method/constructor was
// invoked on. The compiler decides where self is available.
struct SelfExpr final : Expression {
    explicit SelfExpr(errors::SourceLocation loc)
        : Expression(NodeKind::SelfExpr, std::move(loc)) {}
};

// A call expression, e.g.  add(10, 20)  or  printr("Hello", name).
// Calls are full expressions: they may appear on the right side of an
// assignment, inside arithmetic, inside conditions and as bare statements
// (wrapped in an ExpressionStatement). `callee` is a plain name — resolving
// what it refers to (user function vs built-in) is the compiler's job.
struct FunctionCallExpr final : Expression {
    std::string callee;
    std::vector<std::unique_ptr<Expression>> arguments;

    FunctionCallExpr(std::string callee_, errors::SourceLocation loc)
        : Expression(NodeKind::FunctionCallExpr, std::move(loc)),
          callee(std::move(callee_)) {}
};

// A method call on an object, e.g.  car.accelerate(20)  or  self.prepare().
// The receiver is any expression; `Car.getCount()` also parses into this node
// (receiver = Identifier 'Car') and the compiler routes it to a static call.
struct MethodCallExpr final : Expression {
    std::unique_ptr<Expression> object;
    std::string methodName;
    std::vector<std::unique_ptr<Expression>> arguments;

    MethodCallExpr(std::unique_ptr<Expression> object_, std::string methodName_,
                   errors::SourceLocation loc)
        : Expression(NodeKind::MethodCallExpr, std::move(loc)),
          object(std::move(object_)), methodName(std::move(methodName_)) {}
};

// A member access, e.g.  car.name  or  Car.count. Whether it addresses an
// instance field or a class-level (static) member is decided by the compiler
// from the receiver expression.
struct MemberAccessExpr final : Expression {
    std::unique_ptr<Expression> object;
    std::string memberName;

    MemberAccessExpr(std::unique_ptr<Expression> object_, std::string memberName_,
                     errors::SourceLocation loc)
        : Expression(NodeKind::MemberAccessExpr, std::move(loc)),
          object(std::move(object_)), memberName(std::move(memberName_)) {}
};

// Instance creation, e.g.  new Car("BMW", "M3").
struct NewExpr final : Expression {
    std::string className;
    std::vector<std::unique_ptr<Expression>> arguments;

    NewExpr(std::string className_, errors::SourceLocation loc)
        : Expression(NodeKind::NewExpr, std::move(loc)), className(std::move(className_)) {}
};

// A parent-class method call:  super.start(args). 'super' is only valid for
// method calls in Runa — this node exists so anything else after 'super' is
// a clean parse error.
struct SuperCallExpr final : Expression {
    std::string methodName;
    std::vector<std::unique_ptr<Expression>> arguments;

    SuperCallExpr(std::string methodName_, errors::SourceLocation loc)
        : Expression(NodeKind::SuperCallExpr, std::move(loc)),
          methodName(std::move(methodName_)) {}
};

// A type check:  <value> is Car. The class/interface name is resolved by the
// compiler; the runtime check uses the object's actual class.
struct IsExpr final : Expression {
    std::unique_ptr<Expression> value;
    std::string className;

    IsExpr(std::unique_ptr<Expression> value_, std::string className_,
           errors::SourceLocation loc)
        : Expression(NodeKind::IsExpr, std::move(loc)), value(std::move(value_)),
          className(std::move(className_)) {}
};

// A binary expression, e.g.  a + b * 3  or  a == 5.  Operator precedence is
// resolved by the parser (the AST is already the final tree shape); the node
// location is the position of the operator token.
struct BinaryExpr final : Expression {
    BinaryOp op;
    std::unique_ptr<Expression> lhs;
    std::unique_ptr<Expression> rhs;

    BinaryExpr(BinaryOp op_, std::unique_ptr<Expression> lhs_,
               std::unique_ptr<Expression> rhs_, errors::SourceLocation loc)
        : Expression(NodeKind::BinaryExpr, std::move(loc)), op(op_),
          lhs(std::move(lhs_)), rhs(std::move(rhs_)) {}
};

// A unary expression, e.g.  -x  or  not flag. The node location is the
// position of the operator token. 'not' may repeat (not not x) and '-' may
// repeat (--x); both are handled by the parser recursively.
struct UnaryExpr final : Expression {
    UnaryOp op;
    std::unique_ptr<Expression> operand;

    UnaryExpr(UnaryOp op_, std::unique_ptr<Expression> operand_,
              errors::SourceLocation loc)
        : Expression(NodeKind::UnaryExpr, std::move(loc)), op(op_),
          operand(std::move(operand_)) {}
};

// A collection literal, e.g.  {10, 20, 30}  or  {}.  The Collection System
// (v1.0-rc): a set-like value — unordered, duplicate-free, one element type.
// Elements are full expressions; the compiler emits them in source order and
// terminates them with BuildCollection. Nesting a collection literal directly
// inside another is a parse error (not part of this feature).
struct CollectionLiteral final : Expression {
    std::vector<std::unique_ptr<Expression>> elements;

    explicit CollectionLiteral(errors::SourceLocation loc)
        : Expression(NodeKind::CollectionLiteral, std::move(loc)) {}
};

struct AssignmentStatement final : Statement {
    // Assignment target: an Identifier (variable) or a MemberAccessExpr
    // (instance field or class-level static, e.g. self.speed = ...).
    std::unique_ptr<Expression> target;
    std::unique_ptr<Expression> value;  // any expression kind

    AssignmentStatement(std::unique_ptr<Expression> target_,
                        std::unique_ptr<Expression> value_,
                        errors::SourceLocation loc)
        : Statement(NodeKind::AssignmentStatement, std::move(loc)),
          target(std::move(target_)),
          value(std::move(value_)) {}
};

// A bare expression used as a statement — in practice a function call whose
// value nobody consumes, e.g.  printr("Hello")  or  hello().  The compiler
// evaluates the expression and discards the result.
struct ExpressionStatement final : Statement {
    std::unique_ptr<Expression> expression;

    ExpressionStatement(std::unique_ptr<Expression> expression_,
                        errors::SourceLocation loc)
        : Statement(NodeKind::ExpressionStatement, std::move(loc)),
          expression(std::move(expression_)) {}
};

// One declared parameter: its name plus the location of the name token (used
// for precise compiler diagnostics such as duplicate-parameter errors).
struct Parameter {
    std::string name;
    errors::SourceLocation location;
};

// A function declaration:
//
//     fun add(a, b) [
//         return a + b
//     ]
//
// The node location is the 'fun' keyword. The parser accepts declarations
// anywhere syntactically; the compiler rejects nested ones (declarations are
// valid only at the top level of the program for now).
struct FunctionDeclaration final : Statement {
    std::string name;
    std::vector<Parameter> parameters;
    std::vector<std::unique_ptr<Statement>> body;

    FunctionDeclaration(std::string name_, errors::SourceLocation loc)
        : Statement(NodeKind::FunctionDeclaration, std::move(loc)),
          name(std::move(name_)) {}
};

// A return statement. `value` is nullptr for a bare `return` — the function
// then produces the distinct "none" runtime value (never 0, false or "").
struct ReturnStatement final : Statement {
    std::unique_ptr<Expression> value;

    explicit ReturnStatement(errors::SourceLocation loc)
        : Statement(NodeKind::ReturnStatement, std::move(loc)) {}
};

// A conditional loop:
//
//     loop <condition> [
//         statements
//     ]
//
// The condition is evaluated BEFORE every iteration (including the first);
// the body runs while it holds true. The condition uses the ordinary Runa
// expression grammar (the same form as an `if` condition). The node location
// is the 'loop' keyword.
struct LoopStatement final : Statement {
    std::unique_ptr<Expression> condition;
    std::vector<std::unique_ptr<Statement>> body;

    explicit LoopStatement(errors::SourceLocation loc)
        : Statement(NodeKind::LoopStatement, std::move(loc)) {}
};

// A counting loop:
//
//     for <var> = <start> to <end> [
//         statements
//     ]
//
// Both bounds are INCLUSIVE; the counter increases by exactly 1 per
// iteration. `start` and `end` are ordinary expressions, evaluated ONCE when
// the loop is entered (the range is fixed at entry). The node location is
// the 'for' keyword; variableLocation is the counter name token.
struct ForStatement final : Statement {
    std::string variableName;
    errors::SourceLocation variableLocation;
    std::unique_ptr<Expression> start;
    std::unique_ptr<Expression> end;
    std::vector<std::unique_ptr<Statement>> body;

    explicit ForStatement(errors::SourceLocation loc)
        : Statement(NodeKind::ForStatement, std::move(loc)) {}
};

// 'break' — exits the nearest enclosing loop/for immediately; execution
// continues after that loop's body. That a loop actually encloses the
// statement is validated by the compiler (CompileError otherwise).
struct BreakStatement final : Statement {
    explicit BreakStatement(errors::SourceLocation loc)
        : Statement(NodeKind::BreakStatement, std::move(loc)) {}
};

// 'continue' — ends the current iteration of the nearest enclosing loop/for
// and jumps to its next-iteration step (condition re-evaluation for 'loop',
// counter increment for 'for'). Compiler-validated like 'break'.
struct ContinueStatement final : Statement {
    explicit ContinueStatement(errors::SourceLocation loc)
        : Statement(NodeKind::ContinueStatement, std::move(loc)) {}
};

// ---- package system (v0.9) -------------------------------------------------

// `import math` — brings a Runa library package (math.rpr) into the program.
// The compiler resolves the package at compile time; this node emits no code
// itself. Only valid at the top level (compiler-enforced).
struct ImportStatement final : Statement {
    std::string packageName;
    errors::SourceLocation nameLocation;

    ImportStatement(std::string packageName_, errors::SourceLocation loc)
        : Statement(NodeKind::ImportStatement, std::move(loc)),
          packageName(std::move(packageName_)) {}
};

// `as "math" import { sqrt, Vector }` — selective import: only the listed
// top-level symbols of the package become visible in this unit's scope.
// The package name is a string literal by design (the designer's syntax).
struct SelectiveImportStatement final : Statement {
    struct Symbol {
        std::string name;
        errors::SourceLocation location;
    };

    std::string packageName;
    errors::SourceLocation nameLocation;
    std::vector<Symbol> symbols;

    SelectiveImportStatement(std::string packageName_, errors::SourceLocation loc)
        : Statement(NodeKind::SelectiveImportStatement, std::move(loc)),
          packageName(std::move(packageName_)) {}
};

// `cimport native/graphics` — brings a NATIVE package into the program.
// The path is package-relative ('/'-separated identifiers), never an OS
// path. Resolved against the project root in the main unit, or against the
// importing package's own RPR root inside a library. Separate from 'import'
// by design — the two pipelines never merge.
struct CimportStatement final : Statement {
    std::vector<std::string> pathSegments;
    errors::SourceLocation pathLocation;  // first path token

    explicit CimportStatement(errors::SourceLocation loc)
        : Statement(NodeKind::CimportStatement, std::move(loc)) {}

    // "native/graphics"
    std::string joinedPath() const;
};

// Control flow: an if/elif/else chain.
//
//     if <cond1> [ A ] elif <cond2> [ B ] else [ C ]
//
// is ONE IfStatement whose `branches` hold:
//     branch 0: condition = <cond1>, body = A     (the required 'if')
//     branch 1: condition = <cond2>, body = B     (an 'elif')
//     branch 2: condition = nullptr, body = C     (the optional final 'else')
//
// Invariants guaranteed by the parser:
//     - at least one branch (the 'if')
//     - every branch except possibly the last carries a condition
//     - at most one condition-less branch ('else'), and it is the last one
// The location of the node is the 'if' keyword.
struct IfStatement final : Statement {
    struct Branch {
        std::unique_ptr<Expression> condition;  // nullptr for a trailing 'else'
        std::vector<std::unique_ptr<Statement>> body;

        Branch() = default;
        Branch(std::unique_ptr<Expression> condition_,
               std::vector<std::unique_ptr<Statement>> body_)
            : condition(std::move(condition_)), body(std::move(body_)) {}
    };

    std::vector<Branch> branches;

    explicit IfStatement(errors::SourceLocation loc)
        : Statement(NodeKind::IfStatement, std::move(loc)) {}

    bool hasElse() const {
        return !branches.empty() && branches.back().condition == nullptr;
    }
};

// One member of a class body. A member is a field, a method, a constructor
// or a nested class; the modifier flags (public/private/static/abstract/
// override) were parsed from the tokens in front of it.
struct ClassMember {
    enum class Kind { Field, Method, Constructor, NestedClass };

    Kind kind = Kind::Field;
    // Modifiers (their use is validated by the parser/compiler per kind).
    bool isPublic = false;
    bool isPrivate = false;
    bool isStatic = false;
    bool isAbstract = false;
    bool isOverride = false;

    // Field: the declared name + its mandatory default value.
    std::string name;
    errors::SourceLocation nameLocation;
    std::unique_ptr<Expression> initializer;

    // Method / constructor: parameters and body.
    std::vector<Parameter> parameters;
    std::vector<std::unique_ptr<Statement>> body;

    // Nested class.
    std::unique_ptr<struct ClassDeclaration> nested;
};

// A class declaration:
//
//     class (Car : Vehicle) implements Printable [
//         public name = "Unknown"
//         static count = 0
//         constructor(name) [ ... ]
//         fun accelerate(amount) [ ... ]
//         class (Engine) [ ... ]       (nested class)
//     ]
//
// The node also represents an interface declaration (isInterface = true):
//     interface (Printable) [ fun print() ]
// The location is the 'class'/'interface' keyword.
struct ClassDeclaration final : Statement {
    std::string name;
    bool isAbstract = false;
    bool isInterface = false;
    std::string parentName;               // "" = no parent ('class (X : Y)')
    errors::SourceLocation parentLocation; // location of the parent name token
    std::vector<std::string> interfaces;   // implements list
    std::vector<ClassMember> members;

    ClassDeclaration(std::string name_, errors::SourceLocation loc)
        : Statement(NodeKind::ClassDeclaration, std::move(loc)), name(std::move(name_)) {}
};

struct Program : Node {
    std::vector<std::unique_ptr<Statement>> statements;

    Program() : Node(NodeKind::Program, errors::SourceLocation{}) {}
};

// Development aid: prints the tree, e.g.
//   Program
//   ├── AssignmentStatement
//   │   ├── target Identifier 'name'
//   │   └── value   StringLiteral "Runa"
//   ├── FunctionDeclaration 'add'
//   │   ├── params: a, b
//   │   └── body
//   │       └── ReturnStatement
//   │           └── value BinaryExpr (a + b)
//   ├── ClassDeclaration 'Car' (parent: Vehicle)
//   │   ├── field name = "Unknown"
//   │   ├── constructor(params: name, model)
//   │   └── method accelerate(amount)
//   └── ExpressionStatement
//       └── expression FunctionCallExpr add(10, 20)
void dump(const Program& program, std::ostream& out);

}  // namespace runa::ast
