// Runa — Parser.
//
// Consumes the token stream produced by the Lexer, validates grammatical
// structure and produces an AST. Errors carry file, line and column.
//
// Grammar (provisional — statement separator and expression precedence are
// designer decisions, see DECISIONS.md):
//
//     program       := statement* EOF
//     statement     := assignment | expressionStatement | ifStatement
//                    | functionDeclaration | returnStatement | classDeclaration
//     assignment    := (IDENTIFIER | memberAccess) '=' expression
//     exprStatement := expression          (in practice: a bare call)
//     funDecl       := 'fun' IDENTIFIER '(' parameterList? ')' block
//     classDecl     := 'abstract'? ( 'class' | 'interface' ) '(' NAME (':' NAME)? ')'
//                      ( 'implements' NAME (',' NAME)* )? classBody
//     classBody     := '[' member* ']'
//     member        := modifier* ( field | method | constructor | nestedClass )
//     modifier      := 'public' | 'private' | 'static' | 'abstract' | 'override'
//     field         := IDENTIFIER '=' expression
//     method        := 'fun' IDENTIFIER '(' parameterList? ')' (block | nothing-if-abstract)
//     constructor   := 'constructor' '(' parameterList? ')' block
//     nestedClass   := classDecl (inside a class body)
//     parameterList := IDENTIFIER (',' IDENTIFIER)*
//     returnStmt    := 'return' expression?     (value optional)
//     loopStmt      := 'loop' condition block
//     forStmt       := 'for' IDENTIFIER '=' expression 'to' expression block
//     breakStmt     := 'break'     (no value — 'break <expr>' is a syntax error)
//     continueStmt  := 'continue'  (no value)
//     ifStatement   := 'if' condition block
//                      ( 'elif' condition block )* ( 'else' block )?
//     condition     := additive ( compareOp additive | 'is' NAME )?
//     compareOp     := '==' | '===' | '!=' | '>' | '<' | '>=' | '<='
//     block         := '[' newline* (statement (newline+ statement)*)? newline* ']'
//     argumentList  := expression (',' expression)*
//     expression    := additive
//     additive      := multiplicative ( ('+' | '-') multiplicative )*
//     multiplicative:= postfix ( ('*' | '/' | '%') postfix )*
//     postfix       := primary ( '.' IDENTIFIER ('(' argumentList? ')')? )*
//     primary       := NUMBER | STRING | 'true' | 'false' | 'null' | 'self'
//                    | 'new' NAME '(' argumentList? ')'
//                    | 'super' '.' NAME '(' argumentList? ')'
//                    | IDENTIFIER | IDENTIFIER '(' argumentList? ')'   (call)
//                    | '(' expression ')'
//
// Precedence (lowest to highest):
//     or  <  and  <  not  <  == != > < >= <= === is  <  + -  <  * / %  <
//     unary '-'  <  postfix '.'  <  primary
// Operators of the same level are left-associative; 'not' and unary '-' are
// prefix operators and may repeat ('not not x', '--x'). Parentheses override
// precedence. Unary '+' is still not part of the grammar (a leading '+' in
// expression position reports a specific "not supported yet" error).
// (DECISIONS #126: unary minus. DECISIONS #127: and/or/not with short-
// circuit evaluation; 'and' binds tighter than 'or', Python-style.)
//
// Comparison operators exist ONLY in `if`/`elif` conditions in this version:
// a condition is one optional comparison of two arithmetic expressions.
// Chained comparisons (a < b < c) are a syntax error, and comparisons inside
// assignment RHS / call arguments are not part of the grammar yet.
//
// if/elif/else form ONE chain statement: 'elif' continues the chain of the
// immediately preceding 'if', 'else' ends it. Both must DIRECTLY follow the
// closing ']' of the previous branch (same line) — this matches the confirmed
// syntax
//     if ... [ ... ] elif ... [ ... ] else [ ... ]
// so an 'elif'/'else' that starts a new line is reported as a clean syntax
// error ('elif'/'else' without a matching 'if'). 'else' is optional, must be
// the last branch, and cannot be repeated; 'elif' may repeat any number of
// times but never after 'else'. All of these are parser-enforced with
// specific error messages.
//
// A statement starting with IDENTIFIER is an assignment when the next token
// is '=' and an expression statement (a call) when it is '('. The parser does
// not decide what the callee means — that is the compiler's job (user function
// registry lookup first, then the built-in registry). Reserved statement
// keywords: if, elif, else, fun, return, loop, for, break, continue.
//
// Loop system (v0.8): 'loop <condition> [ ... ]' re-evaluates the condition
// before every iteration (the same condition grammar as 'if').
// 'for i = <start> to <end> [ ... ]' is a counting loop with INCLUSIVE
// bounds and a fixed step of +1 — no 'for ... in', no step, no decrement
// (designer's strict scope). 'break'/'continue' are bare statements: they
// take no value, so anything after them that is not a statement terminator
// is a syntax error. Whether a loop actually encloses a break/continue is a
// COMPILER check (CompileError), exactly like 'return' outside a function.
//
// Function calls are full EXPRESSIONS: `add(10, 20)` may appear on the right
// side of an assignment, inside arithmetic (`add(1, 2) * 3`), inside conditions
// and as a bare statement. A call is never a special statement kind.
//
// 'return' is parsed anywhere syntactically; restricting it to function bodies
// (CompileError outside) and rejecting nested function declarations are
// compiler-level checks, per the function-system spec.
//
// Class system (v0.7): 'class'/'interface' declarations are statements. The
// parser accepts them anywhere syntactically; the compiler rejects misplaced
// declarations (they are only valid at the top level or directly inside a
// class body). Member access ('car.name'), method calls ('car.go()'), 'new',
// 'self', 'super.name(...)' and 'null' are part of the expression grammar;
// a '.'-chain binds tighter than any arithmetic operator. Assignment targets
// generalize to member accesses ('self.speed = ...', 'Car.count = 0').
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/ast.hpp"
#include "errors/errors.hpp"
#include "lexer/lexer.hpp"

namespace runa::parser {

class Parser {
public:
    explicit Parser(std::vector<lexer::Token> tokens);

    ast::Program parseProgram();

private:
    std::vector<lexer::Token> tokens_;
    std::size_t pos_ = 0;

    // Recursion guard (DECISIONS #122): parentheses, argument lists, blocks,
    // nested classes and the whole statement/expression cascade recurse
    // through parseExpression/parseStatement. Without a cap, deeply nested
    // source overflows the C++ stack and the toolchain DIES with SIGSEGV
    // instead of reporting a clean SyntaxError. The cap is far above anything
    // a human-written program needs, but low enough to stay inside the
    // smallest supported stack (Windows main thread, 1 MiB).
    static constexpr int kMaxRecursionDepth = 256;
    int recursionDepth_ = 0;

    // RAII helper: increments recursionDepth_ for the enclosing parse call
    // and throws a clean SyntaxError once the nesting cap is exceeded.
    class RecursionGuard {
    public:
        explicit RecursionGuard(Parser& parser);
        ~RecursionGuard();
        RecursionGuard(const RecursionGuard&) = delete;
        RecursionGuard& operator=(const RecursionGuard&) = delete;
    private:
        Parser& parser_;
        int previous_ = 0;
    };

    // Consumes any newline tokens between a header (if/elif/else condition,
    // loop condition, for bounds, fun parameters, class/interface header or
    // a member signature) and its opening '['. A '#/' comment at the end of
    // a header line produces NO tokens, so the header's line break arrives
    // as a plain Newline and must not reject the block opener — in every
    // mainstream language comments are pure whitespace (DECISIONS #122).
    void expectBlockOpener(const std::string& blockKind);

    const lexer::Token& peek() const;
    const lexer::Token& peekAt(std::size_t offset) const;
    const lexer::Token& advance();
    bool check(lexer::TokenType type) const;
    bool match(lexer::TokenType type);
    bool isAtEnd() const;

    std::unique_ptr<ast::Statement> parseStatement();
    std::unique_ptr<ast::Statement> parseAssignmentFromPostfix(
        std::unique_ptr<ast::Expression> target,
        const errors::SourceLocation& startLocation);
    std::unique_ptr<ast::Statement> parseAssignmentStatement();
    std::unique_ptr<ast::Statement> parseIfStatement();
    std::unique_ptr<ast::Statement> parseFunctionDeclaration();
    std::unique_ptr<ast::Statement> parseReturnStatement();
    std::unique_ptr<ast::Statement> parseLoopStatement();
    std::unique_ptr<ast::Statement> parseForStatement();
    std::unique_ptr<ast::Statement> parseBreakStatement();
    std::unique_ptr<ast::Statement> parseContinueStatement();
    // Package system (v0.9).
    std::unique_ptr<ast::Statement> parseImportStatement();
    std::unique_ptr<ast::Statement> parseSelectiveImportStatement();
    std::unique_ptr<ast::Statement> parseCimportStatement();
    std::unique_ptr<ast::Statement> parseClassDeclaration(bool isAbstract,
                                                          bool isInterface);
    std::unique_ptr<ast::Statement> parseStatementAfterPostfix(
        std::unique_ptr<ast::Expression> expression,
        const errors::SourceLocation& startLocation);
    // Parses '[' statements* ']' and reports errors using `blockKind`
    // ("if", "elif" or "else") so messages name the right block.
    std::vector<std::unique_ptr<ast::Statement>> parseBlock(const std::string& blockKind);
    // Parses the member body of a class/interface: '[' member* ']' where a
    // member is a field, a method, a constructor or a nested class.
    void parseClassBody(ast::ClassDeclaration& declaration);
    std::unique_ptr<ast::Expression> parseCondition();

    // Statement terminators: newline or the optional ';' separator
    // (spec 1.0 section 2 + DECISIONS #111).
    bool matchStatementEnd();
    bool checkStatementEnd() const;

    static bool isComparisonOp(lexer::TokenType type);
    std::unique_ptr<ast::Expression> makeCompare(
        lexer::TokenType opType, const errors::SourceLocation& opLocation,
        std::unique_ptr<ast::Expression> lhs, std::unique_ptr<ast::Expression> rhs);

    // Expression cascade: entry point parseExpression, then one level per
    // precedence tier. Arithmetic/comparison levels build left-associative
    // chains of ast::BinaryExpr nodes; the logical levels (and/or) also emit
    // BinaryExpr (LogicalAnd/LogicalOr) and the unary level emits UnaryExpr.
    std::unique_ptr<ast::Expression> parseExpression();
    std::unique_ptr<ast::Expression> parseOr();
    std::unique_ptr<ast::Expression> parseAnd();
    std::unique_ptr<ast::Expression> parseNot();
    std::unique_ptr<ast::Expression> parseComparisonLevel();
    std::unique_ptr<ast::Expression> parseAdditive();
    std::unique_ptr<ast::Expression> parseMultiplicative();
    std::unique_ptr<ast::Expression> parseUnary();
    std::unique_ptr<ast::Expression> parsePostfix();
    std::unique_ptr<ast::Expression> parsePrimary();
    std::unique_ptr<ast::Expression> parseCollectionLiteral();
    std::unique_ptr<ast::Expression> parseCallExpression();
    std::unique_ptr<ast::Expression> makeBinary(
        lexer::TokenType opType, const errors::SourceLocation& opLocation,
        std::unique_ptr<ast::Expression> lhs, std::unique_ptr<ast::Expression> rhs);

    std::vector<std::unique_ptr<ast::Expression>> parseArgumentList();
    std::vector<ast::Parameter> parseParameterList(const std::string& what);

    [[noreturn]] void fail(const lexer::Token& at, const std::string& message);
};

}  // namespace runa::parser
