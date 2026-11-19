// Runa — package manifest parsing + validation.
//
// Two manifest shapes exist (both JSON, both validated strictly and
// deterministically — a malformed manifest is always a clean error, never a
// crash or a silent fallback):
//
// 1. Library manifest (RPR of a Runa library):
//      { "name": "math", "version": "1.0.0", "type": "runa", "entry": "init.ru" }
//
// 2. Native package manifest (native/<name>/manifest.json):
//      { "name": "graphics", "version": "1.0.0", "type": "native",
//        "native": { "targets": { "linux-x86_64": "linux-x86_64/libgraphics.so" },
//                    "api": { "functions": [ { "name": "...", "symbol": "...",
//                                              "return": "...",
//                                              "parameters": [ ... ] } ] } } }
//
// Unknown top-level keys are IGNORED (the schema is versioned and extensible),
// but unknown keys inside the "native" section are REJECTED with an explicit
// "declared but not supported" error so a manifest can never claim an FFI
// capability the toolchain does not really implement.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace runa::importer {

struct ManifestError {
    std::string message;
};

// The native FFI type vocabulary (manifest metadata). Every supported type is
// really executable by the FFI in this version; float32/float64 are valid as
// RETURN types and rejected as parameter types at COMPILE time with a clean
// "not supported yet" error (documented limitation, see DECISIONS.md).
bool isValidNativeType(const std::string& type);

struct LibraryManifest {
    std::string name;
    std::string version;
    std::string type;  // "runa" | "hybrid" (a "native" library cannot be imported)
    std::string entry; // entry source path inside the archive, e.g. "init.ru"
};

struct NativeFunction {
    std::string name;    // Runa-side API name (namespace member)
    std::string symbol;  // exported symbol inside the binary
    std::string returnType;
    std::vector<std::string> parameterTypes;
};

struct NativeManifest {
    std::string name;
    std::string version;
    std::vector<std::pair<std::string, std::string>> targets;  // platform key -> binary path
    std::vector<NativeFunction> functions;
};

// Parses + validates. Throws ManifestError with a precise message.
LibraryManifest parseLibraryManifest(const std::string& jsonText);
NativeManifest parseNativeManifest(const std::string& jsonText);

}  // namespace runa::importer
