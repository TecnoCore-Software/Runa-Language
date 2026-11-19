#include "errors/errors.hpp"

#include <iostream>
#include <sstream>

namespace runa::errors {

const char* errorKindName(ErrorKind kind) {
    switch (kind) {
        case ErrorKind::LexicalError: return "LexicalError";
        case ErrorKind::SyntaxError:  return "SyntaxError";
        case ErrorKind::CompileError: return "CompileError";
        case ErrorKind::RuntimeError: return "RuntimeError";
    }
    return "Error";
}

std::string RunaError::format() const {
    std::ostringstream out;
    if (location.line > 0) {
        out << location.filename << ':' << location.line << ':' << location.column << '\n';
    }
    out << errorKindName(kind) << ": " << message;
    return out.str();
}

void report(const RunaError& error, const std::string& source) {
    std::cerr << error.format() << '\n';
    if (error.location.line <= 0 || source.empty()) return;

    std::string line = extractLine(source, error.location.line);
    if (line.empty()) return;

    // Right-align the line number gutter.
    int width = 1;
    for (int n = error.location.line; n >= 10; n /= 10) width++;

    std::cerr << std::string(static_cast<std::size_t>(width), ' ')
              << error.location.line << " | " << line << '\n';
    std::cerr << std::string(static_cast<std::size_t>(width), ' ') << " | ";

    // The caret is byte-based; ASCII assumption for now (see DECISIONS.md).
    std::size_t caretOffset = (error.location.column >= 1)
                                  ? static_cast<std::size_t>(error.location.column - 1)
                                  : 0;
    if (caretOffset > line.size()) caretOffset = line.size();
    std::cerr << std::string(caretOffset, ' ') << "^\n";
}

std::string extractLine(const std::string& source, int lineNumber) {
    if (lineNumber <= 0) return "";
    int current = 1;
    std::size_t start = 0;
    while (true) {
        std::size_t end = source.find('\n', start);
        std::size_t length =
            (end == std::string::npos) ? source.size() - start : end - start;
        if (current == lineNumber) {
            std::string line = source.substr(start, length);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return line;
        }
        if (end == std::string::npos) return "";
        start = end + 1;
        current++;
    }
}

}  // namespace runa::errors
