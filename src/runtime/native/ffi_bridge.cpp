#include "runtime/native/ffi_bridge.hpp"

#include <limits>
#include <string>
#include <vector>

namespace runa::native {

namespace {

constexpr std::size_t kMaxNativeArgs = 16;

[[noreturn]] void fail(const std::string& message) {
    throw NativeCallError{message};
}

// ---- value -> integer slot (with strict type + range validation) -----------

std::uint64_t slotFromValue(const runtime::Value& value, FfiType type,
                            std::size_t position, const std::string& displayName,
                            std::vector<std::string>& stringKeeps) {
    const std::string where = "native call '" + displayName + "' argument " +
                              std::to_string(position + 1);
    switch (type) {
        case FfiType::Int8:
        case FfiType::Int16:
        case FfiType::Int32:
        case FfiType::Int64: {
            if (value.kind != runtime::Value::Kind::Number) {
                fail(where + " must be a number, got " + value.typeName());
            }
            const std::int64_t n = value.number;
            if (type == FfiType::Int8 && (n < -128 || n > 127)) {
                fail(where + " is out of the int8 range (-128..127)");
            }
            if (type == FfiType::Int16 && (n < -32768 || n > 32767)) {
                fail(where + " is out of the int16 range (-32768..32767)");
            }
            if (type == FfiType::Int32 &&
                (n < std::numeric_limits<std::int32_t>::min() ||
                 n > std::numeric_limits<std::int32_t>::max())) {
                fail(where + " is out of the int32 range");
            }
            // Int64: any 64-bit signed value.
            return static_cast<std::uint64_t>(n);
        }
        case FfiType::UInt8:
        case FfiType::UInt16:
        case FfiType::UInt32:
        case FfiType::UInt64: {
            if (value.kind != runtime::Value::Kind::Number) {
                fail(where + " must be a number, got " + value.typeName());
            }
            const std::int64_t n = value.number;
            if (n < 0) {
                fail(where + " must not be negative for an unsigned native type");
            }
            if (type == FfiType::UInt8 && n > 0xFF) {
                fail(where + " is out of the uint8 range (0..255)");
            }
            if (type == FfiType::UInt16 && n > 0xFFFF) {
                fail(where + " is out of the uint16 range (0..65535)");
            }
            if (type == FfiType::UInt32 && n > 0xFFFFFFFFLL) {
                fail(where + " is out of the uint32 range");
            }
            if (type == FfiType::UInt64 &&
                n > std::numeric_limits<std::int64_t>::max()) {
                fail(where + " exceeds the uint64 range representable in Runa "
                             "(values above 2^63-1 are not supported yet)");
            }
            return static_cast<std::uint64_t>(n);
        }
        case FfiType::Bool:
            if (value.kind != runtime::Value::Kind::Boolean) {
                fail(where + " must be a boolean, got " + value.typeName());
            }
            return value.boolean ? 1u : 0u;
        case FfiType::Char:
            if (value.kind != runtime::Value::Kind::Number) {
                fail(where + " must be a number (a character code 0..255), got " +
                     value.typeName());
            }
            if (value.number < 0 || value.number > 255) {
                fail(where + " is out of the char range (0..255)");
            }
            return static_cast<std::uint64_t>(value.number);
        case FfiType::Pointer:
            // An opaque pointer value, or Runa null for a NULL pointer.
            if (value.kind == runtime::Value::Kind::Null) return 0;
            if (value.kind != runtime::Value::Kind::Pointer) {
                fail(where + " must be a pointer (or null), got " + value.typeName());
            }
            return value.pointer;
        case FfiType::CString:
            // A borrowed NUL-terminated copy of the Runa string, alive until
            // the call returns. Runa null maps to a NULL pointer.
            if (value.kind == runtime::Value::Kind::Null) return 0;
            if (value.kind != runtime::Value::Kind::String) {
                fail(where + " must be a string (or null), got " + value.typeName());
            }
            stringKeeps.push_back(value.str + '\0');
            return reinterpret_cast<std::uint64_t>(stringKeeps.back().c_str());
        case FfiType::Float32:
        case FfiType::Float64:
            fail(where + ": native parameter type '" + ffiTypeName(type) +
                 "' is not supported by this version of the toolchain");
        default:
            fail(where + ": unknown native parameter type");
    }
}

// ---- integer-slot -> value (per return type) --------------------------------

runtime::Value valueFromSlot(std::uint64_t raw, FfiType type) {
    switch (type) {
        case FfiType::Int8:
            return runtime::Value::makeNumber(
                static_cast<std::int64_t>(static_cast<std::int8_t>(raw & 0xFF)));
        case FfiType::UInt8:
            return runtime::Value::makeNumber(
                static_cast<std::int64_t>(raw & 0xFF));
        case FfiType::Int16:
            return runtime::Value::makeNumber(
                static_cast<std::int64_t>(static_cast<std::int16_t>(raw & 0xFFFF)));
        case FfiType::UInt16:
            return runtime::Value::makeNumber(
                static_cast<std::int64_t>(raw & 0xFFFF));
        case FfiType::Int32:
            return runtime::Value::makeNumber(
                static_cast<std::int64_t>(static_cast<std::int32_t>(raw & 0xFFFFFFFFu)));
        case FfiType::UInt32:
            return runtime::Value::makeNumber(
                static_cast<std::int64_t>(raw & 0xFFFFFFFFu));
        case FfiType::Int64:
            return runtime::Value::makeNumber(static_cast<std::int64_t>(raw));
        case FfiType::UInt64:
            // The full unsigned range is not representable in Runa numbers;
            // the bit pattern is reinterpreted (documented limitation).
            return runtime::Value::makeNumber(static_cast<std::int64_t>(raw));
        case FfiType::Bool:
            return runtime::Value::makeBoolean((raw & 1u) != 0);
        case FfiType::Char:
            return runtime::Value::makeNumber(
                static_cast<std::int64_t>(raw & 0xFF));
        default:
            // Pointer/CString/Float/Void are handled by the caller.
            return runtime::Value::makeNone();
    }
}

// ---- the variadic-prototype dispatch (arity switch, 0..16) ------------------

template <typename Ret>
Ret dispatchByArity(std::uintptr_t address, const std::uint64_t* slots,
                    std::size_t argc) {
    using VariadicFn = Ret (*)(...);
    const VariadicFn fn = reinterpret_cast<VariadicFn>(address);
    switch (argc) {
        case 0:  return fn();
        case 1:  return fn(slots[0]);
        case 2:  return fn(slots[0], slots[1]);
        case 3:  return fn(slots[0], slots[1], slots[2]);
        case 4:  return fn(slots[0], slots[1], slots[2], slots[3]);
        case 5:  return fn(slots[0], slots[1], slots[2], slots[3], slots[4]);
        case 6:  return fn(slots[0], slots[1], slots[2], slots[3], slots[4], slots[5]);
        case 7:  return fn(slots[0], slots[1], slots[2], slots[3], slots[4], slots[5],
                           slots[6]);
        case 8:  return fn(slots[0], slots[1], slots[2], slots[3], slots[4], slots[5],
                           slots[6], slots[7]);
        case 9:  return fn(slots[0], slots[1], slots[2], slots[3], slots[4], slots[5],
                           slots[6], slots[7], slots[8]);
        case 10: return fn(slots[0], slots[1], slots[2], slots[3], slots[4], slots[5],
                           slots[6], slots[7], slots[8], slots[9]);
        case 11: return fn(slots[0], slots[1], slots[2], slots[3], slots[4], slots[5],
                           slots[6], slots[7], slots[8], slots[9], slots[10]);
        case 12: return fn(slots[0], slots[1], slots[2], slots[3], slots[4], slots[5],
                           slots[6], slots[7], slots[8], slots[9], slots[10],
                           slots[11]);
        case 13: return fn(slots[0], slots[1], slots[2], slots[3], slots[4], slots[5],
                           slots[6], slots[7], slots[8], slots[9], slots[10],
                           slots[11], slots[12]);
        case 14: return fn(slots[0], slots[1], slots[2], slots[3], slots[4], slots[5],
                           slots[6], slots[7], slots[8], slots[9], slots[10],
                           slots[11], slots[12], slots[13]);
        case 15: return fn(slots[0], slots[1], slots[2], slots[3], slots[4], slots[5],
                           slots[6], slots[7], slots[8], slots[9], slots[10],
                           slots[11], slots[12], slots[13], slots[14]);
        case 16: return fn(slots[0], slots[1], slots[2], slots[3], slots[4], slots[5],
                           slots[6], slots[7], slots[8], slots[9], slots[10],
                           slots[11], slots[12], slots[13], slots[14], slots[15]);
        default: fail("native functions support at most 16 arguments");
    }
}

}  // namespace

bool isValidFfiType(std::uint8_t value) {
    return value >= static_cast<std::uint8_t>(FfiType::Int8) &&
           value <= static_cast<std::uint8_t>(FfiType::Void);
}

const char* ffiTypeName(FfiType type) {
    switch (type) {
        case FfiType::Int8:    return "int8";
        case FfiType::UInt8:   return "uint8";
        case FfiType::Int16:   return "int16";
        case FfiType::UInt16:  return "uint16";
        case FfiType::Int32:   return "int32";
        case FfiType::UInt32:  return "uint32";
        case FfiType::Int64:   return "int64";
        case FfiType::UInt64:  return "uint64";
        case FfiType::Float32: return "float32";
        case FfiType::Float64: return "float64";
        case FfiType::Bool:    return "bool";
        case FfiType::Char:    return "char";
        case FfiType::Pointer: return "pointer";
        case FfiType::CString: return "cstring";
        case FfiType::Void:    return "void";
    }
    return "?";
}

runtime::Value callNativeFunction(std::uintptr_t functionAddress,
                                  const NativeSignature& signature,
                                  const std::vector<runtime::Value>& args,
                                  const std::string& displayName) {
    if (args.size() != signature.parameterTypes.size()) {
        fail("'" + displayName + "' expects " +
             std::to_string(signature.parameterTypes.size()) + " argument(s), got " +
             std::to_string(args.size()));
    }
    if (args.size() > kMaxNativeArgs) {
        fail("'" + displayName + "' has too many parameters (the maximum is 16)");
    }

    // Convert and keep every borrowed buffer alive until the call returns.
    std::uint64_t slots[kMaxNativeArgs] = {0};
    std::vector<std::string> stringKeeps;
    stringKeeps.reserve(args.size());
    for (std::size_t i = 0; i < args.size(); ++i) {
        slots[i] = slotFromValue(args[i], signature.parameterTypes[i], i,
                                 displayName, stringKeeps);
    }

    switch (signature.returnType) {
        case FfiType::Float32: {
            const float raw = dispatchByArity<float>(functionAddress, slots, args.size());
            return runtime::Value::makeFloat(static_cast<double>(raw));
        }
        case FfiType::Float64: {
            const double raw =
                dispatchByArity<double>(functionAddress, slots, args.size());
            return runtime::Value::makeFloat(raw);
        }
        case FfiType::Void: {
            (void)dispatchByArity<unsigned long long>(functionAddress, slots,
                                                      args.size());
            return runtime::Value::makeNone();
        }
        case FfiType::CString: {
            const std::uint64_t raw =
                dispatchByArity<unsigned long long>(functionAddress, slots,
                                                    args.size());
            if (raw == 0) return runtime::Value::makeNull();
            const char* text = reinterpret_cast<const char*>(raw);
            // Contract: the returned pointer is a NUL-terminated string that
            // stays valid until this call returns (copied immediately).
            return runtime::Value::makeString(std::string(text));
        }
        case FfiType::Pointer: {
            const std::uint64_t raw =
                dispatchByArity<unsigned long long>(functionAddress, slots,
                                                    args.size());
            if (raw == 0) return runtime::Value::makeNull();
            return runtime::Value::makePointer(raw);
        }
        default: {
            const std::uint64_t raw =
                dispatchByArity<unsigned long long>(functionAddress, slots,
                                                    args.size());
            return valueFromSlot(raw, signature.returnType);
        }
    }
}

}  // namespace runa::native
