// Runa — structured error system.
//
// Every stage of the toolchain (lexer, parser, compiler, VM) reports failures
// through the same structured error type:
//
//     filename:line:column
//     ErrorKind: message
//
// The exact presentation is provisional (see DECISIONS.md), but the fields
// (filename, line, column, kind, message) are fixed by the Runa baseline spec.
#pragma once

#include <string>

namespace runa::errors {

// Location of one character inside a source file. Line/column are 1-based.
// Columns are byte-based for now (ASCII assumption; see DECISIONS.md).
struct SourceLocation {
    std::string filename;
    int line = 0;    // 0 means "no source location available"
    int column = 0;  // 0 means "no source location available"
};

enum class ErrorKind {
    LexicalError,
    SyntaxError,
    CompileError,
    RuntimeError,
};

const char* errorKindName(ErrorKind kind);

struct RunaError {
    ErrorKind kind = ErrorKind::RuntimeError;
    SourceLocation location;
    std::string message;

    // Renders the header lines, e.g.
    //     main.ru:3:9
    //     SyntaxError: unexpected token
    // When no source location is available, only the second line is rendered.
    std::string format() const;
};

// Thrown after a structured error has been built. Tools catch this,
// print the error and exit with a non-zero status.
class RunaErrorException {
public:
    explicit RunaErrorException(RunaError error) : error_(std::move(error)) {}
    const RunaError& error() const { return error_; }

private:
    RunaError error_;
};

// Prints the error to stderr, optionally with the offending source line and a
// caret under the failing column. `source` may be empty (e.g. runtime errors).
void report(const RunaError& error, const std::string& source = "");

// Returns the 1-based `lineNumber` line of `source` (trailing '\r' stripped).
// Returns "" if the line does not exist.
std::string extractLine(const std::string& source, int lineNumber);

}  // namespace runa::errors
