#include "runtime/native/native_loader.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <string>
#include <vector>

namespace runa::native {

#if defined(_WIN32)

namespace {

std::wstring utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                   static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        wide.data(), size);
    return wide;
}

std::string lastWindowsError() {
    DWORD code = GetLastError();
    if (code == 0) return "unknown error";
    LPWSTR buffer = nullptr;
    const DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::string message;
    if (size > 0 && buffer != nullptr) {
        int utf8Size = WideCharToMultiByte(CP_UTF8, 0, buffer,
                                           static_cast<int>(size), nullptr, 0,
                                           nullptr, nullptr);
        message.resize(static_cast<std::size_t>(utf8Size));
        WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(size),
                            message.data(), utf8Size, nullptr, nullptr);
        while (!message.empty() && (message.back() == '\n' || message.back() == '\r' ||
                                    message.back() == ' ')) {
            message.pop_back();
        }
    }
    if (buffer != nullptr) LocalFree(buffer);
    if (message.empty()) message = "Windows error code " + std::to_string(code);
    return message;
}

}  // namespace

NativeLibrary NativeLoader::load(const std::string& absolutePath) {
    HMODULE handle = LoadLibraryW(utf8ToWide(absolutePath).c_str());
    if (handle == nullptr) {
        throw NativeLoadError{lastWindowsError()};
    }
    return NativeLibrary{reinterpret_cast<std::uintptr_t>(handle)};
}

void NativeLoader::unload(NativeLibrary library) {
    if (library.valid()) {
        FreeLibrary(reinterpret_cast<HMODULE>(library.raw()));
    }
}

std::uintptr_t NativeLoader::lookup(NativeLibrary library, const std::string& symbol) {
    if (!library.valid()) return 0;
    FARPROC address = GetProcAddress(reinterpret_cast<HMODULE>(library.raw()),
                                     symbol.c_str());
    return address == nullptr ? 0 : reinterpret_cast<std::uintptr_t>(address);
}

std::string NativeLoader::lastError() {
    return lastWindowsError();
}

#else  // POSIX (Linux / macOS / the rest)

NativeLibrary NativeLoader::load(const std::string& absolutePath) {
    // RTLD_NOW: every symbol must resolve up front so a broken library fails
    // at load time with a precise reason, never mid-program.
    // RTLD_LOCAL: symbols stay private to the library (two native packages
    // never leak symbols into each other).
    void* handle = ::dlopen(absolutePath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char* reason = ::dlerror();
        throw NativeLoadError{reason != nullptr ? std::string(reason)
                                                : std::string("dlopen failed")};
    }
    return NativeLibrary{reinterpret_cast<std::uintptr_t>(handle)};
}

void NativeLoader::unload(NativeLibrary library) {
    if (library.valid()) {
        ::dlclose(reinterpret_cast<void*>(library.raw()));
    }
}

std::uintptr_t NativeLoader::lookup(NativeLibrary library, const std::string& symbol) {
    if (!library.valid()) return 0;
    // Clear any stale error state before the lookup.
    ::dlerror();
    void* address = ::dlsym(reinterpret_cast<void*>(library.raw()), symbol.c_str());
    return address == nullptr ? 0 : reinterpret_cast<std::uintptr_t>(address);
}

std::string NativeLoader::lastError() {
    const char* reason = ::dlerror();
    return reason != nullptr ? std::string(reason) : std::string("unknown load error");
}

#endif

}  // namespace runa::native
