#include "runtime/runtime.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <locale>
#include <sstream>

namespace runa::runtime {

Value Value::makeNone() {
    Value value;
    value.kind = Kind::None;
    return value;
}

Value Value::makeString(std::string s) {
    Value value;
    value.kind = Kind::String;
    value.str = std::move(s);
    return value;
}

Value Value::makeNumber(std::int64_t n) {
    Value value;
    value.kind = Kind::Number;
    value.number = n;
    return value;
}

Value Value::makeBoolean(bool b) {
    Value value;
    value.kind = Kind::Boolean;
    value.boolean = b;
    return value;
}

Value Value::makeNull() {
    Value value;
    value.kind = Kind::Null;
    return value;
}

Value Value::makeObject(std::shared_ptr<Instance> instance) {
    Value value;
    value.kind = Kind::Object;
    value.object = std::move(instance);
    return value;
}

Value Value::makeCollection(std::shared_ptr<CollectionData> data) {
    Value value;
    value.kind = Kind::Collection;
    value.collection = std::move(data);
    return value;
}

Value Value::makeFloat(double d) {
    Value value;
    value.kind = Kind::Float;
    value.floating = d;
    return value;
}

Value Value::makePointer(std::uint64_t address) {
    Value value;
    value.kind = Kind::Pointer;
    value.pointer = address;
    // Display serial per run (Pointer#1, Pointer#2 ...). Deterministic and
    // free of real addresses — the value stays opaque in dumps.
    static std::atomic<std::uint64_t> nextPointerId{1};
    value.pointerId = nextPointerId.fetch_add(1);
    return value;
}

// Deterministic float rendering: integral values print with one decimal
// ("2.0") so they never masquerade as integers; everything else uses %.6g
// in the classic C locale ("3.14159", "0.1", "1e+20"). Shared with the AST
// dumper so float literals render identically everywhere.
std::string formatFloatDisplay(double value) {
    if (value == value && value - static_cast<double>(static_cast<long long>(value)) == 0.0 &&
        value >= -1e15 && value <= 1e15) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.1f", value);
        return buffer;
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    return buffer;
}

std::string Value::typeName() const {
    switch (kind) {
        case Kind::None:    return "none";
        case Kind::String:  return "string";
        case Kind::Number:  return "number";
        case Kind::Boolean: return "boolean";
        case Kind::Null:    return "null";
        case Kind::Object:  return "object";
        case Kind::Float:   return "float";
        case Kind::Pointer: return "pointer";
        case Kind::Collection: return "collection";
    }
    return "unknown";
}

std::string Value::displayString() const {
    switch (kind) {
        case Kind::None:    return "none";
        case Kind::String:  return str;
        case Kind::Number:  return std::to_string(number);
        case Kind::Boolean: return boolean ? "true" : "false";
        case Kind::Null:    return "null";
        case Kind::Object:  return object ? (object->className + "#" +
                                             std::to_string(object->objectId))
                                          : std::string("<?object>");
        case Kind::Float:   return formatFloatDisplay(floating);
        case Kind::Pointer: return "Pointer#" + std::to_string(pointerId);
        case Kind::Collection:
            return collection ? collection->displayString() : std::string("<?collection>");
    }
    return "?";
}

std::string Value::dumpString() const {
    switch (kind) {
        case Kind::String: return '"' + str + '"';
        default:           return displayString();
    }
}

void Globals::define(const std::string& name, Value value) {
    map_[name] = std::move(value);
}

const Value* Globals::find(const std::string& name) const {
    auto it = map_.find(name);
    return (it == map_.end()) ? nullptr : &it->second;
}

bool Globals::empty() const { return map_.empty(); }

std::vector<std::pair<std::string, Value>> Globals::sortedEntries() const {
    std::vector<std::pair<std::string, Value>> entries(map_.begin(), map_.end());
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return entries;
}

void printGlobals(const Globals& globals, std::ostream& out) {
    auto entries = globals.sortedEntries();
    if (entries.empty()) {
        out << "(no globals)\n";
        return;
    }
    for (const auto& entry : entries) {
        // Compiler-internal temporaries (the hidden end-value slots of `for`
        // loops, named '#for_endN#') are not user variables: '#' cannot appear
        // in source identifiers, so this filter can never hide user state.
        if (!entry.first.empty() && entry.first.front() == '#') continue;
        out << entry.first << " = " << entry.second.dumpString() << '\n';
    }
}

// ============================================================================
// Collections (the Collection System, v1.0-rc)
// ============================================================================

// Canonical total order over runtime values. Numeric floats use a NaN-safe
// total order (NaNs sort before every other float and compare equal to each
// other) so storage, membership and equality stay deterministic even when a
// native boundary produced NaN. Only same-element-type values are stored in
// one collection; the leading type rank keeps the order total regardless.
int CollectionData::compareCanonical(const Value& a, const Value& b) {
    static const int kRank[] = {
        static_cast<int>(ElementType::Boolean),
        static_cast<int>(ElementType::Number),
        static_cast<int>(ElementType::Float),
        static_cast<int>(ElementType::String),
    };
    const auto rankOf = [](const Value& v) -> int {
        switch (v.kind) {
            case Value::Kind::Boolean: return kRank[0];
            case Value::Kind::Number:  return kRank[1];
            case Value::Kind::Float:   return kRank[2];
            case Value::Kind::String:  return kRank[3];
            default:                   return 4;  // never stored, keeps order total
        }
    };
    if (const int ra = rankOf(a), rb = rankOf(b); ra != rb) return ra < rb ? -1 : 1;
    switch (a.kind) {
        case Value::Kind::Boolean:
            if (a.boolean == b.boolean) return 0;
            return a.boolean ? 1 : -1;  // false < true
        case Value::Kind::Number:
            if (a.number == b.number) return 0;
            return a.number < b.number ? -1 : 1;
        case Value::Kind::Float: {
            const bool aNan = a.floating != a.floating;
            const bool bNan = b.floating != b.floating;
            if (aNan && bNan) return 0;
            if (aNan) return -1;
            if (bNan) return 1;
            if (a.floating == b.floating) return 0;
            return a.floating < b.floating ? -1 : 1;
        }
        case Value::Kind::String:
            if (a.str == b.str) return 0;
            return a.str < b.str ? -1 : 1;  // byte-wise UTF-8 lexicographic
        default:
            return 0;  // unreachable for stored elements (defensive)
    }
}

bool CollectionData::elementAllowed(const Value& value) {
    switch (value.kind) {
        case Value::Kind::Number:
        case Value::Kind::Float:
        case Value::Kind::String:
        case Value::Kind::Boolean:
            return true;
        default:
            return false;
    }
}

CollectionData::ElementType CollectionData::elementTypeOf(const Value& value) {
    switch (value.kind) {
        case Value::Kind::Number:  return ElementType::Number;
        case Value::Kind::Float:   return ElementType::Float;
        case Value::Kind::String:  return ElementType::String;
        case Value::Kind::Boolean: return ElementType::Boolean;
        default:                   return ElementType::Number;  // unreachable
    }
}

const char* CollectionData::elementTypeName(ElementType type) {
    switch (type) {
        case ElementType::Number:  return "number";
        case ElementType::Float:   return "float";
        case ElementType::String:  return "string";
        case ElementType::Boolean: return "boolean";
    }
    return "unknown";
}

bool CollectionData::insert(const Value& value) {
    if (elements.empty()) {
        elements.push_back(value);
        return true;
    }
    // Binary search for the canonical position; equal neighbor = duplicate.
    auto position = std::lower_bound(
        elements.begin(), elements.end(), value,
        [](const Value& a, const Value& b) { return compareCanonical(a, b) < 0; });
    if (position != elements.end() && compareCanonical(*position, value) == 0) {
        return false;  // duplicate — a set keeps one copy
    }
    elements.insert(position, value);
    return true;
}

bool CollectionData::contains(const Value& value) const {
    if (elementTypeOf(value) != elementType) return false;
    const auto position = std::lower_bound(
        elements.begin(), elements.end(), value,
        [](const Value& a, const Value& b) { return compareCanonical(a, b) < 0; });
    return position != elements.end() && compareCanonical(*position, value) == 0;
}

bool CollectionData::equals(const CollectionData& other) const {
    if (elementType != other.elementType || elements.size() != other.elements.size()) {
        return false;
    }
    for (std::size_t i = 0; i < elements.size(); ++i) {
        if (compareCanonical(elements[i], other.elements[i]) != 0) return false;
    }
    return true;
}

std::string CollectionData::displayString() const {
    // Mirror the literal syntax: {10, 20, 30} / {"Runa", "Tluxe"} / {}.
    // String elements are quoted (the dump form) so the output is faithful
    // to what the equivalent source literal would look like.
    std::string out = "{";
    for (std::size_t i = 0; i < elements.size(); ++i) {
        if (i > 0) out += ", ";
        out += elements[i].kind == Value::Kind::String ? elements[i].dumpString()
                                                       : elements[i].displayString();
    }
    out += "}";
    return out;
}

}  // namespace runa::runtime
