// Runa — Native bridge: the native library loader.
//
// One abstraction, two backends:
//
//     NativeLoader::load / unload / lookup / lastError
//       ├── POSIX (Linux, macOS): dlopen / dlclose / dlsym / dlerror
//       └── Windows:              LoadLibraryW / FreeLibrary / GetProcAddress
//
// The loader is intentionally tiny and stateless per handle: the VM's native
// bridge keeps the handle cache and the error policy. All failures surface as
// a NativeLoadError with a platform-reported reason — the loader never
// crashes, never partially loads and never swallows errors silently.
//
// Cross-platform design notes:
//   - Paths are UTF-8 everywhere at this boundary; the Windows backend
//     converts to UTF-16 for LoadLibraryW.
//   - Handles are process-wide dynamic libraries loaded with LOCAL symbol
//     visibility (RTLD_LOCAL) so two packages never leak symbols into each
//     other.
//   - The architecture is extensible to further targets (the platform key
//     selection already supports x86_64 / arm64 / armv7; see platform.hpp).
#pragma once

#include <cstdint>
#include <string>

namespace runa::native {

// Thrown for every load/lookup failure. Message is user-facing.
struct NativeLoadError {
    std::string message;
};

// An opaque handle to one loaded native library. Copyable as a plain value;
// the VM owns the lifetime through NativeHandleGuard-style explicit closes.
class NativeLibrary {
public:
    NativeLibrary() = default;
    explicit NativeLibrary(std::uintptr_t handle) : handle_(handle) {}

    bool valid() const { return handle_ != 0; }
    std::uintptr_t raw() const { return handle_; }

private:
    std::uintptr_t handle_ = 0;
};

class NativeLoader {
public:
    // Loads a dynamic library from an absolute filesystem path.
    // Throws NativeLoadError with the platform reason on failure.
    static NativeLibrary load(const std::string& absolutePath);

    // Unloads a library. Safe to call with an invalid handle (no-op).
    static void unload(NativeLibrary library);

    // Resolves a symbol. Returns 0 when the symbol is absent (caller reports
    // a clean NativeSymbolNotFound error).
    static std::uintptr_t lookup(NativeLibrary library, const std::string& symbol);

    // The last platform error text (for messages composed without throwing).
    static std::string lastError();
};

}  // namespace runa::native
