#include "parser/parser.hpp"

#include <cstdlib>

namespace runa::parser {

Parser::Parser(std::vector<lexer::Token> tokens) : tokens_(std::move(tokens)) {}

const lexer::Token& Parser::peek() const { return tokens_[pos_]; }

const lexer::Token& Parser::peekAt(std::size_t offset) const {
    const std::size_t index = pos_ + offset;
    return (index < tokens_.size()) ? tokens_[index] : tokens_.back();
}

const lexer::Token& Parser::advance() {
    const lexer::Token& token = tokens_[pos_];
    if (pos_ + 1 < tokens_.size()) pos_++;
    return token;
}

bool Parser::check(lexer::TokenType type) const { return peek().type == type; }

bool Parser::match(lexer::TokenType type) {
    if (check(type)) {
        advance();
        return true;
    }
    return false;
}

bool Parser::isAtEnd() const { return check(lexer::TokenType::Eof); }

// ---- recursion guard (DECISIONS #122) -------------------------------------
Parser::RecursionGuard::RecursionGuard(Parser& parser) : parser_(parser) {
    if (parser_.recursionDepth_ >= kMaxRecursionDepth) {
        parser_.recursionDepth_ = 0;  // the parse aborts anyway on this error
        parser_.fail(parser_.peek(),
                     "program nesting is too deep (more than " +
                         std::to_string(kMaxRecursionDepth) +
                         " levels of parentheses, calls, or blocks)");
    }
    previous_ = parser_.recursionDepth_++;
}

Parser::RecursionGuard::~RecursionGuard() {
    parser_.recursionDepth_ = previous_;
}

// ---- header block openers (DECISIONS #122) ---------------------------------
void Parser::expectBlockOpener(const std::string& blockKind) {
    // A '#/' comment at the end of a header line produces no tokens, so the
    // line break arrives as an ordinary Newline token. Skipping newlines here
    // makes comments after 'if' / 'elif' / 'else' / 'loop' / 'for' / 'fun' /
    // 'class' headers (and member signatures) work like in every mainstream
    // language, and additionally allows the opening '[' on the next line.
    while (check(lexer::TokenType::Newline)) {
        advance();
    }
    if (!check(lexer::TokenType::LeftBracket)) {
        fail(peek(), "expected '[' to open the " + blockKind +
                     " block (it may also be on the next line)");
    }
}

// Statement terminators: a newline, or the optional explicit separator ';'
// (spec 1.0 section 2 + DECISIONS #111). Runs of separators behave like
// blank lines; a ';' never introduces an empty statement of its own.
bool Parser::matchStatementEnd() {
    if (check(lexer::TokenType::Newline) || check(lexer::TokenType::Semicolon)) {
        advance();
        return true;
    }
    return false;
}

bool Parser::checkStatementEnd() const {
    return check(lexer::TokenType::Newline) || check(lexer::TokenType::Semicolon);
}

void Parser::fail(const lexer::Token& at, const std::string& message) {
    errors::RunaError error;
    error.kind = errors::ErrorKind::SyntaxError;
    error.location = at.location;
    error.message = message;
    throw errors::RunaErrorException(error);
}

ast::Program Parser::parseProgram() {
    ast::Program program;

    // Leading blank lines are allowed.
    while (matchStatementEnd()) {
    }

    while (!isAtEnd()) {
        program.statements.push_back(parseStatement());

        // A statement must be followed by a newline (or the end of file).
        // Consecutive blank lines are allowed. (Provisional grammar — the
        // statement separator rule is a designer decision, see DECISIONS.md.)
        if (matchStatementEnd()) {
            while (matchStatementEnd()) {
            }
            continue;
        }
        if (isAtEnd()) break;
        fail(peek(), "expected end of line after the statement");
    }
    return program;
}

std::unique_ptr<ast::Statement> Parser::parseStatement() {
    if (check(lexer::TokenType::If)) {
        return parseIfStatement();
    }
    if (check(lexer::TokenType::Fun)) {
        return parseFunctionDeclaration();
    }
    if (check(lexer::TokenType::Return)) {
        return parseReturnStatement();
    }
    // Loop system: 'loop', 'for', 'break' and 'continue' are statements.
    // The loop condition uses the same grammar as an 'if' condition; the for
    // bounds use the general expression grammar. Whether break/continue are
    // enclosed by a loop is validated later by the compiler.
    if (check(lexer::TokenType::Loop)) {
        return parseLoopStatement();
    }
    if (check(lexer::TokenType::For)) {
        return parseForStatement();
    }
    if (check(lexer::TokenType::Break)) {
        return parseBreakStatement();
    }
    if (check(lexer::TokenType::Continue)) {
        return parseContinueStatement();
    }
    // Package system (v0.9): 'import', 'as ".." import {..}' and 'cimport'
    // are statements. The parser accepts them anywhere syntactically; the
    // compiler rejects misplaced ones (top level only) — the same pattern
    // as class declarations.
    if (check(lexer::TokenType::Import)) {
        return parseImportStatement();
    }
    if (check(lexer::TokenType::As)) {
        return parseSelectiveImportStatement();
    }
    if (check(lexer::TokenType::Cimport)) {
        return parseCimportStatement();
    }
    // Class/interface declarations are statements. 'abstract class' and
    // 'abstract interface' are handled here too; the compiler validates where
    // declarations may appear and which modifier combinations make sense.
    if (check(lexer::TokenType::Abstract) || check(lexer::TokenType::Class) ||
        check(lexer::TokenType::Interface)) {
        const bool isAbstract = match(lexer::TokenType::Abstract);
        if (check(lexer::TokenType::Class)) {
            return parseClassDeclaration(isAbstract, /*isInterface=*/false);
        }
        if (check(lexer::TokenType::Interface)) {
            return parseClassDeclaration(isAbstract, /*isInterface=*/true);
        }
        fail(peek(), "expected 'class' or 'interface' after 'abstract'");
    }
    // 'self' can start an expression statement (self.go()) or a member
    // assignment (self.speed = 20). A direct 'self = ...' is reported
    // specifically — self is a reference, not a variable.
    if (check(lexer::TokenType::Self)) {
        const errors::SourceLocation start = peek().location;
        auto postfix = parsePostfix();
        if (check(lexer::TokenType::Equal)) {
            if (postfix->kind == ast::NodeKind::SelfExpr) {
                fail(peek(), "'self' cannot be assigned to (self is a reference "
                             "to the current object, not a variable)");
            }
            return parseAssignmentFromPostfix(std::move(postfix), start);
        }
        return parseStatementAfterPostfix(std::move(postfix), start);
    }
    // 'super' can only start a super method call statement (super.go()).
    if (check(lexer::TokenType::Super)) {
        const errors::SourceLocation start = peek().location;
        auto postfix = parsePostfix();
        if (check(lexer::TokenType::Equal)) {
            fail(peek(), "'super' cannot be assigned to");
        }
        return parseStatementAfterPostfix(std::move(postfix), start);
    }
    // 'new Car().drive()' as a statement: evaluate the creation (and any
    // member chain), then discard the result. A 'new' expression itself is
    // never assignable.
    if (check(lexer::TokenType::New)) {
        const errors::SourceLocation start = peek().location;
        auto postfix = parsePostfix();
        if (check(lexer::TokenType::Equal)) {
            fail(peek(), "cannot assign to a 'new' expression");
        }
        return parseStatementAfterPostfix(std::move(postfix), start);
    }
    // 'elif'/'else' are only valid when the if-chain parser consumes them
    // directly after a closing ']'. Reaching parseStatement with one of them
    // means no 'if' is being continued — report a specific error instead of
    // a generic "expected a statement".
    if (check(lexer::TokenType::Elif)) {
        fail(peek(), "'elif' without a matching 'if' (elif must directly "
                     "follow the closing ']' of an if or elif block)");
    }
    if (check(lexer::TokenType::Else)) {
        fail(peek(), "'else' without a matching 'if' (else must directly "
                     "follow the closing ']' of an if or elif block)");
    }
    if (check(lexer::TokenType::Identifier)) {
        const lexer::TokenType next = peekAt(1).type;
        if (next == lexer::TokenType::Equal) {
            return parseAssignmentStatement();
        }
        if (next == lexer::TokenType::LeftParen || next == lexer::TokenType::Dot) {
            const errors::SourceLocation start = peek().location;
            auto postfix = parsePostfix();
            if (check(lexer::TokenType::Equal)) {
                return parseAssignmentFromPostfix(std::move(postfix), start);
            }
            return parseStatementAfterPostfix(std::move(postfix), start);
        }
        fail(peekAt(1), "expected '=', '(' or '.' after name '" +
                            peek().lexeme + "'");
    }
    fail(peek(), "expected a statement — Runa currently supports assignments "
                 "('name = value'), function calls ('printr(...)'), 'if' blocks, "
                 "'loop' and 'for' loops, 'fun' declarations and 'class' "
                 "declarations");
}

// The postfix expression before '=' is an assignment target (variable or
// member access); anything else is a syntax error.
std::unique_ptr<ast::Statement> Parser::parseAssignmentFromPostfix(
    std::unique_ptr<ast::Expression> target,
    const errors::SourceLocation& startLocation) {
    advance();  // consume '=' (caller checked)
    auto value = parseExpression();
    return std::make_unique<ast::AssignmentStatement>(
        std::move(target), std::move(value), startLocation);
}

std::unique_ptr<ast::Statement> Parser::parseStatementAfterPostfix(
    std::unique_ptr<ast::Expression> expression,
    const errors::SourceLocation& startLocation) {
    return std::make_unique<ast::ExpressionStatement>(std::move(expression),
                                                      startLocation);
}

std::unique_ptr<ast::Statement> Parser::parseIfStatement() {
    RecursionGuard guard(*this);
    const lexer::Token ifToken = advance();  // caller checked: 'if'
    auto statement = std::make_unique<ast::IfStatement>(ifToken.location);

    // --- the required 'if' branch ----------------------------------------
    if (check(lexer::TokenType::LeftBracket)) {
        fail(peek(), "expected a condition after 'if'");
    }
    {
        ast::IfStatement::Branch branch;
        branch.condition = parseCondition();
        expectBlockOpener("if");
        branch.body = parseBlock("if");
        statement->branches.push_back(std::move(branch));
    }

    // --- any number of 'elif' branches ------------------------------------
    while (check(lexer::TokenType::Elif)) {
        advance();  // consume 'elif'
        if (check(lexer::TokenType::LeftBracket)) {
            fail(peek(), "expected a condition after 'elif'");
        }
        ast::IfStatement::Branch branch;
        branch.condition = parseCondition();
        expectBlockOpener("elif");
        branch.body = parseBlock("elif");
        statement->branches.push_back(std::move(branch));
    }

    // --- the optional final 'else' branch ---------------------------------
    if (check(lexer::TokenType::Else)) {
        advance();  // consume 'else'
        expectBlockOpener("else");
        ast::IfStatement::Branch branch;  // condition stays nullptr = 'else'
        branch.body = parseBlock("else");
        statement->branches.push_back(std::move(branch));
    }

    // After 'else' the chain is closed: another 'elif' or 'else' attached to
    // it is invalid and gets a specific error instead of falling through to
    // the generic statement error.
    if (check(lexer::TokenType::Elif)) {
        fail(peek(), "'elif' cannot appear after 'else' (elif branches must "
                     "come before else)");
    }
    if (check(lexer::TokenType::Else)) {
        fail(peek(), "duplicate 'else' — an if/elif chain may have only one "
                     "'else' branch");
    }

    return statement;
}

std::vector<std::unique_ptr<ast::Statement>> Parser::parseBlock(
    const std::string& blockKind) {
    std::vector<std::unique_ptr<ast::Statement>> statements;
    const lexer::Token openToken = advance();  // caller checked: '['

    while (matchStatementEnd()) {
    }

    while (!check(lexer::TokenType::RightBracket) && !isAtEnd()) {
        statements.push_back(parseStatement());

        // Statements inside the block use the same newline-terminated rule as
        // the top level, but the closing ']' may follow on the same line.
        if (matchStatementEnd()) {
            while (matchStatementEnd()) {
            }
            continue;
        }
        if (check(lexer::TokenType::RightBracket)) break;
        fail(peek(), "expected end of line after the statement inside the " +
                         blockKind + " block");
    }

    if (!check(lexer::TokenType::RightBracket)) {
        fail(peek(), "expected ']' to close the " + blockKind +
                         " block opened at line " +
                         std::to_string(openToken.location.line) + ", column " +
                         std::to_string(openToken.location.column));
    }
    advance();  // consume ']'
    return statements;
}

bool Parser::isComparisonOp(lexer::TokenType type) {
    switch (type) {
        case lexer::TokenType::EqualEqual:
        case lexer::TokenType::EqualEqualEqual:
        case lexer::TokenType::BangEqual:
        case lexer::TokenType::Greater:
        case lexer::TokenType::Less:
        case lexer::TokenType::GreaterEqual:
        case lexer::TokenType::LessEqual:
            return true;
        default:
            return false;
    }
}

std::unique_ptr<ast::Expression> Parser::parseCondition() {
    // Comparisons are full expressions since Runa 1.0, so a condition uses
    // exactly the general expression grammar. The run time still demands a
    // boolean — there is no truthiness in Runa.
    return parseExpression();
}

std::unique_ptr<ast::Statement> Parser::parseAssignmentStatement() {
    const lexer::Token& nameToken = advance();  // caller checked: Identifier
    auto target = std::make_unique<ast::Identifier>(nameToken.lexeme, nameToken.location);

    if (!check(lexer::TokenType::Equal)) {
        fail(peek(), "expected '=' after variable name '" + nameToken.lexeme + "'");
    }
    advance();  // consume '='

    auto value = parseExpression();

    return std::make_unique<ast::AssignmentStatement>(
        std::move(target), std::move(value), nameToken.location);
}

std::unique_ptr<ast::Statement> Parser::parseFunctionDeclaration() {
    RecursionGuard guard(*this);
    const lexer::Token funToken = advance();  // caller checked: 'fun'

    if (!check(lexer::TokenType::Identifier)) {
        fail(peek(), "expected a function name after 'fun'");
    }
    const lexer::Token nameToken = advance();
    auto declaration = std::make_unique<ast::FunctionDeclaration>(
        nameToken.lexeme, funToken.location);

    if (!check(lexer::TokenType::LeftParen)) {
        fail(peek(), "expected '(' after the function name '" + nameToken.lexeme + "'");
    }
    advance();  // consume '('

    declaration->parameters = parseParameterList("function '" + nameToken.lexeme + "'");

    expectBlockOpener("body block of function '" + nameToken.lexeme + "'");
    declaration->body = parseBlock("function '" + nameToken.lexeme + "'");
    return declaration;
}

// classDecl := 'abstract'? ( 'class' | 'interface' ) '(' NAME (':' NAME)? ')'
//              ( 'implements' NAME (',' NAME)* )? classBody
std::unique_ptr<ast::Statement> Parser::parseClassDeclaration(bool isAbstract,
                                                              bool isInterface) {
    RecursionGuard guard(*this);
    const lexer::Token keywordToken = advance();  // caller checked: class/interface
    const std::string keyword = isInterface ? "interface" : "class";

    if (!check(lexer::TokenType::LeftParen)) {
        fail(peek(), "expected '(' after '" + keyword + "'");
    }
    advance();  // consume '('

    if (!check(lexer::TokenType::Identifier)) {
        fail(peek(), "expected a " + keyword + " name after '(" + keyword + " '");
    }
    const lexer::Token nameToken = advance();
    auto declaration = std::make_unique<ast::ClassDeclaration>(nameToken.lexeme,
                                                               keywordToken.location);
    declaration->isAbstract = isAbstract;
    declaration->isInterface = isInterface;

    // Inheritance: class (Car : Vehicle). Interfaces cannot inherit.
    if (match(lexer::TokenType::Colon)) {
        if (isInterface) {
            fail(peek(), "an interface cannot inherit (interfaces are declared "
                         "with 'interface (Name)')");
        }
        if (!check(lexer::TokenType::Identifier)) {
            fail(peek(), "expected a parent class name after ':' in '" +
                             nameToken.lexeme + "'");
        }
        const lexer::Token parentToken = advance();
        declaration->parentName = parentToken.lexeme;
        declaration->parentLocation = parentToken.location;
    }

    if (!check(lexer::TokenType::RightParen)) {
        fail(peek(), "expected ':' or ')' in the " + keyword +
                         " header of '" + nameToken.lexeme + "'");
    }
    advance();  // consume ')'

    // Interface implementation list: implements A, B
    if (match(lexer::TokenType::Implements)) {
        if (isInterface) {
            fail(peek(), "an interface cannot implement other interfaces");
        }
        while (true) {
            if (!check(lexer::TokenType::Identifier)) {
                fail(peek(), "expected an interface name after 'implements'");
            }
            const lexer::Token interfaceToken = advance();
            declaration->interfaces.push_back(interfaceToken.lexeme);
            if (match(lexer::TokenType::Comma)) continue;
            break;
        }
    }

    expectBlockOpener("body of " + keyword + " '" + nameToken.lexeme + "'");
    parseClassBody(*declaration);
    return declaration;
}

// classBody := '[' newline* ( member (newline+ member)* )* newline* ']'
// member    := modifier* ( field | method | constructor | nestedClass )
void Parser::parseClassBody(ast::ClassDeclaration& declaration) {
    advance();  // consume '[' (caller checked)

    while (matchStatementEnd()) {
    }

    while (!check(lexer::TokenType::RightBracket) && !isAtEnd()) {
        // ---- modifiers ------------------------------------------------
        bool seenPublic = false, seenPrivate = false, seenStatic = false;
        bool seenAbstract = false, seenOverride = false;
        while (check(lexer::TokenType::Public) || check(lexer::TokenType::Private) ||
               check(lexer::TokenType::Static) || check(lexer::TokenType::Abstract) ||
               check(lexer::TokenType::Override)) {
            const lexer::Token modifierToken = advance();
            bool* flag = nullptr;
            const char* name = "";
            switch (modifierToken.type) {
                case lexer::TokenType::Public:   flag = &seenPublic;   name = "public";   break;
                case lexer::TokenType::Private:  flag = &seenPrivate;  name = "private";  break;
                case lexer::TokenType::Static:   flag = &seenStatic;   name = "static";   break;
                case lexer::TokenType::Abstract: flag = &seenAbstract; name = "abstract"; break;
                case lexer::TokenType::Override: flag = &seenOverride; name = "override"; break;
                default: break;
            }
            if (*flag) {
                fail(modifierToken, std::string("duplicate modifier '") + name + "'");
            }
            *flag = true;
        }
        if (seenPublic && seenPrivate) {
            fail(peek(), "a member cannot be both 'public' and 'private'");
        }

        // ---- the member itself ----------------------------------------
        ast::ClassMember member;
        member.isPublic = seenPublic;
        member.isPrivate = seenPrivate;
        member.isStatic = seenStatic;
        member.isAbstract = seenAbstract;
        member.isOverride = seenOverride;

        if (check(lexer::TokenType::Constructor)) {
            if (member.isPublic || member.isPrivate || member.isStatic ||
                member.isAbstract || member.isOverride) {
                fail(peek(), "a constructor cannot have modifiers");
            }
            const lexer::Token constructorToken = advance();
            member.kind = ast::ClassMember::Kind::Constructor;
            member.nameLocation = constructorToken.location;
            if (!check(lexer::TokenType::LeftParen)) {
                fail(peek(), "expected '(' after 'constructor'");
            }
            advance();  // consume '('
            member.parameters = parseParameterList(
                "constructor of '" + declaration.name + "'");
            expectBlockOpener("body block of the constructor of '" +
                               declaration.name + "'");
            member.body = parseBlock("constructor of '" + declaration.name + "'");
        } else if (check(lexer::TokenType::Fun)) {
            const lexer::Token funToken = advance();
            if (!check(lexer::TokenType::Identifier)) {
                fail(peek(), "expected a method name after 'fun'");
            }
            const lexer::Token nameToken = advance();
            member.kind = ast::ClassMember::Kind::Method;
            member.name = nameToken.lexeme;
            member.nameLocation = nameToken.location;
            if (!check(lexer::TokenType::LeftParen)) {
                fail(peek(), "expected '(' after the method name '" +
                                 nameToken.lexeme + "'");
            }
            advance();  // consume '('
            member.parameters = parseParameterList("method '" + nameToken.lexeme +
                                                   "' of class '" + declaration.name + "'");
            if (member.isAbstract) {
                // Abstract methods have NO body: the declaration ends here.
                if (check(lexer::TokenType::LeftBracket)) {
                    fail(peek(), "an abstract method cannot have a body (method '" +
                                     nameToken.lexeme + "' of class '" +
                                     declaration.name + "')");
                }
            } else if (declaration.isInterface) {
                // Interface methods are contracts: no body allowed.
                if (check(lexer::TokenType::LeftBracket)) {
                    fail(peek(), "interface methods cannot have a body (method '" +
                                     nameToken.lexeme + "' of interface '" +
                                     declaration.name + "')");
                }
            } else {
                expectBlockOpener("body block of method '" + nameToken.lexeme + "'");
                member.body = parseBlock("method '" + nameToken.lexeme + "'");
            }
        } else if (check(lexer::TokenType::Class) || check(lexer::TokenType::Abstract)) {
            // A nested class declaration (possibly 'abstract class (...)').
            if (member.isPublic || member.isPrivate || member.isStatic ||
                member.isOverride) {
                fail(peek(), "a nested class cannot have visibility/static/override "
                             "modifiers");
            }
            const bool nestedAbstract = match(lexer::TokenType::Abstract);
            if (!check(lexer::TokenType::Class)) {
                fail(peek(), "expected 'class' here");
            }
            auto nestedStatement = parseClassDeclaration(nestedAbstract,
                                                         /*isInterface=*/false);
            auto* nestedDeclaration =
                static_cast<ast::ClassDeclaration*>(nestedStatement.release());
            member.kind = ast::ClassMember::Kind::NestedClass;
            member.nested.reset(nestedDeclaration);
        } else if (check(lexer::TokenType::Interface)) {
            fail(peek(), "interfaces cannot be nested inside a class");
        } else if (check(lexer::TokenType::Identifier)) {
            // field := IDENTIFIER '=' expression
            const lexer::Token nameToken = advance();
            member.kind = ast::ClassMember::Kind::Field;
            member.name = nameToken.lexeme;
            member.nameLocation = nameToken.location;
            if (!check(lexer::TokenType::Equal)) {
                fail(peek(), "expected '=' — fields must declare a default value "
                             "(field '" + nameToken.lexeme + "' of class '" +
                                 declaration.name + "')");
            }
            advance();  // consume '='
            member.initializer = parseExpression();
            if (member.isAbstract || member.isOverride) {
                fail(nameToken, std::string("'") +
                                    (member.isAbstract ? "abstract" : "override") +
                                    "' is only valid on methods");
            }
        } else {
            fail(peek(), "expected a field, method, constructor or nested class "
                         "declaration inside class '" + declaration.name + "'");
        }
        declaration.members.push_back(std::move(member));

        // Members use the same newline-terminated rule as top-level
        // statements, but the closing ']' may follow on the same line.
        if (matchStatementEnd()) {
            while (matchStatementEnd()) {
            }
            continue;
        }
        if (check(lexer::TokenType::RightBracket)) break;
        fail(peek(), "expected end of line after the member inside '" +
                         declaration.name + "'");
    }

    if (!check(lexer::TokenType::RightBracket)) {
        fail(peek(), "expected ']' to close the body of '" + declaration.name +
                         "' (opened at line " +
                         std::to_string(declaration.location.line) + ", column " +
                         std::to_string(declaration.location.column) + ")");
    }
    advance();  // consume ']'
}

std::unique_ptr<ast::Statement> Parser::parseImportStatement() {
    const lexer::Token importToken = advance();  // caller checked: 'import'
    if (!check(lexer::TokenType::Identifier)) {
        fail(peek(), "expected a package name after 'import'");
    }
    const lexer::Token nameToken = advance();
    return std::make_unique<ast::ImportStatement>(nameToken.lexeme,
                                                  importToken.location);
}

// selectiveImport := 'as' STRING 'import' '{' IDENTIFIER (',' IDENTIFIER)* '}'
std::unique_ptr<ast::Statement> Parser::parseSelectiveImportStatement() {
    const lexer::Token asToken = advance();  // caller checked: 'as'
    if (!check(lexer::TokenType::String)) {
        fail(peek(), "expected a quoted package name after 'as' "
                     "(as \"math\" import { sqrt })");
    }
    const lexer::Token nameToken = advance();
    if (nameToken.value.empty()) {
        fail(nameToken, "the package name in a selective import must not be empty");
    }
    if (!check(lexer::TokenType::Import)) {
        fail(peek(), "expected 'import' after the package name in a selective "
                     "import (as \"math\" import { .. })");
    }
    advance();  // consume 'import'
    if (!check(lexer::TokenType::LeftBrace)) {
        fail(peek(), "expected '{' to open the selective import list "
                     "(as \"math\" import { sqrt, Vector })");
    }
    advance();  // consume '{'

    auto statement = std::make_unique<ast::SelectiveImportStatement>(
        nameToken.value, asToken.location);
    statement->nameLocation = nameToken.location;

    while (true) {
        if (!check(lexer::TokenType::Identifier)) {
            fail(peek(), "expected a symbol name inside the selective import list");
        }
        const lexer::Token symbolToken = advance();
        for (const auto& existing : statement->symbols) {
            if (existing.name == symbolToken.lexeme) {
                fail(symbolToken, "duplicate symbol '" + symbolToken.lexeme +
                                      "' in the selective import list");
            }
        }
        statement->symbols.push_back({symbolToken.lexeme, symbolToken.location});
        if (match(lexer::TokenType::Comma)) continue;
        break;
    }
    if (!check(lexer::TokenType::RightBrace)) {
        fail(peek(), "expected ',' or '}' in the selective import list");
    }
    advance();  // consume '}'
    return statement;
}

// cimport := 'cimport' IDENTIFIER ('/' IDENTIFIER)*
std::unique_ptr<ast::Statement> Parser::parseCimportStatement() {
    const lexer::Token cimportToken = advance();  // caller checked: 'cimport'
    auto statement =
        std::make_unique<ast::CimportStatement>(cimportToken.location);
    if (!check(lexer::TokenType::Identifier)) {
        fail(peek(), "expected a package-relative path after 'cimport' "
                     "(cimport native/graphics)");
    }
    const lexer::Token firstSegment = advance();
    statement->pathLocation = firstSegment.location;
    statement->pathSegments.push_back(firstSegment.lexeme);
    while (check(lexer::TokenType::Slash)) {
        advance();  // consume '/'
        if (!check(lexer::TokenType::Identifier)) {
            fail(peek(), "expected a path segment after '/' in a cimport path");
        }
        statement->pathSegments.push_back(advance().lexeme);
    }
    return statement;
}

std::unique_ptr<ast::Statement> Parser::parseReturnStatement() {
    const lexer::Token returnToken = advance();  // caller checked: 'return'
    auto statement = std::make_unique<ast::ReturnStatement>(returnToken.location);

    // A bare `return` (no value) is recognized when the next token ends the
    // statement: a newline, the closing ']' of a one-line block, or EOF.
    // Anything else starts the returned expression.
    if (checkStatementEnd() || check(lexer::TokenType::RightBracket) ||
        check(lexer::TokenType::Eof)) {
        return statement;
    }
    statement->value = parseExpression();
    return statement;
}

// loopStmt := 'loop' condition block
std::unique_ptr<ast::Statement> Parser::parseLoopStatement() {
    RecursionGuard guard(*this);
    const lexer::Token loopToken = advance();  // caller checked: 'loop'

    if (check(lexer::TokenType::LeftBracket)) {
        fail(peek(), "expected a condition after 'loop'");
    }
    auto condition = parseCondition();
    expectBlockOpener("loop");

    auto statement = std::make_unique<ast::LoopStatement>(loopToken.location);
    statement->condition = std::move(condition);
    statement->body = parseBlock("loop");
    return statement;
}

// forStmt := 'for' IDENTIFIER '=' expression 'to' expression block
std::unique_ptr<ast::Statement> Parser::parseForStatement() {
    RecursionGuard guard(*this);
    const lexer::Token forToken = advance();  // caller checked: 'for'

    if (!check(lexer::TokenType::Identifier)) {
        fail(peek(), "expected a loop variable name after 'for'");
    }
    const lexer::Token nameToken = advance();
    if (!check(lexer::TokenType::Equal)) {
        fail(peek(), "expected '=' after the loop variable '" + nameToken.lexeme + "'");
    }
    advance();  // consume '='

    if (check(lexer::TokenType::To)) {
        fail(peek(), "expected a start value in the for loop (for " +
                         nameToken.lexeme + " = <start> to <end>)");
    }
    auto start = parseExpression();
    if (!check(lexer::TokenType::To)) {
        fail(peek(), "expected 'to' after the start value of the for loop (for " +
                         nameToken.lexeme + " = <start> to <end>)");
    }
    advance();  // consume 'to'

    if (check(lexer::TokenType::LeftBracket)) {
        fail(peek(), "expected an end value after 'to' in the for loop");
    }
    auto end = parseExpression();
    expectBlockOpener("for loop");

    auto statement = std::make_unique<ast::ForStatement>(forToken.location);
    statement->variableName = nameToken.lexeme;
    statement->variableLocation = nameToken.location;
    statement->start = std::move(start);
    statement->end = std::move(end);
    statement->body = parseBlock("for loop");
    return statement;
}

// breakStmt := 'break'   — takes NO value (designer's strict scope), so
// anything that is not a statement terminator after it is a syntax error.
std::unique_ptr<ast::Statement> Parser::parseBreakStatement() {
    const lexer::Token breakToken = advance();  // caller checked: 'break'
    if (!checkStatementEnd() && !check(lexer::TokenType::RightBracket) &&
        !check(lexer::TokenType::Eof)) {
        fail(peek(), "expected end of line after 'break' ('break' takes no value)");
    }
    return std::make_unique<ast::BreakStatement>(breakToken.location);
}

// continueStmt := 'continue' — bare, like 'break'.
std::unique_ptr<ast::Statement> Parser::parseContinueStatement() {
    const lexer::Token continueToken = advance();  // caller checked: 'continue'
    if (!checkStatementEnd() && !check(lexer::TokenType::RightBracket) &&
        !check(lexer::TokenType::Eof)) {
        fail(peek(), "expected end of line after 'continue' ('continue' takes no value)");
    }
    return std::make_unique<ast::ContinueStatement>(continueToken.location);
}

std::unique_ptr<ast::Expression> Parser::parseCallExpression() {
    const lexer::Token& nameToken = advance();  // caller checked: Identifier
    advance();  // caller checked: '(' — consume it

    auto call = std::make_unique<ast::FunctionCallExpr>(nameToken.lexeme,
                                                        nameToken.location);
    if (!check(lexer::TokenType::RightParen)) {
        call->arguments = parseArgumentList();
    }

    if (!check(lexer::TokenType::RightParen)) {
        fail(peek(), "expected ',' or ')' in the argument list of '" +
                         nameToken.lexeme + "'");
    }
    advance();  // consume ')'
    return call;
}

std::vector<std::unique_ptr<ast::Expression>> Parser::parseArgumentList() {
    std::vector<std::unique_ptr<ast::Expression>> arguments;
    arguments.push_back(parseExpression());
    while (match(lexer::TokenType::Comma)) {
        arguments.push_back(parseExpression());
    }
    return arguments;
}

std::unique_ptr<ast::Expression> Parser::parseExpression() {
    // Every expression passes through here, so this is where the recursion
    // depth guard lives (DECISIONS #125): parenthesized groups, call
    // arguments and collection elements all re-enter parseExpression. The
    // unary levels add their own guards for '-x' / 'not x' chains.
    RecursionGuard guard(*this);
    // Lowest level of the cascade: the 'or' chains. The full ladder is
    // or < and < not < comparison < additive < multiplicative < unary <
    // postfix < primary (DECISIONS #126/#127).
    return parseOr();
}

std::unique_ptr<ast::Expression> Parser::parseOr() {
    auto lhs = parseAnd();
    while (check(lexer::TokenType::Or)) {
        const lexer::Token opToken = advance();
        auto rhs = parseAnd();
        lhs = std::make_unique<ast::BinaryExpr>(ast::BinaryOp::LogicalOr,
                                                std::move(lhs), std::move(rhs),
                                                opToken.location);
    }
    return lhs;
}

std::unique_ptr<ast::Expression> Parser::parseAnd() {
    auto lhs = parseNot();
    while (check(lexer::TokenType::And)) {
        const lexer::Token opToken = advance();
        auto rhs = parseNot();
        lhs = std::make_unique<ast::BinaryExpr>(ast::BinaryOp::LogicalAnd,
                                                std::move(lhs), std::move(rhs),
                                                opToken.location);
    }
    return lhs;
}

std::unique_ptr<ast::Expression> Parser::parseNot() {
    // 'not' is prefix and may repeat (not not x). It binds TIGHTER than
    // and/or but LOOSER than comparison: `not a == b` parses as
    // `not (a == b)` — Python's reading, which is the less surprising one.
    if (check(lexer::TokenType::Not)) {
        RecursionGuard guard(*this);  // 'not not not ...' is unbounded recursion
        const lexer::Token opToken = advance();
        auto operand = parseNot();
        return std::make_unique<ast::UnaryExpr>(ast::UnaryOp::Not,
                                                std::move(operand),
                                                opToken.location);
    }
    return parseComparisonLevel();
}

std::unique_ptr<ast::Expression> Parser::parseComparisonLevel() {
    auto lhs = parseAdditive();

    // Comparisons and type checks are FULL expressions in Runa 1.0
    // (spec sections 11/12/48 + DECISIONS #109/#110):
    // ==  !=  >  <  >=  <=  ===  and  is. Ordering is numeric; equality is
    // strict (enforced at run time).
    if (isComparisonOp(peek().type) || check(lexer::TokenType::Is)) {
        if (check(lexer::TokenType::Is)) {
            // Type check: <value> is ClassName — the name may be dotted for
            // imported classes (x is math.Vector).
            const lexer::Token isToken = advance();
            if (!check(lexer::TokenType::Identifier)) {
                fail(peek(), "expected a class name after 'is'");
            }
            std::string className = advance().lexeme;
            while (check(lexer::TokenType::Dot)) {
                advance();  // consume '.'
                if (!check(lexer::TokenType::Identifier)) {
                    fail(peek(), "expected a class name segment after '.'");
                }
                className += ".";
                className += advance().lexeme;
            }
            lhs = std::make_unique<ast::IsExpr>(std::move(lhs), className,
                                                isToken.location);
        } else {
            const lexer::Token opToken = peek();  // copy: pos_ advances below
            advance();
            auto rhs = parseAdditive();
            lhs = makeCompare(opToken.type, opToken.location, std::move(lhs),
                              std::move(rhs));
        }
        // One comparison per level: `a < b < c` is rejected instead of being
        // silently misinterpreted. Parenthesized comparisons may nest — the
        // inner one belongs to the group's own expression.
        if (isComparisonOp(peek().type) || check(lexer::TokenType::Is)) {
            fail(peek(), "chained comparisons are not supported in Runa yet");
        }
    }
    return lhs;
}

std::unique_ptr<ast::Expression> Parser::parseAdditive() {
    auto lhs = parseMultiplicative();
    while (check(lexer::TokenType::Plus) || check(lexer::TokenType::Minus)) {
        const lexer::Token opToken = peek();  // copy: pos_ advances below
        advance();
        auto rhs = parseMultiplicative();
        lhs = makeBinary(opToken.type, opToken.location, std::move(lhs), std::move(rhs));
    }
    return lhs;
}

std::unique_ptr<ast::Expression> Parser::parseMultiplicative() {
    auto lhs = parseUnary();
    while (check(lexer::TokenType::Star) || check(lexer::TokenType::Slash) ||
           check(lexer::TokenType::Percent)) {
        const lexer::Token opToken = peek();  // copy: pos_ advances below
        advance();
        auto rhs = parseUnary();
        lhs = makeBinary(opToken.type, opToken.location, std::move(lhs), std::move(rhs));
    }
    return lhs;
}

// unary := '-' unary | postfix
// Unary minus (DECISIONS #126): '-5' is an ordinary expression now. It
// binds tighter than '*' '/' '%' (so -2 * 3 reads (-2) * 3) and may repeat
// ('--x' negates twice, like in Python).
std::unique_ptr<ast::Expression> Parser::parseUnary() {
    if (check(lexer::TokenType::Minus)) {
        RecursionGuard guard(*this);  // '-----...' is unbounded recursion
        const lexer::Token opToken = advance();
        auto operand = parseUnary();
        return std::make_unique<ast::UnaryExpr>(ast::UnaryOp::Negate,
                                                std::move(operand),
                                                opToken.location);
    }
    return parsePostfix();
}

// postfix := primary ( '.' IDENTIFIER ('(' argumentList? ')')? )*
// A '.'-chain binds tighter than any arithmetic operator and may mix field
// reads and method calls:  self.engine.start().name
std::unique_ptr<ast::Expression> Parser::parsePostfix() {
    auto expression = parsePrimary();
    while (check(lexer::TokenType::Dot)) {
        advance();  // consume '.'
        if (!check(lexer::TokenType::Identifier)) {
            fail(peek(), "expected a member name after '.'");
        }
        const lexer::Token memberToken = advance();
        if (check(lexer::TokenType::LeftParen)) {
            advance();  // consume '('
            auto call = std::make_unique<ast::MethodCallExpr>(
                std::move(expression), memberToken.lexeme, memberToken.location);
            if (!check(lexer::TokenType::RightParen)) {
                call->arguments = parseArgumentList();
            }
            if (!check(lexer::TokenType::RightParen)) {
                fail(peek(), "expected ',' or ')' in the argument list of method '" +
                                 memberToken.lexeme + "'");
            }
            advance();  // consume ')'
            expression = std::move(call);
        } else {
            expression = std::make_unique<ast::MemberAccessExpr>(
                std::move(expression), memberToken.lexeme, memberToken.location);
        }
    }
    return expression;
}

std::unique_ptr<ast::Expression> Parser::parsePrimary() {
    const lexer::Token& token = peek();
    switch (token.type) {
        case lexer::TokenType::String: {
            advance();
            return std::make_unique<ast::StringLiteral>(token.value, token.location);
        }
        case lexer::TokenType::Number: {
            advance();
            // The lexer guarantees a valid, in-range 64-bit literal, so the
            // conversion cannot fail here.
            return std::make_unique<ast::NumberLiteral>(
                std::strtoll(token.lexeme.c_str(), nullptr, 10), token.location);
        }
        case lexer::TokenType::Float: {
            advance();
            // The lexer validated the literal (digits on both sides of the
            // dot, finite value), so strtod cannot fail here.
            return std::make_unique<ast::FloatLiteral>(
                std::strtod(token.lexeme.c_str(), nullptr), token.location);
        }
        case lexer::TokenType::True: {
            advance();
            return std::make_unique<ast::BoolLiteral>(true, token.location);
        }
        case lexer::TokenType::False: {
            advance();
            return std::make_unique<ast::BoolLiteral>(false, token.location);
        }
        case lexer::TokenType::Null: {
            advance();
            return std::make_unique<ast::NullLiteral>(token.location);
        }
        case lexer::TokenType::Self: {
            advance();
            return std::make_unique<ast::SelfExpr>(token.location);
        }
        case lexer::TokenType::New: {
            const lexer::Token newToken = advance();
            if (!check(lexer::TokenType::Identifier)) {
                fail(peek(), "expected a class name after 'new'");
            }
            // The class name may be dotted when it addresses an imported
            // package member (new graphics.Window) or ::-qualified for a
            // nested class from outside (new Car::Engine). The compiler
            // decides what the path means.
            const lexer::Token nameToken = advance();
            std::string className = nameToken.lexeme;
            while (check(lexer::TokenType::Dot) ||
                   check(lexer::TokenType::ColonColon)) {
                const std::string separator =
                    check(lexer::TokenType::Dot) ? "." : "::";
                advance();  // consume '.' or '::'
                if (!check(lexer::TokenType::Identifier)) {
                    fail(peek(), "expected a class or package member name after '" +
                                     separator + "'");
                }
                className += separator;
                className += advance().lexeme;
            }
            if (!check(lexer::TokenType::LeftParen)) {
                fail(peek(), "expected '(' after the class name in 'new " +
                                 className + "'");
            }
            advance();  // consume '('
            auto creation = std::make_unique<ast::NewExpr>(className,
                                                           newToken.location);
            if (!check(lexer::TokenType::RightParen)) {
                creation->arguments = parseArgumentList();
            }
            if (!check(lexer::TokenType::RightParen)) {
                fail(peek(), "expected ',' or ')' in the argument list of 'new " +
                                 className + "'");
            }
            advance();  // consume ')'
            return creation;
        }
        case lexer::TokenType::Super: {
            // 'super' is only valid for method calls: super.name(args).
            const lexer::Token superToken = advance();
            if (!check(lexer::TokenType::Dot)) {
                fail(peek(), "expected '.name(...)' after 'super' (super is only "
                             "valid for parent method calls)");
            }
            advance();  // consume '.'
            if (!check(lexer::TokenType::Identifier)) {
                fail(peek(), "expected a method name after 'super.'");
            }
            const lexer::Token nameToken = advance();
            if (!check(lexer::TokenType::LeftParen)) {
                fail(peek(), "expected '(' — super is only valid for method calls "
                             "(super." + nameToken.lexeme + "(...))");
            }
            advance();  // consume '('
            auto call = std::make_unique<ast::SuperCallExpr>(nameToken.lexeme,
                                                             superToken.location);
            if (!check(lexer::TokenType::RightParen)) {
                call->arguments = parseArgumentList();
            }
            if (!check(lexer::TokenType::RightParen)) {
                fail(peek(), "expected ',' or ')' in the argument list of 'super." +
                                 nameToken.lexeme + "'");
            }
            advance();  // consume ')'
            return call;
        }
        case lexer::TokenType::Identifier: {
            // IDENTIFIER '(' is a function call — a full expression. A plain
            // identifier remains a variable read. Both continue into the
            // postfix chain (e.g. add(1, 2).field is a parse-level chain; the
            // compiler decides what it means).
            if (peekAt(1).type == lexer::TokenType::LeftParen) {
                return parseCallExpression();
            }
            advance();
            return std::make_unique<ast::Identifier>(token.lexeme, token.location);
        }
        case lexer::TokenType::LeftParen: {
            const errors::SourceLocation openLocation = token.location;
            advance();  // consume '('
            auto inner = parseExpression();
            if (!check(lexer::TokenType::RightParen)) {
                fail(peek(), "expected ')' to close the group opened at line " +
                                 std::to_string(openLocation.line) + ", column " +
                                 std::to_string(openLocation.column));
            }
            advance();  // consume ')'
            return inner;
        }
        case lexer::TokenType::LeftBrace:
            // '{' in expression position is a collection literal (the
            // Collection System). Blocks use '[' in statement position and
            // selective imports parse their own braces, so there is no
            // ambiguity with any other '{' use in the grammar.
            return parseCollectionLiteral();
        case lexer::TokenType::Plus:
            // Unary '+' is still not in the confirmed grammar (unary minus
            // is, DECISIONS #126). Report the limitation explicitly instead
            // of a generic "expected expression".
            fail(token, std::string("unary '+' is not supported in Runa yet") +
                            " (write the number without the leading '+')");
        default:
            fail(token, "expected an expression (a number, a string, "
                        "true/false/null, a variable name, 'self', 'new ...', "
                        "or a '(' group)");
    }
}

// Collection literal (the Collection System): '{' [expression (',' expression)*]
// '}'  —  '{}' is the empty collection. Elements are full expressions
// (variables, arithmetic, calls...). A directly nested collection literal is
// rejected here with a clear message; nesting through variables/other
// expressions is caught at run time by the VM instead (both paths are clean).
std::unique_ptr<ast::Expression> Parser::parseCollectionLiteral() {
    const lexer::Token openToken = advance();  // consume '{'
    auto literal = std::make_unique<ast::CollectionLiteral>(openToken.location);
    if (check(lexer::TokenType::RightBrace)) {
        advance();  // consume '}'
        return literal;
    }
    while (true) {
        auto element = parseExpression();
        if (element->kind == ast::NodeKind::CollectionLiteral) {
            fail(peek(), "nested collections are not supported (collection "
                         "elements must be number, string, or boolean values)");
        }
        literal->elements.push_back(std::move(element));
        if (match(lexer::TokenType::Comma)) continue;
        break;
    }
    if (!check(lexer::TokenType::RightBrace)) {
        fail(peek(), "expected ',' or '}' in the collection literal opened at line " +
                         std::to_string(openToken.location.line) + ", column " +
                         std::to_string(openToken.location.column));
    }
    advance();  // consume '}'
    return literal;
}

std::unique_ptr<ast::Expression> Parser::makeBinary(
    lexer::TokenType opType, const errors::SourceLocation& opLocation,
    std::unique_ptr<ast::Expression> lhs, std::unique_ptr<ast::Expression> rhs) {
    ast::BinaryOp op = ast::BinaryOp::Add;
    switch (opType) {
        case lexer::TokenType::Plus:    op = ast::BinaryOp::Add; break;
        case lexer::TokenType::Minus:   op = ast::BinaryOp::Sub; break;
        case lexer::TokenType::Star:    op = ast::BinaryOp::Mul; break;
        case lexer::TokenType::Slash:   op = ast::BinaryOp::Div; break;
        case lexer::TokenType::Percent: op = ast::BinaryOp::Mod; break;
        default:
            fail(lexer::Token{opType, "", "", opLocation},
                 "internal error: not an arithmetic operator token");
    }
    return std::make_unique<ast::BinaryExpr>(op, std::move(lhs), std::move(rhs),
                                             opLocation);
}

std::unique_ptr<ast::Expression> Parser::makeCompare(
    lexer::TokenType opType, const errors::SourceLocation& opLocation,
    std::unique_ptr<ast::Expression> lhs, std::unique_ptr<ast::Expression> rhs) {
    ast::BinaryOp op = ast::BinaryOp::Eq;
    switch (opType) {
        case lexer::TokenType::EqualEqual:      op = ast::BinaryOp::Eq; break;
        case lexer::TokenType::EqualEqualEqual: op = ast::BinaryOp::IdentEq; break;
        case lexer::TokenType::BangEqual:       op = ast::BinaryOp::Ne; break;
        case lexer::TokenType::Greater:         op = ast::BinaryOp::Gt; break;
        case lexer::TokenType::Less:            op = ast::BinaryOp::Lt; break;
        case lexer::TokenType::GreaterEqual:    op = ast::BinaryOp::Ge; break;
        case lexer::TokenType::LessEqual:       op = ast::BinaryOp::Le; break;
        default:
            fail(lexer::Token{opType, "", "", opLocation},
                 "internal error: not a comparison operator token");
    }
    return std::make_unique<ast::BinaryExpr>(op, std::move(lhs), std::move(rhs),
                                             opLocation);
}

// Parameter list shared by functions, methods and constructors:
// empty or IDENTIFIER (',' IDENTIFIER)*.
std::vector<ast::Parameter> Parser::parseParameterList(const std::string& what) {
    std::vector<ast::Parameter> parameters;
    if (check(lexer::TokenType::RightParen)) {
        advance();  // consume ')'
        return parameters;
    }
    while (true) {
        if (!check(lexer::TokenType::Identifier)) {
            fail(peek(), "expected a parameter name in the parameter list of " + what);
        }
        const lexer::Token parameterToken = advance();
        parameters.push_back({parameterToken.lexeme, parameterToken.location});
        if (match(lexer::TokenType::Comma)) continue;
        break;
    }
    if (!check(lexer::TokenType::RightParen)) {
        fail(peek(), "expected ',' or ')' in the parameter list of " + what);
    }
    advance();  // consume ')'
    return parameters;
}

}  // namespace runa::parser
