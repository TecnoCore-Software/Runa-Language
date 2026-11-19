// Runa — Native bridge: FFI type mapping + call dispatch.
//
// The bridge converts Runa values across the native boundary and invokes the
// target function through the platform C ABI. It owns:
//
//   1. the canonical FFI type vocabulary (FfiType),
//   2. the deterministic Runa-value <-> native-value mapping,
//   3. the call mechanism.
//
// CALL MECHANISM (documented, deliberate):
// Every supported PARAMETER type (int8..uint64, bool, char, pointer, cstring)
// belongs to the INTEGER register class on all supported ABIs (SysV x86_64,
// AArch64, Win64). The call therefore goes through a variadic prototype with
// the matching number of integer slots — the classic dlsym technique: each
// slot lands in exactly the register (or stack position) the non-variadic
// callee expects. The arity is dispatched by an explicit switch (0..16).
//
// float32/float64 RETURN values are read from the SSE/FP return register by
// instantiating the same dispatch with float/double; float PARAMETERS are NOT
// supported in this version (on Win64 variadic floats change register class;
// kept out honestly instead of half-working — see DECISIONS.md). Structs,
// buffers and callbacks are likewise future work: the manifest vocabulary may
// describe them, but the compiler rejects them with a clean "not supported"
// error, so the bridge never sees them.
//
// Memory safety across the boundary:
//   - cstring arguments: the bridge copies the Runa string into a
//     NUL-terminated temporary that lives until the call returns (borrowed
//     by the callee, owned by the bridge). Runa null maps to a NULL pointer.
//   - cstring returns: the bridge copies the returned bytes into a fresh
//     Runa string immediately (Runa-owned copy). A NULL return maps to null.
//   - pointer returns: stored as an OPAQUE handle. A NULL pointer maps to
//     the null value. Runa code has no dereference syntax (by design).
//   - The bridge never frees native-owned memory and never exposes VM memory
//     to the callee beyond the borrowed argument buffers above.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "runtime/runtime.hpp"

namespace runa::native {

// The canonical FFI type vocabulary (matches the manifest metadata and the
// bytecode natives-table byte values 1:1).
enum class FfiType : std::uint8_t {
    Int8 = 1,
    UInt8 = 2,
    Int16 = 3,
    UInt16 = 4,
    Int32 = 5,
    UInt32 = 6,
    Int64 = 7,
    UInt64 = 8,
    Float32 = 9,
    Float64 = 10,
    Bool = 11,
    Char = 12,
    Pointer = 13,
    CString = 14,
    Void = 15,
};

bool isValidFfiType(std::uint8_t value);
const char* ffiTypeName(FfiType type);

struct NativeSignature {
    FfiType returnType = FfiType::Void;
    std::vector<FfiType> parameterTypes;
};

// Thrown for argument-conversion problems. Message is user-facing and
// already includes the argument position.
struct NativeCallError {
    std::string message;
};

// Invokes `functionAddress` with `args` converted per `signature`.
// `displayName` (e.g. "graphics.create_window") is used in error messages.
// Returns exactly one runtime value (void returns produce the "none" value).
runtime::Value callNativeFunction(std::uintptr_t functionAddress,
                                  const NativeSignature& signature,
                                  const std::vector<runtime::Value>& args,
                                  const std::string& displayName);

}  // namespace runa::native
