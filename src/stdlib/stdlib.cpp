#include "stdlib/stdlib.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ostream>

namespace runa::stdlib {

namespace {

// The registry. Keep entries sorted by name for predictable iteration.
// `union` is a reserved word in C++, so its handler is named union_ while
// the language-level name stays exactly `union`.
const std::vector<BuiltinSpec>& registry() {
    static const std::vector<BuiltinSpec> specs = {
        {"printr", 0, SIZE_MAX},
        {"average", 1, 1},
        {"contains", 2, 2},
        {"count", 1, 1},
        {"difference", 2, 2},
        {"empty", 1, 1},
        {"first", 1, 1},
        {"intersection", 2, 2},
        {"last", 1, 1},
        {"max", 1, 1},
        {"min", 1, 1},
        {"product", 1, 1},
        {"reverse", 1, 1},
        {"sort", 1, 1},
        {"sum", 1, 1},
        {"union", 2, 2},
    };
    return specs;
}

// ---- shared collection helpers ----------------------------------------------

// Returns the collection payload of args[index], or throws a clean
// BuiltinError naming the built-in and the offending kind.
const runtime::CollectionData& requireCollection(
    const std::vector<runtime::Value>& args, const char* name,
    std::size_t index = 0) {
    const runtime::Value& value = args[index];
    if (value.kind != runtime::Value::Kind::Collection || !value.collection) {
        throw BuiltinError(std::string(name) + " expects a collection as its " +
                           (index == 0 ? "first" : "second") + " argument, got " +
                           value.typeName());
    }
    return *value.collection;
}

bool isNumericCollection(const runtime::CollectionData& collection) {
    return collection.elementType == runtime::CollectionData::ElementType::Number ||
           collection.elementType == runtime::CollectionData::ElementType::Float;
}

// Renders one element for the sort()/reverse() ordered views. String elements
// are quoted so the view is faithful to the source-level value, exactly like
// the collection display format.
std::string renderElement(const runtime::Value& value) {
    return value.kind == runtime::Value::Kind::String ? value.dumpString()
                                                      : value.displayString();
}

// Joins the elements in the given direction (ascending = the canonical
// storage order) into the "visible ordered result" of sort()/reverse().
std::string orderedView(const runtime::CollectionData& collection, bool ascending) {
    std::string out;
    const std::size_t size = collection.elements.size();
    for (std::size_t i = 0; i < size; ++i) {
        if (i > 0) out += ", ";
        out += renderElement(ascending ? collection.elements[i]
                                       : collection.elements[size - 1 - i]);
    }
    return out;
}

// Signed 64-bit overflow predicates for the numeric aggregations. Same
// formulas as the VM's arithmetic (written without any operation that could
// itself overflow, so no C++ undefined behavior can occur here).
bool addOverflows(std::int64_t a, std::int64_t b) {
    return (b > 0) ? (a > std::numeric_limits<std::int64_t>::max() - b)
                   : (a < std::numeric_limits<std::int64_t>::min() - b);
}

bool mulOverflows(std::int64_t a, std::int64_t b) {
    if (a == 0 || b == 0) return false;
    if (a == -1) return b == std::numeric_limits<std::int64_t>::min();
    if (b == -1) return a == std::numeric_limits<std::int64_t>::min();
    const std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    const std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    if (a > 0 && b > 0) return a > kMax / b;
    if (a < 0 && b < 0) return a < kMax / b;
    return (a > 0) ? (a > kMin / b) : (b > kMin / a);
}

// Builds the result collection of a set operation from an already-sorted,
// already-deduplicated element vector (one sorted merge pass, O(n+m)).
runtime::Value makeCollection(
    runtime::CollectionData::ElementType elementType,
    std::vector<runtime::Value> elements) {
    auto data = std::make_shared<runtime::CollectionData>();
    data->elementType = elementType;
    data->elements = std::move(elements);
    return runtime::Value::makeCollection(std::move(data));
}

}  // namespace

const BuiltinSpec* findBuiltin(const std::string& name) {
    for (const BuiltinSpec& spec : registry()) {
        if (spec.name == name) return &spec;
    }
    return nullptr;
}

BuiltinFn findBuiltinFn(const std::string& name) {
    if (name == "printr") return &printr;
    if (name == "sum") return &sum;
    if (name == "product") return &product;
    if (name == "average") return &average;
    if (name == "min") return &min;
    if (name == "max") return &max;
    if (name == "count") return &count;
    if (name == "contains") return &contains;
    if (name == "empty") return &empty;
    if (name == "union") return &union_;
    if (name == "intersection") return &intersection;
    if (name == "difference") return &difference;
    if (name == "sort") return &sort;
    if (name == "reverse") return &reverse;
    if (name == "first") return &first;
    if (name == "last") return &last;
    return nullptr;
}

runtime::Value printr(std::ostream& out, const std::vector<runtime::Value>& args) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i > 0) out << ' ';
        out << args[i].displayString();
    }
    out << '\n';
    return runtime::Value::makeNone();
}

// ============================================================================
// Collection built-ins (the Collection System, v1.0-rc)
// ============================================================================

// sum(collection) — total of a numeric collection. Empty is NOT defined
// (spec 1.0 section 69 + DECISIONS #116: uniform RuntimeError, no made-up
// identities). Float collections sum as doubles; the result must stay
// finite — Runa never auto-produces Infinity.
runtime::Value sum(std::ostream&, const std::vector<runtime::Value>& args) {
    const runtime::CollectionData& collection = requireCollection(args, "sum");
    if (!isNumericCollection(collection)) {
        throw BuiltinError(std::string("sum expects a numeric collection, got a collection of ") +
                           runtime::CollectionData::elementTypeName(collection.elementType) +
                           " values");
    }
    if (collection.elements.empty()) {
        throw BuiltinError("sum of an empty collection is not defined");
    }
    if (collection.elementType == runtime::CollectionData::ElementType::Float) {
        double total = 0.0;
        for (const runtime::Value& element : collection.elements) {
            total += element.floating;
        }
        if (!std::isfinite(total)) {
            throw BuiltinError("float overflow in sum");
        }
        return runtime::Value::makeFloat(total);
    }
    std::int64_t total = 0;
    for (const runtime::Value& element : collection.elements) {
        if (addOverflows(total, element.number)) {
            throw BuiltinError("integer overflow in sum");
        }
        total += element.number;
    }
    return runtime::Value::makeNumber(total);
}

// product(collection) — product of a numeric collection. Empty is NOT
// defined (spec 1.0 section 69 + DECISIONS #116). Overflow follows the
// arithmetic semantics: a clean runtime error, never silent wrap-around.
runtime::Value product(std::ostream&, const std::vector<runtime::Value>& args) {
    const runtime::CollectionData& collection = requireCollection(args, "product");
    if (!isNumericCollection(collection)) {
        throw BuiltinError(
            std::string("product expects a numeric collection, got a collection of ") +
            runtime::CollectionData::elementTypeName(collection.elementType) +
            " values");
    }
    if (collection.elements.empty()) {
        throw BuiltinError("product of an empty collection is not defined");
    }
    if (collection.elementType == runtime::CollectionData::ElementType::Float) {
        double total = 1.0;
        for (const runtime::Value& element : collection.elements) {
            total *= element.floating;
        }
        if (!std::isfinite(total)) {
            throw BuiltinError("float overflow in product");
        }
        return runtime::Value::makeFloat(total);
    }
    std::int64_t total = 1;
    for (const runtime::Value& element : collection.elements) {
        if (mulOverflows(total, element.number)) {
            throw BuiltinError("integer overflow in product");
        }
        total *= element.number;
    }
    return runtime::Value::makeNumber(total);
}

// average(collection) — arithmetic mean of a numeric collection. Empty →
// error (no deterministic value exists). An all-integer collection whose mean
// is mathematically exact yields a number (average({10,20,30,40}) → 25);
// otherwise the result is a float (average({1,2}) → 1.5).
runtime::Value average(std::ostream&, const std::vector<runtime::Value>& args) {
    const runtime::CollectionData& collection = requireCollection(args, "average");
    if (!isNumericCollection(collection)) {
        throw BuiltinError(
            std::string("average expects a numeric collection, got a collection of ") +
            runtime::CollectionData::elementTypeName(collection.elementType) +
            " values");
    }
    if (collection.elements.empty()) {
        throw BuiltinError("average of an empty collection is not defined");
    }
    if (collection.elementType == runtime::CollectionData::ElementType::Float) {
        double total = 0.0;
        for (const runtime::Value& element : collection.elements) {
            total += element.floating;
        }
        return runtime::Value::makeFloat(total / static_cast<double>(collection.elements.size()));
    }
    std::int64_t total = 0;
    for (const runtime::Value& element : collection.elements) {
        if (addOverflows(total, element.number)) {
            throw BuiltinError("integer overflow in average");
        }
        total += element.number;
    }
    const std::int64_t size = static_cast<std::int64_t>(collection.elements.size());
    if (total % size == 0) {
        return runtime::Value::makeNumber(total / size);
    }
    return runtime::Value::makeFloat(static_cast<double>(total) /
                                     static_cast<double>(size));
}

// min(collection) / max(collection) — smallest/largest member of a NUMERIC
// collection. Empty → error. Non-numeric collections have no confirmed
// ordering semantics in the language yet, so they are rejected cleanly.
runtime::Value min(std::ostream&, const std::vector<runtime::Value>& args) {
    const runtime::CollectionData& collection = requireCollection(args, "min");
    if (!isNumericCollection(collection)) {
        throw BuiltinError(std::string("min expects a numeric collection, got a collection of ") +
                           runtime::CollectionData::elementTypeName(collection.elementType) +
                           " values");
    }
    if (collection.elements.empty()) {
        throw BuiltinError("min of an empty collection");
    }
    return collection.elements.front();  // canonical ascending order
}

runtime::Value max(std::ostream&, const std::vector<runtime::Value>& args) {
    const runtime::CollectionData& collection = requireCollection(args, "max");
    if (!isNumericCollection(collection)) {
        throw BuiltinError(std::string("max expects a numeric collection, got a collection of ") +
                           runtime::CollectionData::elementTypeName(collection.elementType) +
                           " values");
    }
    if (collection.elements.empty()) {
        throw BuiltinError("max of an empty collection");
    }
    return collection.elements.back();  // canonical ascending order
}

// count(collection) — the number of DISTINCT members (a set never stores
// duplicates, so that is simply its size).
runtime::Value count(std::ostream&, const std::vector<runtime::Value>& args) {
    const runtime::CollectionData& collection = requireCollection(args, "count");
    return runtime::Value::makeNumber(
        static_cast<std::int64_t>(collection.elements.size()));
}

// contains(collection, value) — membership. The probe must carry exactly the
// collection's element type; any other value (different type, or a
// non-scalar kind) is simply not a member → false. Deterministic, no error.
runtime::Value contains(std::ostream&, const std::vector<runtime::Value>& args) {
    const runtime::CollectionData& collection = requireCollection(args, "contains");
    return runtime::Value::makeBoolean(collection.contains(args[1]));
}

// empty(collection) — true exactly when the collection has no members.
// An empty collection is a valid value, distinct from null.
runtime::Value empty(std::ostream&, const std::vector<runtime::Value>& args) {
    const runtime::CollectionData& collection = requireCollection(args, "empty");
    return runtime::Value::makeBoolean(collection.elements.empty());
}

// ---- set operations ----------------------------------------------------------
// Both operands must be collections of the SAME element type. All three run
// as one sorted merge over the two canonically-ordered element vectors:
// O(n+m), and the result is built already sorted and duplicate-free.

runtime::Value union_(std::ostream&, const std::vector<runtime::Value>& args) {
    const runtime::CollectionData& a = requireCollection(args, "union", 0);
    const runtime::CollectionData& b = requireCollection(args, "union", 1);
    if (a.elementType != b.elementType) {
        throw BuiltinError(std::string("union expects two collections with the same element "
                           "type (found ") +
                           runtime::CollectionData::elementTypeName(a.elementType) +
                           " and " +
                           runtime::CollectionData::elementTypeName(b.elementType) + ")");
    }
    std::vector<runtime::Value> result;
    result.reserve(a.elements.size() + b.elements.size());
    std::size_t i = 0, j = 0;
    while (i < a.elements.size() && j < b.elements.size()) {
        const int order =
            runtime::CollectionData::compareCanonical(a.elements[i], b.elements[j]);
        if (order < 0) {
            result.push_back(a.elements[i++]);
        } else if (order > 0) {
            result.push_back(b.elements[j++]);
        } else {
            result.push_back(a.elements[i++]);  // shared member — take once
            ++j;
        }
    }
    while (i < a.elements.size()) result.push_back(a.elements[i++]);
    while (j < b.elements.size()) result.push_back(b.elements[j++]);
    return makeCollection(a.elementType, std::move(result));
}

runtime::Value intersection(std::ostream&, const std::vector<runtime::Value>& args) {
    const runtime::CollectionData& a = requireCollection(args, "intersection", 0);
    const runtime::CollectionData& b = requireCollection(args, "intersection", 1);
    if (a.elementType != b.elementType) {
        throw BuiltinError(
            std::string("intersection expects two collections with the same element type "
            "(found ") +
            runtime::CollectionData::elementTypeName(a.elementType) + " and " +
            runtime::CollectionData::elementTypeName(b.elementType) + ")");
    }
    std::vector<runtime::Value> result;
    std::size_t i = 0, j = 0;
    while (i < a.elements.size() && j < b.elements.size()) {
        const int order =
            runtime::CollectionData::compareCanonical(a.elements[i], b.elements[j]);
        if (order < 0) {
            ++i;
        } else if (order > 0) {
            ++j;
        } else {
            result.push_back(a.elements[i++]);
            ++j;
        }
    }
    return makeCollection(a.elementType, std::move(result));
}

runtime::Value difference(std::ostream&, const std::vector<runtime::Value>& args) {
    const runtime::CollectionData& a = requireCollection(args, "difference", 0);
    const runtime::CollectionData& b = requireCollection(args, "difference", 1);
    if (a.elementType != b.elementType) {
        throw BuiltinError(
            std::string("difference expects two collections with the same element type "
            "(found ") +
            runtime::CollectionData::elementTypeName(a.elementType) + " and " +
            runtime::CollectionData::elementTypeName(b.elementType) + ")");
    }
    std::vector<runtime::Value> result;
    std::size_t i = 0, j = 0;
    while (i < a.elements.size() && j < b.elements.size()) {
        const int order =
            runtime::CollectionData::compareCanonical(a.elements[i], b.elements[j]);
        if (order < 0) {
            result.push_back(a.elements[i++]);  // in a, not in b
        } else if (order > 0) {
            ++j;
        } else {
            ++i;  // shared member — excluded from a - b
            ++j;
        }
    }
    while (i < a.elements.size()) result.push_back(a.elements[i++]);
    return makeCollection(a.elementType, std::move(result));
}

// ---- ordering views ----------------------------------------------------------
// Collections are inherently UNORDERED; sort()/reverse() do not turn them
// into ordered ones. They produce the spec's "visible ordered result" — a
// deterministic string view of the members in ascending / descending
// canonical order. The collection itself is untouched.

runtime::Value sort(std::ostream&, const std::vector<runtime::Value>& args) {
    const runtime::CollectionData& collection = requireCollection(args, "sort");
    return runtime::Value::makeString(orderedView(collection, true));
}

runtime::Value reverse(std::ostream&, const std::vector<runtime::Value>& args) {
    const runtime::CollectionData& collection = requireCollection(args, "reverse");
    return runtime::Value::makeString(orderedView(collection, false));
}

// first(collection) / last(collection) — the smallest / largest member under
// the standard canonical ordering (the first/last element of sort()'s
// result). Empty → error.
runtime::Value first(std::ostream&, const std::vector<runtime::Value>& args) {
    const runtime::CollectionData& collection = requireCollection(args, "first");
    if (collection.elements.empty()) {
        throw BuiltinError("first of an empty collection");
    }
    return collection.elements.front();
}

runtime::Value last(std::ostream&, const std::vector<runtime::Value>& args) {
    const runtime::CollectionData& collection = requireCollection(args, "last");
    if (collection.elements.empty()) {
        throw BuiltinError("last of an empty collection");
    }
    return collection.elements.back();
}

}  // namespace runa::stdlib
