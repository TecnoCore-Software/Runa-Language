// Runa — Runtime.
//
// The runtime provides the value model and services needed while a Runa
// program executes. Runtime and VM remain conceptually separate: the VM
// manipulates runtime values, but the runtime knows nothing about bytecode.
//
// ⚠ PROVISIONAL value model. The designer confirmed the first three value
// kinds (string, number, boolean) through the `printr` built-in. `none` is
// the distinct "no value" kind produced by a bare `return` or by reaching
// the end of a function body — it is never equal to 0, false or "". `null`
// is the object-system literal (a valid value of its own, distinct from
// `none`). An `object` is a class instance: reference-counted (shared), so
// several variables may reference the same object (identity via '===') and
// the memory is reclaimed automatically when the last reference is gone.
// The Kind tag keeps the model ready to grow when more types are confirmed —
// adding a new kind means: one enum entry, one payload field, and one case in
// each of the three rendering functions below.
#pragma once

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace runa::runtime {

// Canonical float rendering (shared with the AST dumper): integral values as
// "2.0", everything else %.6g in the classic C locale.
std::string formatFloatDisplay(double value);

struct Instance;       // defined below (Value and Instance reference each other)
struct CollectionData;  // defined below (Value holds it shared)

struct Value {
    enum class Kind { None, String, Number, Boolean, Null, Object, Float, Pointer,
                      Collection };

    Kind kind = Kind::None;
    std::string str;                        // payload for Kind::String
    std::int64_t number = 0;                // payload for Kind::Number (64-bit signed)
    bool boolean = false;                   // payload for Kind::Boolean
    std::shared_ptr<Instance> object;       // payload for Kind::Object
    std::shared_ptr<CollectionData> collection;  // payload for Kind::Collection
    double floating = 0.0;                  // payload for Kind::Float (native boundary)
    std::uint64_t pointer = 0;              // payload for Kind::Pointer (opaque)
    std::uint64_t pointerId = 0;            // display serial (Pointer#1, Pointer#2 ...)

    static Value makeNone();
    static Value makeString(std::string s);
    static Value makeNumber(std::int64_t n);
    static Value makeBoolean(bool b);
    static Value makeNull();
    static Value makeObject(std::shared_ptr<Instance> instance);
    // Collections are reference-counted like objects: assignment/argument-
    // passing copies the shared reference (aliasing keeps one identity via
    // '==='), and the storage is reclaimed when the last reference is gone.
    static Value makeCollection(std::shared_ptr<CollectionData> data);
    // Float values originate ONLY from native (FFI) calls in this version.
    static Value makeFloat(double d);
    // Pointer values are opaque handles returned by native functions. A null
    // native pointer becomes a plain null value instead.
    static Value makePointer(std::uint64_t address);

    std::string typeName() const;  // none/string/number/boolean/null/object/float/pointer/collection

    // Form used by printr output: none as `none`, null as `null`, objects as
    // `Car#3`, strings raw, numbers decimal, booleans true/false.
    std::string displayString() const;

    // Form used by developer dumps (--dump-globals): strings quoted, the rest
    // identical to displayString.  e.g.  "Runa"   123   true   Car#1
    std::string dumpString() const;
};

// One object instance (class-system runtime state). Instances are reference-
// counted through shared_ptr: assignment/argument-passing copies the shared
// reference (several variables can point at the SAME object — '===' checks
// that), and the memory is released automatically when the last reference
// disappears. The class index is an opaque number the VM resolves; the class
// name is kept for display and diagnostics.
struct Instance {
    std::uint32_t classIndex = 0;
    std::string className;
    std::uint64_t objectId = 0;  // per-run serial number (display: Car#3)
    std::unordered_map<std::string, Value> fields;  // instance fields (flat)
};

// One collection value (the Collection System, v1.0-rc). A Runa collection is
// a SET: element order carries no meaning, duplicates are silently collapsed,
// and every collection holds exactly one element type (number, float, string
// or boolean — the scalar kinds of the current type model).
//
// Storage: the elements vector is kept in ascending CANONICAL order (defined
// by compareCanonical below). That single choice gives deterministic display,
// O(log n) membership via binary search, O(n) equality and O(n+m) sorted-
// merge set operations — with no hashing at all, so hash/equality consistency
// is guaranteed by construction. Collections are immutable through the
// public language API (no add/remove built-ins exist): the only mutation is
// insert() while a literal/set-operation result is being built.
struct CollectionData {
    enum class ElementType : std::uint8_t { Number = 0, Float = 1, String = 2,
                                            Boolean = 3 };

    ElementType elementType = ElementType::Number;
    std::vector<Value> elements;  // ascending canonical order, duplicates removed

    // ---- construction helpers (used by the VM while building) -------------
    // Maps a runtime value to its element type, or false when the value's
    // kind may not be a collection element (objects, null, none, pointers,
    // collections — nesting is not part of this feature).
    static bool elementAllowed(const Value& value);
    static ElementType elementTypeOf(const Value& value);
    // Human-readable element type name for error messages:
    // number / float / string / boolean.
    static const char* elementTypeName(ElementType type);

    // Appends one element, keeping canonical order and dropping duplicates.
    // Returns false when the value was a duplicate (collection unchanged).
    bool insert(const Value& value);

    // Strict same-type membership: the probe must carry exactly the
    // collection's element type (a float never matches a number collection).
    bool contains(const Value& value) const;

    // Set equality: same element type, same size, pairwise equal. Both sides
    // are canonically sorted, so this is a linear scan.
    bool equals(const CollectionData& other) const;

    // Total order over runtime values used for canonical storage. Only
    // same-element-type values are ever stored together; the type rank in
    // front keeps the order total across types anyway (defensive).
    static int compareCanonical(const Value& a, const Value& b);

    // printr form: {10, 20, 30} — canonical order, string elements quoted.
    std::string displayString() const;
};

// Global variable storage of a running program.
class Globals {
public:
    void define(const std::string& name, Value value);  // create or overwrite
    const Value* find(const std::string& name) const;
    bool empty() const;

    // Alphabetically sorted view (deterministic dumps for the tools).
    std::vector<std::pair<std::string, Value>> sortedEntries() const;

private:
    std::unordered_map<std::string, Value> map_;
};

// Development aid: prints globals as Runa-like lines, e.g.
//     name = "Runa"
//     count = 42
//     flag = true
void printGlobals(const Globals& globals, std::ostream& out);

}  // namespace runa::runtime
