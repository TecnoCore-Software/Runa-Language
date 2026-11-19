#include "import/manifest.hpp"

#include <string>
#include <vector>

#include "import/json.hpp"
#include "import/rpr.hpp"

namespace runa::importer {

namespace {

// Wraps rpr::validateArchivePath as a plain message ("" when valid).
std::string rprPathProblem(const std::string& path) {
    return rpr::validateArchivePath(path);
}

// Insertion-ordered key list of a JSON object.
std::vector<std::string> nativeSectionKeys(const json::Value& value) {
    std::vector<std::string> keys;
    keys.reserve(value.objectEntries.size());
    for (const auto& entry : value.objectEntries) keys.push_back(entry.first);
    return keys;
}

bool isIdentifier(const std::string& text) {
    if (text.empty()) return false;
    if (!((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= 'A' && text[0] <= 'Z') ||
          text[0] == '_')) {
        return false;
    }
    for (char c : text) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    return true;
}

[[noreturn]] void fail(const std::string& message) {
    throw ManifestError{message};
}

const json::Value& requireObject(const json::Value& value, const std::string& what) {
    if (!value.isObject()) fail(what + " must be a JSON object");
    return value;
}

std::string requireString(const json::Value& parent, const std::string& key,
                          const std::string& what) {
    const json::Value* field = parent.find(key);
    if (field == nullptr) fail(what + " is missing the required field \"" + key + "\"");
    if (!field->isString()) fail(what + ": field \"" + key + "\" must be a string");
    return field->str;
}

std::string validateVersion(const std::string& version) {
    // x.y.z — three dot-separated numeric components. Simple, deterministic.
    std::size_t position = 0;
    int components = 0;
    while (components < 3) {
        const std::size_t dot = version.find('.', position);
        const std::string part =
            (dot == std::string::npos) ? version.substr(position)
                                       : version.substr(position, dot - position);
        if (part.empty()) fail("version must have three numeric components (x.y.z)");
        for (char c : part) {
            if (c < '0' || c > '9') fail("version must have three numeric components (x.y.z)");
        }
        if (dot == std::string::npos) break;
        position = dot + 1;
        ++components;
    }
    if (version.find('.', position) != std::string::npos) {
        fail("version must have exactly three numeric components (x.y.z)");
    }
    return version;
}

std::string validateTypeField(const std::string& type) {
    if (type != "runa" && type != "native" && type != "hybrid") {
        fail("manifest \"type\" must be \"runa\", \"native\" or \"hybrid\" (got \"" +
             type + "\")");
    }
    return type;
}

}  // namespace

bool isValidNativeType(const std::string& type) {
    return type == "int8" || type == "uint8" || type == "int16" || type == "uint16" ||
           type == "int32" || type == "uint32" || type == "int64" || type == "uint64" ||
           type == "float32" || type == "float64" || type == "bool" || type == "char" ||
           type == "pointer" || type == "cstring" || type == "void";
}

LibraryManifest parseLibraryManifest(const std::string& jsonText) {
    json::ParseError parseError;
    const json::ValuePtr root = json::parse(jsonText, parseError);
    if (root == nullptr) {
        fail("manifest is not valid JSON (" + parseError.message + " at " +
             std::to_string(parseError.line) + ":" + std::to_string(parseError.column) + ")");
    }
    requireObject(*root, "manifest");

    LibraryManifest manifest;
    manifest.name = requireString(*root, "name", "manifest");
    if (!isIdentifier(manifest.name)) {
        fail("manifest \"name\" must be a valid Runa identifier (got \"" + manifest.name +
             "\")");
    }
    manifest.version = validateVersion(requireString(*root, "version", "manifest"));
    manifest.type = validateTypeField(requireString(*root, "type", "manifest"));
    if (manifest.type == "native") {
        fail("manifest \"type\" is \"native\" — a native package cannot be used with "
             "'import' (native packages are consumed with 'cimport')");
    }
    manifest.entry = requireString(*root, "entry", "manifest");
    const std::string entryProblem = rprPathProblem(manifest.entry);
    if (!entryProblem.empty()) {
        fail("manifest \"entry\": " + entryProblem);
    }
    return manifest;
}

NativeManifest parseNativeManifest(const std::string& jsonText) {
    json::ParseError parseError;
    const json::ValuePtr root = json::parse(jsonText, parseError);
    if (root == nullptr) {
        fail("manifest is not valid JSON (" + parseError.message + " at " +
             std::to_string(parseError.line) + ":" + std::to_string(parseError.column) + ")");
    }
    requireObject(*root, "manifest");

    NativeManifest manifest;
    manifest.name = requireString(*root, "name", "manifest");
    if (!isIdentifier(manifest.name)) {
        fail("manifest \"name\" must be a valid Runa identifier (got \"" + manifest.name +
             "\")");
    }
    manifest.version = validateVersion(requireString(*root, "version", "manifest"));

    const json::Value* nativeSection = root->find("native");
    if (nativeSection == nullptr) {
        fail("native manifest is missing the \"native\" section");
    }
    requireObject(*nativeSection, "the \"native\" section");
    for (const auto& key : nativeSectionKeys(*nativeSection)) {
        if (key != "targets" && key != "api") {
            fail("\"native." + key +
                 "\" is declared but not supported by this version of the toolchain");
        }
    }

    const json::Value* targets = nativeSection->find("targets");
    if (targets == nullptr) fail("the \"native\" section is missing \"targets\"");
    requireObject(*targets, "\"native.targets\"");
    for (const auto& entry : targets->objectEntries) {
        if (!entry.second->isString()) {
            fail("native target \"" + entry.first + "\" must be a relative path string");
        }
        const std::string problem = rprPathProblem(entry.second->str);
        if (!problem.empty()) {
            fail("native target \"" + entry.first + "\": " + problem);
        }
        for (const auto& existing : manifest.targets) {
            if (existing.first == entry.first) {
                fail("duplicate native target \"" + entry.first + "\"");
            }
        }
        manifest.targets.emplace_back(entry.first, entry.second->str);
    }

    const json::Value* api = nativeSection->find("api");
    if (api == nullptr) fail("the \"native\" section is missing \"api\"");
    requireObject(*api, "\"native.api\"");
    for (const auto& key : nativeSectionKeys(*api)) {
        if (key != "functions") {
            fail("\"native.api." + key +
                 "\" is declared but not supported by this version of the toolchain");
        }
    }
    const json::Value* functions = api->find("functions");
    if (functions == nullptr) fail("\"native.api\" is missing \"functions\"");
    if (!functions->isArray()) fail("\"native.api.functions\" must be an array");
    for (const json::ValuePtr& item : functions->array) {
        requireObject(*item, "every entry of \"native.api.functions\"");
        NativeFunction function;
        function.name = requireString(*item, "name", "a native API function");
        if (!isIdentifier(function.name)) {
            fail("native function \"name\" must be a valid Runa identifier (got \"" +
                 function.name + "\")");
        }
        for (const NativeFunction& existing : manifest.functions) {
            if (existing.name == function.name) {
                fail("duplicate native function name \"" + function.name + "\"");
            }
        }
        function.symbol = requireString(*item, "symbol", "a native API function");
        if (!isIdentifier(function.symbol)) {
            fail("native function \"symbol\" must be a valid C identifier (got \"" +
                 function.symbol + "\")");
        }
        function.returnType = requireString(*item, "return", "a native API function");
        if (!isValidNativeType(function.returnType)) {
            fail("native function '" + function.name + "': unknown return type \"" +
                 function.returnType + "\"");
        }
        const json::Value* parameters = item->find("parameters");
        if (parameters == nullptr) {
            fail("native function '" + function.name + "' is missing \"parameters\"");
        }
        if (!parameters->isArray()) {
            fail("native function '" + function.name + "': \"parameters\" must be an array");
        }
        if (parameters->array.size() > 16) {
            fail("native function '" + function.name +
                 "' has too many parameters (the maximum is 16)");
        }
        for (const json::ValuePtr& parameter : parameters->array) {
            if (!parameter->isString()) {
                fail("native function '" + function.name +
                     "': parameter types must be strings");
            }
            if (!isValidNativeType(parameter->str)) {
                fail("native function '" + function.name + "': unknown parameter type \"" +
                     parameter->str + "\"");
            }
            if (parameter->str == "void") {
                fail("native function '" + function.name +
                     "': \"void\" is not a valid parameter type");
            }
            function.parameterTypes.push_back(parameter->str);
        }
        manifest.functions.push_back(std::move(function));
    }
    return manifest;
}

}  // namespace runa::importer
