// Runa — minimal JSON reader for package manifests.
//
// Self-contained, dependency-free, deterministic. Supports exactly what the
// manifest schema needs: objects, arrays, strings (with the standard
// escapes), numbers, true/false/null. Duplicate object keys are REJECTED
// (a duplicated key is a malformed manifest, never silently overwritten).
// Trailing content after the top-level value is rejected as well.
//
// Errors carry line/column so manifest problems read like source errors.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace runa::json {

class Value;
using ValuePtr = std::shared_ptr<Value>;

struct ParseError {
    std::string message;
    int line = 0;
    int column = 0;
};

class Value {
public:
    enum class Kind { Null, Boolean, Number, String, Array, Object };

    Kind kind = Kind::Null;
    bool boolean = false;
    // Numbers keep both views: manifests use small integers and version
    // strings; doubles cover everything else deterministically.
    double number = 0.0;
    std::int64_t integer = 0;
    std::string str;
    std::vector<ValuePtr> array;
    // Object entries keep insertion order for deterministic iteration.
    std::vector<std::pair<std::string, ValuePtr>> objectEntries;
    std::unordered_map<std::string, ValuePtr> objectIndex;

    bool isNull() const { return kind == Kind::Null; }
    bool isBoolean() const { return kind == Kind::Boolean; }
    bool isNumber() const { return kind == Kind::Number; }
    bool isString() const { return kind == Kind::String; }
    bool isArray() const { return kind == Kind::Array; }
    bool isObject() const { return kind == Kind::Object; }

    // Object access. Returns nullptr when this is not an object or the key
    // is absent — callers turn that into precise validation messages.
    const Value* find(const std::string& key) const {
        if (kind != Kind::Object) return nullptr;
        const auto it = objectIndex.find(key);
        return (it == objectIndex.end()) ? nullptr : it->second.get();
    }
};

// Parses `text`. On success returns the root value. On failure returns
// nullptr and fills `error`.
ValuePtr parse(const std::string& text, ParseError& error);

}  // namespace runa::json
