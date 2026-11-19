// Runa — Lexer.
//
// Converts Runa source text into a stream of tokens. The lexer performs NO
// semantic analysis: it only recognizes the lexical elements of Runa 1.0:
//
//     identifiers (ASCII + Unicode XID_Start/XID_Continue), string literals
//     (single-line "..." with the official escapes, and raw triple-quoted
//     """...""" multiline strings), number literals (integers + decimal
//     floats), the keywords 'true' / 'false' / 'if' / 'elif' / 'else' /
//     'fun' / 'return' and the class-system keywords 'class' / 'interface'
//     / 'new' / 'self' / 'super' / 'constructor' / 'static' / 'public' /
//     'private' / 'abstract' / 'implements' / 'override' / 'null' / 'is',
//     the loop-system keywords 'loop' / 'for' / 'to' / 'break' / 'continue',
//     the package-system keywords 'import' / 'cimport' / 'as',
//     '=', '==', '===', '(', ')', ',', '[', ']', '{', '}', '.', the
//     arithmetic operators '+' '-' '*' '/' '%', the comparison operators
//     '==' '!=' '>' '<' '>=' '<=', the optional statement separator ';',
//     newlines, end-of-file.
//
// Comments are recognized here and produce NO tokens:
//     #/ ...          single-line comment (to end of line)
//     */ ... *\      multi-line comment (closed by the FIRST *\, no nesting)
// Inside a string literal, comment markers are ordinary characters.
//
// Columns are CHARACTER based (user-visible positions), not byte offsets.
//
// `printr` is NOT a keyword: it is an ordinary identifier that the parser
// recognizes as a function call. Reserved words: true, false, if, elif,
// else, fun, return, class, interface, new, self, super, constructor,
// static, public, private, abstract, implements, override, null, is,
// loop, for, to, break, continue, import, cimport, as.
#include "lexer/lexer.hpp"

#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>

#include "lexer/unicode_tables.hpp"

namespace runa::lexer {

const char* tokenTypeName(TokenType type) {
    switch (type) {
        case TokenType::Identifier: return "Identifier";
        case TokenType::String:     return "String";
        case TokenType::Float:      return "Float";
        case TokenType::Number:     return "Number";
        case TokenType::True:       return "True";
        case TokenType::False:      return "False";
        case TokenType::Equal:      return "Equal";
        case TokenType::LeftParen:  return "LeftParen";
        case TokenType::RightParen: return "RightParen";
        case TokenType::Comma:      return "Comma";
        case TokenType::Plus:       return "Plus";
        case TokenType::Minus:      return "Minus";
        case TokenType::Star:       return "Star";
        case TokenType::Slash:      return "Slash";
        case TokenType::Percent:    return "Percent";
        case TokenType::LeftBracket:  return "LeftBracket";
        case TokenType::RightBracket: return "RightBracket";
        case TokenType::EqualEqual:   return "EqualEqual";
        case TokenType::BangEqual:    return "BangEqual";
        case TokenType::Greater:      return "Greater";
        case TokenType::Less:         return "Less";
        case TokenType::GreaterEqual: return "GreaterEqual";
        case TokenType::LessEqual:    return "LessEqual";
        case TokenType::If:         return "If";
        case TokenType::Elif:       return "Elif";
        case TokenType::Else:       return "Else";
        case TokenType::Fun:        return "Fun";
        case TokenType::Return:     return "Return";
        case TokenType::Class:      return "Class";
        case TokenType::Interface:  return "Interface";
        case TokenType::New:        return "New";
        case TokenType::Self:       return "Self";
        case TokenType::Super:      return "Super";
        case TokenType::Constructor: return "Constructor";
        case TokenType::Static:     return "Static";
        case TokenType::Public:     return "Public";
        case TokenType::Private:    return "Private";
        case TokenType::Abstract:   return "Abstract";
        case TokenType::Implements: return "Implements";
        case TokenType::Override:   return "Override";
        case TokenType::Null:       return "Null";
        case TokenType::Is:         return "Is";
        case TokenType::Loop:       return "Loop";
        case TokenType::For:        return "For";
        case TokenType::To:         return "To";
        case TokenType::Break:      return "Break";
        case TokenType::Continue:   return "Continue";
        case TokenType::Import:     return "Import";
        case TokenType::Cimport:    return "Cimport";
        case TokenType::As:         return "As";
        case TokenType::And:        return "And";
        case TokenType::Or:         return "Or";
        case TokenType::Not:        return "Not";
        case TokenType::LeftBrace:  return "LeftBrace";
        case TokenType::RightBrace: return "RightBrace";
        case TokenType::Dot:            return "Dot";
        case TokenType::Colon:          return "Colon";
        case TokenType::ColonColon:     return "ColonColon";
        case TokenType::EqualEqualEqual: return "EqualEqualEqual";
        case TokenType::Newline:    return "Newline";
        case TokenType::Semicolon:  return "Semicolon";
        case TokenType::Eof:        return "Eof";
        case TokenType::Unknown:    return "Unknown";
    }
    return "Token";
}

Lexer::Lexer(std::string source, std::string filename)
    : source_(std::move(source)), filename_(std::move(filename)) {}

bool Lexer::isIdentStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool Lexer::isIdentChar(char c) {
    return isIdentStart(c) || (c >= '0' && c <= '9');
}

char Lexer::peek() const {
    return atEnd() ? '\0' : source_[pos_];
}

char Lexer::peekNext() const {
    return (pos_ + 1 >= source_.size()) ? '\0' : source_[pos_ + 1];
}

char Lexer::peekAt(std::size_t offset) const {
    return (pos_ + offset >= source_.size()) ? '\0' : source_[pos_ + offset];
}

char Lexer::advance() {
    char c = source_[pos_];
    pos_++;
    column_++;
    return c;
}

std::uint32_t Lexer::decodeCodePoint(std::size_t& byteLength) const {
    unsigned char b0 = static_cast<unsigned char>(source_[pos_]);
    if (b0 < 0x80) {
        byteLength = 1;
        return b0;
    }
    std::size_t length = 0;
    std::uint32_t cp = 0;
    if ((b0 & 0xE0) == 0xC0) { length = 2; cp = b0 & 0x1F; }
    else if ((b0 & 0xF0) == 0xE0) { length = 3; cp = b0 & 0x0F; }
    else if ((b0 & 0xF8) == 0xF0) { length = 4; cp = b0 & 0x07; }
    else {
        byteLength = 1;
        return 0xFFFD;  // stray continuation byte or invalid lead byte
    }
    if (pos_ + length > source_.size()) {
        byteLength = 1;
        return 0xFFFD;  // truncated sequence at end of input
    }
    for (std::size_t i = 1; i < length; ++i) {
        unsigned char bi = static_cast<unsigned char>(source_[pos_ + i]);
        if ((bi & 0xC0) != 0x80) {
            byteLength = 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (bi & 0x3F);
    }
    // Reject overlong encodings, surrogates and out-of-range code points.
    if ((length == 2 && cp < 0x80) || (length == 3 && cp < 0x800) ||
        (length == 4 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
        byteLength = 1;
        return 0xFFFD;
    }
    byteLength = length;
    return cp;
}

void Lexer::advanceChar() {
    std::size_t byteLength = 0;
    decodeCodePoint(byteLength);
    pos_ += byteLength;
    column_ += 1;
}

std::uint32_t Lexer::consumeChar(std::string& sink) {
    std::size_t byteLength = 0;
    std::uint32_t cp = decodeCodePoint(byteLength);
    for (std::size_t i = 0; i < byteLength; ++i) {
        sink += advance();  // keeps column_ in sync per consumed character
    }
    // advance() counted one column per BYTE; a character counts as one.
    column_ -= static_cast<int>(byteLength) - 1;
    return cp;
}

bool Lexer::atEnd() const { return pos_ >= source_.size(); }

errors::SourceLocation Lexer::here() const {
    errors::SourceLocation loc;
    loc.filename = filename_;
    loc.line = line_;
    loc.column = column_;
    return loc;
}

Token Lexer::makeToken(TokenType type, std::string lexeme, std::string value,
                       const errors::SourceLocation& start) {
    Token token;
    token.type = type;
    token.lexeme = std::move(lexeme);
    token.value = std::move(value);
    token.location = start;
    return token;
}

Token Lexer::makeIdentifierOrKeyword(const std::string& lexeme,
                                     const errors::SourceLocation& start) {
    if (lexeme == "true") return makeToken(TokenType::True, lexeme, lexeme, start);
    if (lexeme == "false") return makeToken(TokenType::False, lexeme, lexeme, start);
    if (lexeme == "if") return makeToken(TokenType::If, lexeme, lexeme, start);
    if (lexeme == "elif") return makeToken(TokenType::Elif, lexeme, lexeme, start);
    if (lexeme == "else") return makeToken(TokenType::Else, lexeme, lexeme, start);
    if (lexeme == "fun") return makeToken(TokenType::Fun, lexeme, lexeme, start);
    if (lexeme == "return") return makeToken(TokenType::Return, lexeme, lexeme, start);
    if (lexeme == "class") return makeToken(TokenType::Class, lexeme, lexeme, start);
    if (lexeme == "interface") return makeToken(TokenType::Interface, lexeme, lexeme, start);
    if (lexeme == "new") return makeToken(TokenType::New, lexeme, lexeme, start);
    if (lexeme == "self") return makeToken(TokenType::Self, lexeme, lexeme, start);
    if (lexeme == "super") return makeToken(TokenType::Super, lexeme, lexeme, start);
    if (lexeme == "constructor") return makeToken(TokenType::Constructor, lexeme, lexeme, start);
    if (lexeme == "static") return makeToken(TokenType::Static, lexeme, lexeme, start);
    if (lexeme == "public") return makeToken(TokenType::Public, lexeme, lexeme, start);
    if (lexeme == "private") return makeToken(TokenType::Private, lexeme, lexeme, start);
    if (lexeme == "abstract") return makeToken(TokenType::Abstract, lexeme, lexeme, start);
    if (lexeme == "implements") return makeToken(TokenType::Implements, lexeme, lexeme, start);
    if (lexeme == "override") return makeToken(TokenType::Override, lexeme, lexeme, start);
    if (lexeme == "null") return makeToken(TokenType::Null, lexeme, lexeme, start);
    if (lexeme == "is") return makeToken(TokenType::Is, lexeme, lexeme, start);
    if (lexeme == "loop") return makeToken(TokenType::Loop, lexeme, lexeme, start);
    if (lexeme == "for") return makeToken(TokenType::For, lexeme, lexeme, start);
    if (lexeme == "to") return makeToken(TokenType::To, lexeme, lexeme, start);
    if (lexeme == "break") return makeToken(TokenType::Break, lexeme, lexeme, start);
    if (lexeme == "continue") return makeToken(TokenType::Continue, lexeme, lexeme, start);
    if (lexeme == "import") return makeToken(TokenType::Import, lexeme, lexeme, start);
    if (lexeme == "cimport") return makeToken(TokenType::Cimport, lexeme, lexeme, start);
    if (lexeme == "as") return makeToken(TokenType::As, lexeme, lexeme, start);
    // Logical system keywords (DECISIONS #127): 'and' / 'or' / 'not' become
    // reserved words so a compound condition reads as one token stream and a
    // variable accidentally named 'and' fails loudly instead of silently
    // changing the meaning of an expression.
    if (lexeme == "and") return makeToken(TokenType::And, lexeme, lexeme, start);
    if (lexeme == "or") return makeToken(TokenType::Or, lexeme, lexeme, start);
    if (lexeme == "not") return makeToken(TokenType::Not, lexeme, lexeme, start);
    return makeToken(TokenType::Identifier, lexeme, lexeme, start);
}

void Lexer::fail(const errors::SourceLocation& at, const std::string& message) {
    errors::RunaError error;
    error.kind = errors::ErrorKind::LexicalError;
    error.location = at;
    error.message = message;
    throw errors::RunaErrorException(error);
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;

    // Skip a UTF-8 byte order mark (EF BB BF) if present (DECISIONS #122).
    // Editors and tools on Windows (Notepad, PowerShell redirection, some
    // IDE defaults) still emit BOMs; every mainstream toolchain ignores it
    // silently instead of failing on the first character of the file.
    if (source_.size() >= 3 &&
        static_cast<unsigned char>(source_[0]) == 0xEF &&
        static_cast<unsigned char>(source_[1]) == 0xBB &&
        static_cast<unsigned char>(source_[2]) == 0xBF) {
        pos_ += 3;  // invisible to the user: column 1 stays at the first real character
    }

    while (!atEnd()) {
        char c = peek();

        // Horizontal whitespace is insignificant.
        if (c == ' ' || c == '\t') {
            advance();
            continue;
        }

        // '\r' is part of a line ending, not ordinary whitespace. CRLF (the
        // Windows default) produces exactly ONE Newline token; a lone '\r'
        // (the classic Mac line ending, still emitted by some tools) is a
        // line terminator in its own right, like in every mainstream lexer
        // (DECISIONS #122).
        if (c == '\r') {
            if (peekNext() == '\n') {
                advance();  // skip the '\r' — the '\n' below emits the Newline
                continue;
            }
            errors::SourceLocation start = here();
            advance();
            line_++;
            column_ = 1;
            tokens.push_back(makeToken(TokenType::Newline, "\r", "\r", start));
            continue;
        }

        if (c == '\n') {
            errors::SourceLocation start = here();
            advance();
            line_++;
            column_ = 1;
            tokens.push_back(makeToken(TokenType::Newline, "\n", "\n", start));
            continue;
        }

        // ---- comments (spec 1.0 section 3) — produce NO tokens ------------
        // '#/' starts a single-line comment running to the end of the line.
        // The terminating newline itself is left for the main loop so the
        // statement separator behavior stays identical.
        if (c == '#' && peekNext() == '/') {
            advance();  // '#'
            advance();  // '/'
            while (!atEnd() && peek() != '\n') advance();
            continue;
        }

        // A lone '#' is still outside the language.
        if (c == '#') {
            errors::SourceLocation start = here();
            advance();
            fail(start, "unexpected character '#' (single-line comments start with '#/')");
        }

        // '*/' opens a multi-line comment closed by the FIRST '*\'. Nesting
        // is not allowed and '*/' inside does not reopen (spec section 3).
        if (c == '*' && peekNext() == '/') {
            errors::SourceLocation start = here();
            advance();  // '*'
            advance();  // '/'
            while (true) {
                if (atEnd()) {
                    fail(start, "unterminated multi-line comment (expected '*\\' to close '*/')");
                }
                if (peek() == '\n') {
                    advance();
                    line_++;
                    column_ = 1;
                    continue;
                }
                if (peek() == '*' && peekNext() == '\\') {
                    advance();  // '*'
                    advance();  // '\'
                    break;
                }
                advance();
            }
            continue;
        }

        // ---- statement separator: ';' (optional, spec 1.0 section 2) -----
        if (c == ';') {
            errors::SourceLocation start = here();
            advance();
            tokens.push_back(makeToken(TokenType::Semicolon, ";", ";", start));
            continue;
        }

        // '=' grows into '==' and '==='; a lone '!' is only valid as the
        // start of '!='.
        if (c == '=') {
            errors::SourceLocation start = here();
            advance();
            if (peek() == '=') {
                advance();
                if (peek() == '=') {
                    advance();
                    tokens.push_back(makeToken(TokenType::EqualEqualEqual, "===", "===", start));
                    continue;
                }
                tokens.push_back(makeToken(TokenType::EqualEqual, "==", "==", start));
                continue;
            }
            tokens.push_back(makeToken(TokenType::Equal, "=", "=", start));
            continue;
        }

        if (c == '!') {
            errors::SourceLocation start = here();
            advance();
            if (peek() == '=') {
                advance();
                tokens.push_back(makeToken(TokenType::BangEqual, "!=", "!=", start));
                continue;
            }
            fail(start, "unexpected character '!' (only '!=' is supported)");
        }

        if (c == '>') {
            errors::SourceLocation start = here();
            advance();
            if (peek() == '=') {
                advance();
                tokens.push_back(makeToken(TokenType::GreaterEqual, ">=", ">=", start));
                continue;
            }
            tokens.push_back(makeToken(TokenType::Greater, ">", ">", start));
            continue;
        }

        if (c == '<') {
            errors::SourceLocation start = here();
            advance();
            if (peek() == '=') {
                advance();
                tokens.push_back(makeToken(TokenType::LessEqual, "<=", "<=", start));
                continue;
            }
            tokens.push_back(makeToken(TokenType::Less, "<", "<", start));
            continue;
        }

        if (c == '[') {
            errors::SourceLocation start = here();
            advance();
            tokens.push_back(makeToken(TokenType::LeftBracket, "[", "[", start));
            continue;
        }

        if (c == ']') {
            errors::SourceLocation start = here();
            advance();
            tokens.push_back(makeToken(TokenType::RightBracket, "]", "]", start));
            continue;
        }

        if (c == '(') {
            errors::SourceLocation start = here();
            advance();
            tokens.push_back(makeToken(TokenType::LeftParen, "(", "(", start));
            continue;
        }

        if (c == ')') {
            errors::SourceLocation start = here();
            advance();
            tokens.push_back(makeToken(TokenType::RightParen, ")", ")", start));
            continue;
        }

        if (c == ',') {
            errors::SourceLocation start = here();
            advance();
            tokens.push_back(makeToken(TokenType::Comma, ",", ",", start));
            continue;
        }

        // '.' opens member access (car.name). A '.' directly after digits is
        // handled inside the number scanner (float literals).
        if (c == '.') {
            errors::SourceLocation start = here();
            advance();
            tokens.push_back(makeToken(TokenType::Dot, ".", ".", start));
            continue;
        }

        // ':' separates a class from its parent (class (Car : Vehicle));
        // '::' qualifies a nested class (new Car::Engine()).
        if (c == ':') {
            errors::SourceLocation start = here();
            advance();
            if (peek() == ':') {
                advance();
                tokens.push_back(makeToken(TokenType::ColonColon, "::", "::", start));
                continue;
            }
            tokens.push_back(makeToken(TokenType::Colon, ":", ":", start));
            continue;
        }

        // '{' and '}' delimit collection literals in expression position and
        // the selective import list. They are still NOT block delimiters —
        // blocks use '[' ']' (designer's rule, unchanged).
        if (c == '{') {
            errors::SourceLocation start = here();
            advance();
            tokens.push_back(makeToken(TokenType::LeftBrace, "{", "{", start));
            continue;
        }

        if (c == '}') {
            errors::SourceLocation start = here();
            advance();
            tokens.push_back(makeToken(TokenType::RightBrace, "}", "}", start));
            continue;
        }

        // Arithmetic operator tokens are single characters with no lookahead.
        // '*/' adjacency was handled above as the multi-line comment opener.
        if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%') {
            errors::SourceLocation start = here();
            std::string lexeme(1, advance());
            TokenType type = TokenType::Unknown;
            switch (lexeme[0]) {
                case '+': type = TokenType::Plus;    break;
                case '-': type = TokenType::Minus;   break;
                case '*': type = TokenType::Star;    break;
                case '/': type = TokenType::Slash;   break;
                case '%': type = TokenType::Percent; break;
            }
            tokens.push_back(makeToken(type, lexeme, lexeme, start));
            continue;
        }

        // ---- identifiers: ASCII fast path + Unicode XID -------------------
        if (isIdentStart(c)) {
            errors::SourceLocation start = here();
            std::string lexeme;
            while (!atEnd()) {
                unsigned char ch = static_cast<unsigned char>(peek());
                if (ch < 0x80) {
                    if (isIdentChar(static_cast<char>(ch))) {
                        lexeme += advance();
                        continue;
                    }
                    break;
                }
                std::string sink;
                std::uint32_t cp = consumeChar(sink);
                if (cp == 0xFFFD || !unicode::isXidContinue(cp)) {
                    // Not part of the identifier — put the character back.
                    pos_ -= sink.size();
                    column_ -= 1;
                    break;
                }
                lexeme += sink;
            }
            tokens.push_back(makeIdentifierOrKeyword(lexeme, start));
            continue;
        }

        // Non-ASCII: either a Unicode identifier start or an error.
        if (static_cast<unsigned char>(c) >= 0x80) {
            errors::SourceLocation start = here();
            std::size_t byteLength = 0;
            std::uint32_t cp = decodeCodePoint(byteLength);
            if (cp == 0xFFFD) {
                fail(start, "invalid UTF-8 byte sequence");
            }
            if (unicode::isXidStart(cp)) {
                std::string lexeme;
                for (std::size_t i = 0; i < byteLength; ++i) lexeme += advance();
                // column_ counted one column per byte; restore character basis.
                column_ -= static_cast<int>(byteLength) - 1;
                while (!atEnd()) {
                    unsigned char ch = static_cast<unsigned char>(peek());
                    if (ch < 0x80) {
                        if (isIdentChar(static_cast<char>(ch))) {
                            lexeme += advance();
                            continue;
                        }
                        break;
                    }
                    std::string sink;
                    std::uint32_t next = consumeChar(sink);
                    if (next == 0xFFFD || !unicode::isXidContinue(next)) {
                        pos_ -= sink.size();
                        column_ -= 1;
                        break;
                    }
                    lexeme += sink;
                }
                tokens.push_back(makeIdentifierOrKeyword(lexeme, start));
                continue;
            }
            std::string character(source_, pos_, byteLength);
            advance();
            column_ -= static_cast<int>(byteLength) - 1;
            fail(start, "unexpected character '" + character + "'");
        }

        // ---- numbers: integers and decimal floats -------------------------
        if (c >= '0' && c <= '9') {
            errors::SourceLocation start = here();
            std::string digits;
            while (!atEnd() && peek() >= '0' && peek() <= '9') digits += advance();

            // A '.' right after the digits either starts a float literal
            // (digits follow) or is a malformed decimal.
            if (peek() == '.') {
                if (peekNext() >= '0' && peekNext() <= '9') {
                    std::string lexeme = digits;
                    lexeme += advance();  // the '.'
                    while (!atEnd() && peek() >= '0' && peek() <= '9') {
                        lexeme += advance();
                    }
                    // Deterministic range check: digits-only literals cannot
                    // be NaN, but hundreds of digits overflow to infinity.
                    errno = 0;
                    char* end = nullptr;
                    const double parsed = std::strtod(lexeme.c_str(), &end);
                    if (end != lexeme.c_str() + lexeme.size() ||
                        !std::isfinite(parsed)) {
                        fail(start, "float literal out of range: '" + lexeme + "'");
                    }
                    tokens.push_back(makeToken(TokenType::Float, lexeme, lexeme, start));
                    continue;
                }
                errors::SourceLocation dotStart = here();
                advance();  // consume the '.' so the caret points past it
                fail(dotStart, "malformed number literal '" + digits +
                                   ".' (digits are required after the decimal point)");
            }

            // Fail-fast range check: 64-bit signed limit (9223372036854775807).
            // The (max - digit) / 10 guard avoids unsigned overflow entirely.
            constexpr unsigned long long kMax = 9223372036854775807ULL;
            unsigned long long acc = 0;
            for (char d : digits) {
                unsigned int digit = static_cast<unsigned int>(d - '0');
                if (acc > (kMax - digit) / 10) {
                    fail(start, "integer literal out of range (64-bit signed limit): '" +
                                    digits + "'");
                }
                acc = acc * 10 + digit;
            }
            tokens.push_back(makeToken(TokenType::Number, digits, digits, start));
            continue;
        }

        // ---- string literals ----------------------------------------------
        if (c == '"') {
            errors::SourceLocation start = here();

            // Triple quote: raw multiline string (spec decision #113).
            // Content is verbatim — no escape processing, no trimming;
            // the FIRST closing '"""' ends it.
            if (peekNext() == '"' && peekAt(2) == '"') {
                const std::size_t startOffset = pos_;
                advance(); advance(); advance();  // opening """
                const std::size_t contentStart = pos_;
                while (true) {
                    if (atEnd()) {
                        fail(start, "unterminated multi-line string literal (expected '\"\"\"')");
                    }
                    if (peek() == '"' && peekNext() == '"' && peekAt(2) == '"') {
                        std::string value(source_, contentStart, pos_ - contentStart);
                        advance(); advance(); advance();  // closing """
                        std::string lexeme = source_.substr(startOffset, pos_ - startOffset);
                        tokens.push_back(makeToken(TokenType::String, std::move(lexeme),
                                                   std::move(value), start));
                        break;
                    }
                    if (peek() == '\n') {
                        advance();
                        line_++;
                        column_ = 1;
                        continue;
                    }
                    advanceChar();
                }
                continue;
            }

            // Single-line string with the official escapes: \n \t \" \\.
            const std::size_t startOffset = pos_;
            std::string value;
            advance();  // opening quote
            while (true) {
                if (atEnd() || peek() == '\n') {
                    fail(start, "unterminated string literal");
                }
                char ch = peek();
                if (ch == '"') {
                    advance();  // closing quote
                    break;
                }
                if (ch == '\\') {
                    errors::SourceLocation escapeStart = here();
                    advance();  // the backslash
                    if (atEnd() || peek() == '\n') {
                        fail(escapeStart, "unterminated string literal");
                    }
                    char esc = peek();
                    switch (esc) {
                        case 'n': value += '\n'; break;
                        case 't': value += '\t'; break;
                        case '"': value += '"';  break;
                        case '\\': value += '\\'; break;
                        default:
                            fail(escapeStart, std::string("unknown escape sequence '\\") +
                                                  esc + "' (supported: \\n \\t \\\" \\\\)");
                    }
                    advance();
                    continue;
                }
                value += advance();
            }
            // Raw lexeme = exact source span of the literal.
            std::string lexeme = source_.substr(startOffset, pos_ - startOffset);
            tokens.push_back(makeToken(TokenType::String, std::move(lexeme),
                                       std::move(value), start));
            continue;
        }

        // Anything else is outside Runa 1.0 syntax.
        {
            errors::SourceLocation start = here();
            std::ostringstream message;
            unsigned char raw = static_cast<unsigned char>(c);
            if (raw < 0x20 || raw == 0x7F) {
                message << "unexpected character (control character 0x" << std::hex
                        << static_cast<int>(raw) << ")";
            } else {
                message << "unexpected character '" << std::string{c} << "'";
            }
            advance();
            fail(start, message.str());
        }
    }
    tokens.push_back(makeToken(TokenType::Eof, "", "", here()));
    return tokens;
}

// ---- development aid -------------------------------------------------------

namespace {

std::string padRight(const std::string& text, std::size_t width) {
    if (text.size() >= width) return text;
    return text + std::string(width - text.size(), ' ');
}

std::string padLeft(const std::string& text, std::size_t width) {
    if (text.size() >= width) return text;
    return std::string(width - text.size(), ' ') + text;
}

std::string visible(const std::string& text) {
    std::string out;
    for (char c : text) {
        switch (c) {
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:   out += c;
        }
    }
    return out;
}

}  // namespace

void dumpTokens(const std::vector<Token>& tokens, std::ostream& out) {
    for (const Token& token : tokens) {
        std::ostringstream loc;
        loc << token.location.line << ':' << token.location.column;
        out << padLeft(loc.str(), 8) << "  "
            << padRight(tokenTypeName(token.type), 11) << ' '
            << visible(token.lexeme);
        if (token.type == TokenType::String) {
            out << "   (value: " << visible(token.value) << ")";
        }
        out << '\n';
    }
}

}  // namespace runa::lexer
