// Runa — Standard library.
//
// STATUS: the built-in surface of the language. `printr` was confirmed by the
// language designer; the collection built-ins (sum..last) are part of the
// Collection System (v1.0-rc). The registry below is the single place where
// built-ins live:
//
//   - the COMPILER reads BuiltinSpec metadata (name + arity) to validate
//     call statements at compile time — it never executes anything;
//   - the VM looks up the handler at runtime and invokes it with the
//     already-evaluated arguments.
//
// The registry is static and stateless on purpose: no install step, no init
// order pitfalls, and both tools (runac/runavm) always agree on what exists.
//
// Adding a new built-in later = one spec entry + one handler + one lookup
// branch. Value kinds it can consume are defined by the runtime value model.
#pragma once

#include <cstddef>
#include <iosfwd>
#include <stdexcept>
#include <string>
#include <vector>

#include "runtime/runtime.hpp"

namespace runa::stdlib {

// Compile-time metadata for one built-in function.
struct BuiltinSpec {
    std::string name;
    std::size_t minArgs;
    std::size_t maxArgs;  // SIZE_MAX = accepts any number of arguments
};

// Native built-in handler: receives the output stream to write to and the
// already-evaluated argument values, and RETURNS exactly one runtime value —
// calls are expressions, so every call must leave one value behind. Built-ins
// that produce nothing useful (printr) return the distinct "none" value.
using BuiltinFn = runtime::Value (*)(std::ostream& out,
                                     const std::vector<runtime::Value>& args);

// Error path of a built-in handler. A handler that cannot fulfill its
// contract (wrong argument kinds, an empty collection where elements are
// required, numeric overflow...) throws BuiltinError; the VM converts it
// into the standard clean runtime failure — the process never crashes.
struct BuiltinError : std::runtime_error {
    explicit BuiltinError(const std::string& message)
        : std::runtime_error(message) {}
};

// Returns the spec of the named built-in, or nullptr if it does not exist.
const BuiltinSpec* findBuiltin(const std::string& name);

// Returns the handler of the named built-in, or nullptr if it does not exist.
BuiltinFn findBuiltinFn(const std::string& name);

// ---- the built-ins themselves ------------------------------------------------

// printr(value, ...) — prints all values joined by single spaces, followed by
// a newline. Strings print raw (no quotes). Accepts 1+ arguments of any kind.
// Returns the "none" value (printr is used for its output, not its result).
runtime::Value printr(std::ostream& out, const std::vector<runtime::Value>& args);

// ---- collection built-ins (the Collection System, v1.0-rc) -------------------
//
// Every collection built-in requires a collection in its first argument slot
// and reports wrong kinds via BuiltinError. See README ("Collection System")
// for the language-level semantics these implement.

runtime::Value sum(std::ostream&, const std::vector<runtime::Value>& args);          // sum(c)
runtime::Value product(std::ostream&, const std::vector<runtime::Value>& args);      // product(c)
runtime::Value average(std::ostream&, const std::vector<runtime::Value>& args);      // average(c)
runtime::Value min(std::ostream&, const std::vector<runtime::Value>& args);          // min(c)
runtime::Value max(std::ostream&, const std::vector<runtime::Value>& args);          // max(c)
runtime::Value count(std::ostream&, const std::vector<runtime::Value>& args);        // count(c)
runtime::Value contains(std::ostream&, const std::vector<runtime::Value>& args);     // contains(c, x)
runtime::Value empty(std::ostream&, const std::vector<runtime::Value>& args);        // empty(c)
runtime::Value union_(std::ostream&, const std::vector<runtime::Value>& args);       // union(a, b)
runtime::Value intersection(std::ostream&, const std::vector<runtime::Value>& args); // intersection(a, b)
runtime::Value difference(std::ostream&, const std::vector<runtime::Value>& args);   // difference(a, b)
runtime::Value sort(std::ostream&, const std::vector<runtime::Value>& args);         // sort(c)
runtime::Value reverse(std::ostream&, const std::vector<runtime::Value>& args);      // reverse(c)
runtime::Value first(std::ostream&, const std::vector<runtime::Value>& args);        // first(c)
runtime::Value last(std::ostream&, const std::vector<runtime::Value>& args);         // last(c)

}  // namespace runa::stdlib
