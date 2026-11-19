#include "import/json.hpp"

#include <cerrno>
#include <cstdlib>
#include <limits>

namespace runa::json {

namespace {

class Parser {
public:
    Parser(const std::string& text, ParseError& error) : text_(text), error_(error) {}

    ValuePtr run() {
        skipWhitespace();
        ValuePtr root = parseValue();
        if (!root) return nullptr;
        skipWhitespace();
        if (pos_ < text_.size()) {
            return fail("unexpected content after the JSON value");
        }
        return root;
    }

private:
    const std::string& text_;
    ParseError& error_;
    std::size_t pos_ = 0;
    int line_ = 1;
    int column_ = 1;

    bool atEnd() const { return pos_ >= text_.size(); }
    char peek() const { return atEnd() ? '\0' : text_[pos_]; }

    void advance() {
        if (atEnd()) return;
        if (text_[pos_] == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
        ++pos_;
    }

    ValuePtr fail(const std::string& message) {
        error_.message = message;
        error_.line = line_;
        error_.column = column_;
        return nullptr;
    }

    void skipWhitespace() {
        while (!atEnd()) {
            const char c = peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                advance();
            } else {
                break;
            }
        }
    }

    bool match(char c) {
        if (peek() == c) {
            advance();
            return true;
        }
        return false;
    }

    ValuePtr parseValue() {
        if (atEnd()) return fail("unexpected end of JSON input");
        switch (peek()) {
            case '{': return parseObject();
            case '[': return parseArray();
            case '"': {
                std::string out;
                if (!parseString(out)) return nullptr;
                auto value = std::make_shared<Value>();
                value->kind = Value::Kind::String;
                value->str = std::move(out);
                return value;
            }
            case 't': return parseLiteral("true", Value::Kind::Boolean, true);
            case 'f': return parseLiteral("false", Value::Kind::Boolean, false);
            case 'n': return parseLiteral("null", Value::Kind::Null, false);
            default:  return parseNumber();
        }
    }

    ValuePtr parseLiteral(const char* literal, Value::Kind kind, bool flag) {
        for (const char* p = literal; *p != '\0'; ++p) {
            if (atEnd() || peek() != *p) {
                return fail(std::string("invalid literal (expected '") + literal + "')");
            }
            advance();
        }
        auto value = std::make_shared<Value>();
        value->kind = kind;
        value->boolean = flag;
        return value;
    }

    ValuePtr parseObject() {
        advance();  // consume '{'
        auto value = std::make_shared<Value>();
        value->kind = Value::Kind::Object;
        skipWhitespace();
        if (match('}')) return value;
        while (true) {
            skipWhitespace();
            if (peek() != '"') {
                return fail("expected a string key inside the JSON object");
            }
            std::string key;
            if (!parseString(key)) return nullptr;
            if (value->objectIndex.count(key) > 0) {
                return fail("duplicate key '" + key + "' in the JSON object");
            }
            skipWhitespace();
            if (!match(':')) return fail("expected ':' after the object key");
            skipWhitespace();
            ValuePtr member = parseValue();
            if (!member) return nullptr;
            value->objectEntries.emplace_back(key, member);
            value->objectIndex.emplace(std::move(key), std::move(member));
            skipWhitespace();
            if (match(',')) continue;
            if (match('}')) return value;
            return fail("expected ',' or '}' inside the JSON object");
        }
    }

    ValuePtr parseArray() {
        advance();  // consume '['
        auto value = std::make_shared<Value>();
        value->kind = Value::Kind::Array;
        skipWhitespace();
        if (match(']')) return value;
        while (true) {
            skipWhitespace();
            if (atEnd()) return fail("unexpected end of JSON input inside an array");
            ValuePtr element = parseValue();
            if (!element) return nullptr;
            value->array.push_back(std::move(element));
            skipWhitespace();
            if (match(',')) continue;
            if (match(']')) return value;
            return fail("expected ',' or ']' inside the JSON array");
        }
    }

    bool parseString(std::string& out) {
        if (peek() != '"') {
            error_.message = "expected a string";
            error_.line = line_;
            error_.column = column_;
            return false;
        }
        advance();  // consume the opening quote
        while (true) {
            if (atEnd()) {
                error_.message = "unterminated string";
                error_.line = line_;
                error_.column = column_;
                return false;
            }
            char c = peek();
            if (c == '"') {
                advance();
                return true;
            }
            if (c == '\\') {
                advance();
                if (atEnd()) {
                    error_.message = "unterminated escape sequence";
                    error_.line = line_;
                    error_.column = column_;
                    return false;
                }
                char e = peek();
                switch (e) {
                    case '"':  out += '"';  advance(); break;
                    case '\\': out += '\\'; advance(); break;
                    case '/':  out += '/';  advance(); break;
                    case 'b':  out += '\b'; advance(); break;
                    case 'f':  out += '\f'; advance(); break;
                    case 'n':  out += '\n'; advance(); break;
                    case 'r':  out += '\r'; advance(); break;
                    case 't':  out += '\t'; advance(); break;
                    case 'u': {
                        advance();
                        unsigned int code = 0;
                        for (int i = 0; i < 4; ++i) {
                            if (atEnd() || hexValue(peek()) < 0) {
                                error_.message = "invalid \\u escape";
                                error_.line = line_;
                                error_.column = column_;
                                return false;
                            }
                            code = code * 16 + static_cast<unsigned int>(hexValue(peek()));
                            advance();
                        }
                        // Encode as UTF-8 (surrogate pairs not needed for
                        // manifests; lone surrogates are rejected).
                        if (code >= 0xD800 && code <= 0xDFFF) {
                            error_.message = "surrogate escapes are not supported";
                            error_.line = line_;
                            error_.column = column_;
                            return false;
                        }
                        appendUtf8(out, code);
                        break;
                    }
                    default:
                        error_.message = "invalid escape sequence";
                        error_.line = line_;
                        error_.column = column_;
                        return false;
                }
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                error_.message = "control character inside a string";
                error_.line = line_;
                error_.column = column_;
                return false;
            }
            out += c;
            advance();
        }
    }

    static int hexValue(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    static void appendUtf8(std::string& out, unsigned int code) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    ValuePtr parseNumber() {
        const int startLine = line_;
        const int startColumn = column_;
        std::string digits;
        if (peek() == '-') {
            digits += '-';
            advance();
        }
        bool anyDigits = false;
        while (!atEnd() && peek() >= '0' && peek() <= '9') {
            digits += peek();
            advance();
            anyDigits = true;
        }
        if (match('.')) {
            digits += '.';
            bool fractionDigits = false;
            while (!atEnd() && peek() >= '0' && peek() <= '9') {
                digits += peek();
                advance();
                fractionDigits = true;
            }
            if (!fractionDigits) return failAt(startLine, startColumn, "invalid number");
        }
        if (peek() == 'e' || peek() == 'E') {
            digits += peek();
            advance();
            if (peek() == '+' || peek() == '-') {
                digits += peek();
                advance();
            }
            bool exponentDigits = false;
            while (!atEnd() && peek() >= '0' && peek() <= '9') {
                digits += peek();
                advance();
                exponentDigits = true;
            }
            if (!exponentDigits) return failAt(startLine, startColumn, "invalid number");
        }
        if (!anyDigits) {
            return failAt(startLine, startColumn, "expected a JSON value");
        }
        errno = 0;
        char* end = nullptr;
        const double parsed = std::strtod(digits.c_str(), &end);
        if (end != digits.c_str() + digits.size() || errno == ERANGE) {
            return failAt(startLine, startColumn, "number out of range");
        }
        auto value = std::make_shared<Value>();
        value->kind = Value::Kind::Number;
        value->number = parsed;
        value->integer = static_cast<std::int64_t>(parsed);
        return value;
    }

    ValuePtr failAt(int line, int column, const std::string& message) {
        error_.message = message;
        error_.line = line;
        error_.column = column;
        return nullptr;
    }
};

}  // namespace

ValuePtr parse(const std::string& text, ParseError& error) {
    error = ParseError{};
    Parser parser(text, error);
    return parser.run();
}

}  // namespace runa::json
