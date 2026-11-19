#include "import/platform.hpp"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

#include <sys/stat.h>

#include <cstdlib>
#include <string>

namespace runa::platform {

namespace {

#if defined(_WIN32)

std::string wideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                   static_cast<int>(wide.size()),
                                   nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                        out.data(), size, nullptr, nullptr);
    return out;
}

std::string directoryOf(const std::wstring& path) {
    std::wstring::size_type pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return ".";
    if (pos == 0) return wideToUtf8(path.substr(0, 1));
    return wideToUtf8(path.substr(0, pos));
}

#elif defined(__APPLE__)

std::string directoryOf(const std::string& path) {
    std::string::size_type pos = path.find_last_of('/');
    if (pos == std::string::npos) return ".";
    if (pos == 0) return "/";
    return path.substr(0, pos);
}

#else  // POSIX (Linux and the rest)

std::string directoryOf(const std::string& path) {
    std::string::size_type pos = path.find_last_of('/');
    if (pos == std::string::npos) return ".";
    if (pos == 0) return "/";
    return path.substr(0, pos);
}

#endif

}  // namespace

std::string executableDir() {
#if defined(_WIN32)
    std::wstring buffer;
    buffer.resize(MAX_PATH);
    DWORD size = GetModuleFileNameW(nullptr, buffer.data(),
                                    static_cast<DWORD>(buffer.size()));
    while (size == buffer.size()) {
        // Path longer than the buffer: grow and retry (bounded).
        if (buffer.size() > 32768) break;
        buffer.resize(buffer.size() * 2);
        size = GetModuleFileNameW(nullptr, buffer.data(),
                                  static_cast<DWORD>(buffer.size()));
    }
    if (size == 0) return ".";
    buffer.resize(size);
    return directoryOf(buffer);
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    if (_NSGetExecutablePath(nullptr, &size) != 0 && size > 0) {
        std::string buffer(size, '\0');
        if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
            return directoryOf(buffer.c_str());
        }
    }
    return ".";
#else
    char buffer[PATH_MAX];
    const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0) return ".";
    buffer[length] = '\0';
    return directoryOf(buffer);
#endif
}

std::string nativeTargetKey() {
#if defined(_WIN32)
    const char* os = "windows";
#elif defined(__APPLE__)
    const char* os = "macos";
#else
    const char* os = "linux";
#endif

#if defined(__x86_64__) || defined(__amd64__) || defined(_M_X64)
    const char* arch = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    const char* arch = "arm64";
#elif defined(__arm__) || defined(_M_ARM)
    const char* arch = "armv7";
#else
    const char* arch = "unknown";
#endif

    return std::string(os) + "-" + arch;
}

}  // namespace runa::platform
