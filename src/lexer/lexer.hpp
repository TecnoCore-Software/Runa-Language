// Runa — Lexer.
//
// Converts Runa source text into a stream of tokens. The lexer performs NO
// semantic analysis: it only recognizes the lexical elements that exist in
// Runa syntax so far:
//
//     identifiers (ASCII + Unicode XID_Start/XID_Continue), string literals
//     (single-line with escapes, and raw triple-quoted multiline strings),
//     number literals (integer + decimal float), the keywords
//     'true' / 'false' / 'if' / 'elif' / 'else' / 'fun' / 'return' and the
//     class-system keywords 'class' / 'interface' / 'new' / 'self' / 'super'
//     / 'constructor' / 'static' / 'public' / 'private' / 'abstract'
//     / 'implements' / 'override' / 'null' / 'is', the loop-system
//     keywords 'loop' / 'for' / 'to' / 'break' / 'continue', the package
//     system keywords 'import' / 'cimport' / 'as', the logical-system
//     keywords 'and' / 'or' / 'not',
//     '=', '==', '===', '(', ')', ',', '[', ']', '{', '}', '.', the arithmetic
//     operators '+' '-' '*' '/' '%', the comparison operators '==' '!='
//     '>' '<' '>=' '<=', the optional statement separator ';', newlines,
//     end-of-file. Comments ('#/ ...' and '*/ ... *\') produce NO tokens.
//
// `printr` is NOT a keyword: it is an ordinary identifier that the parser
// recognizes as a function call. Reserved words so far: true, false, if,
// elif, else, fun, return, class, interface, new, self, super, constructor,
// static, public, private, abstract, implements, override, null, is,
// loop, for, to, break, continue, import, cimport, as, and, or, not.
#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

#include "errors/errors.hpp"

namespace runa::lexer {

enum class TokenType {
    Identifier,  // name, user_name, name2, _age, printr, تعداد ...
    String,      // "Runa" (value = processed text between the quotes)
    Float,       // 1.5 (decimal literal, digits on both sides of the dot)
    Number,      // 123 (decimal digits only, 64-bit signed range)
    True,        // true   (reserved word)
    False,       // false  (reserved word)
    Equal,       // =
    LeftParen,   // (
    RightParen,  // )
    Comma,       // ,
    Plus,        // +
    Minus,       // -
    Star,        // *
    Slash,       // /
    Percent,     // %
    LeftBracket,   // [
    RightBracket,  // ]
    EqualEqual,    // ==
    BangEqual,     // !=
    Greater,       // >
    Less,          // <
    GreaterEqual,  // >=
    LessEqual,     // <=
    If,          // 'if'   (reserved word, starts control flow)
    Elif,        // 'elif' (reserved word, continues an if chain)
    Else,        // 'else' (reserved word, ends an if chain)
    Fun,         // 'fun'    (reserved word, starts a function declaration)
    Return,      // 'return' (reserved word, returns from a function)
    // --- class system keywords ---
    Class,       // 'class'      (reserved word, starts a class declaration)
    Interface,   // 'interface'  (reserved word, starts an interface declaration)
    New,         // 'new'        (reserved word, creates an instance)
    Self,        // 'self'       (reserved word, reference to the current object)
    Super,       // 'super'      (reserved word, parent-class method access)
    Constructor, // 'constructor'(reserved word, class constructor declaration)
    Static,      // 'static'     (reserved word, class-level member modifier)
    Public,      // 'public'     (reserved word, visibility modifier)
    Private,     // 'private'    (reserved word, visibility modifier)
    Abstract,    // 'abstract'   (reserved word, abstract class/method modifier)
    Implements,  // 'implements' (reserved word, interface implementation list)
    Override,    // 'override'   (reserved word, method override modifier)
    Null,        // 'null'       (reserved word, the null literal)
    Is,          // 'is'         (reserved word, type-check operator)
    // --- loop system keywords ---
    Loop,        // 'loop'    (reserved word, starts a conditional loop)
    For,         // 'for'     (reserved word, starts a counting loop)
    To,          // 'to'      (reserved word, separates the for loop bounds)
    Break,       // 'break'   (reserved word, exits the innermost loop)
    Continue,    // 'continue'(reserved word, skips to the next iteration)
    // --- package system keywords + tokens (v0.9) ---
    Import,      // 'import'  (reserved word, imports a Runa library package)
    Cimport,     // 'cimport' (reserved word, imports a native package)
    As,          // 'as'      (reserved word, starts a selective import)
    // --- logical system keywords (v1.0+, DECISIONS #127) ---
    And,         // 'and'  (reserved word, logical conjunction, short-circuit)
    Or,          // 'or'   (reserved word, logical disjunction, short-circuit)
    Not,         // 'not'  (reserved word, logical negation)
    LeftBrace,   // {          (selective import list — NOT a block)
    RightBrace,  // }
    Dot,         // .            (member access)
    Colon,           // :            (inheritance in 'class (X : Y)')
    ColonColon,      // ::           (qualified nested class name 'Car::Engine')
    EqualEqualEqual, // ===      (identity comparison)
    Newline,     // end of a line (provisional statement separator)
    Semicolon,   // ;        (optional explicit statement separator)
    Eof,         // end of file (always the last token)
    Unknown,     // reserved for characters outside confirmed syntax
};

struct Token {
    TokenType type;
    std::string lexeme;               // raw source text of the token
    std::string value;                // processed value (strings: inside quotes)
    errors::SourceLocation location;  // start of the token, 1-based
};

const char* tokenTypeName(TokenType type);

class Lexer {
public:
    Lexer(std::string source, std::string filename);

    // Tokenizes the whole source. Always ends with an Eof token.
    // Throws errors::RunaErrorException on the first lexical error.
    std::vector<Token> tokenize();

private:
    std::string source_;
    std::string filename_;
    std::size_t pos_ = 0;
    int line_ = 1;
    int column_ = 1;

    char peek() const;  // current character, or '\0' at end of input
    char peekNext() const;             // next byte, or '\0'
    char peekAt(std::size_t offset) const;  // byte at +offset, or '\0'
    char advance();     // consumes one ASCII character and returns it
    bool atEnd() const;
    errors::SourceLocation here() const;

    // Decodes the UTF-8 sequence starting at the cursor. Returns the code
    // point; sets byteLength (1..4). Invalid sequences yield 0xFFFD with a
    // byteLength of 1 so the caller can fail with a clean message.
    std::uint32_t decodeCodePoint(std::size_t& byteLength) const;
    // Consumes one CHARACTER (1..4 bytes) and advances the character column.
    void advanceChar();
    // Like advanceChar, but appends the character's UTF-8 bytes to `sink`
    // and returns the code point (used by identifier/string scanners).
    std::uint32_t consumeChar(std::string& sink);

    Token makeToken(TokenType type, std::string lexeme, std::string value,
                    const errors::SourceLocation& start);
    Token makeIdentifierOrKeyword(const std::string& lexeme,
                                  const errors::SourceLocation& start);
    [[noreturn]] void fail(const errors::SourceLocation& at, const std::string& message);

    static bool isIdentStart(char c);
    static bool isIdentChar(char c);
};

// Development aid: prints the token stream as a table.
void dumpTokens(const std::vector<Token>& tokens, std::ostream& out);

}  // namespace runa::lexer
