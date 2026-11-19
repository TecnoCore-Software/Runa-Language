// nruna — the native-ecosystem tool of Runa.
//
//     nruna init <name>            Scaffold a native package (manifest + C template)
//     nruna build <directory>      Compile the package sources for this platform
//     nruna check <directory>      Validate the manifest + current-platform binary
//
// nruna is a build/authoring tool: the language-level `cimport` goes through
// the Runtime/Native bridge (manifest metadata + NativeLoader), never through
// nruna at run time. nruna only helps CREATE and VERIFY native packages.
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <dirent.h>
#endif
#include <sys/stat.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "import/manifest.hpp"
#include "import/platform.hpp"
#include "runtime/native/native_loader.hpp"
#include "tools/tool_common.hpp"

namespace {

bool isDirectory(const std::string& path) {
    struct stat info;
    return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

void makeDirs(const std::string& path) {
    if (path.empty()) return;
    std::string current;
    std::size_t start = 0;
    if (path.front() == '/') {
        current = "/";
        start = 1;
    }
    while (start <= path.size()) {
        const std::size_t end = path.find('/', start);
        const std::size_t length =
            (end == std::string::npos) ? path.size() - start : end - start;
        if (length > 0) {
            if (!current.empty() && current.back() != '/') current += '/';
            current += path.substr(start, length);
#if defined(_WIN32)
            ::CreateDirectoryA(current.c_str(), nullptr);
#else
            ::mkdir(current.c_str(), 0755);
#endif
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
}

void writeFileOrExit(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::cerr << "nruna: cannot write " << path << "\n";
        std::exit(1);
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    if (!out) {
        std::cerr << "nruna: failed while writing " << path << "\n";
        std::exit(1);
    }
}

void printUsage(std::ostream& out) {
    out << "Usage:\n"
           "  nruna init <name>            scaffold a native package under native/<name>\n"
           "  nruna build <directory>      build the native package for this platform\n"
           "  nruna check <directory>      validate manifest + current-platform binary\n"
           "\n"
           "Platform target of this toolchain: "
        << runa::platform::nativeTargetKey() << "\n";
}

std::string platformKey() { return runa::platform::nativeTargetKey(); }

std::string defaultBinaryName(const std::string& name) {
#if defined(_WIN32)
    return name + ".dll";
#elif defined(__APPLE__)
    return "lib" + name + ".dylib";
#else
    return "lib" + name + ".so";
#endif
}

int commandInit(const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cerr << "nruna: usage: nruna init <name>\n";
        return 2;
    }
    const std::string name = args[0];
    if (name.empty() || name.find_first_not_of(
                            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") !=
                            std::string::npos) {
        std::cerr << "nruna: the package name must be a valid Runa identifier\n";
        return 2;
    }
    const std::string dir = "native/" + name;
    if (isDirectory(dir)) {
        std::cerr << "nruna: " << dir << " already exists\n";
        return 1;
    }
    makeDirs(dir + "/" + platformKey());
    makeDirs(dir + "/src");

    std::string manifest = "{\n";
    manifest += "  \"name\": \"" + name + "\",\n";
    manifest += "  \"version\": \"1.0.0\",\n";
    manifest += "  \"type\": \"native\",\n";
    manifest += "  \"native\": {\n";
    manifest += "    \"targets\": {\n";
    manifest += "      \"" + platformKey() + "\": \"" + platformKey() + "/" +
                defaultBinaryName(name) + "\"\n";
    manifest += "    },\n";
    manifest += "    \"api\": {\n";
    manifest += "      \"functions\": [\n";
    manifest += "        { \"name\": \"ping\", \"symbol\": \"" + name + "_ping\", "
                "\"return\": \"int64\", \"parameters\": [] }\n";
    manifest += "      ]\n";
    manifest += "    }\n";
    manifest += "  }\n";
    manifest += "}\n";

    std::string source = "// Native package '" + name + "' — built by nruna.\n";
    source += "// long long maps to Runa numbers (int64); see the README type table.\n";
    source += "long long " + name + "_ping(void) {\n";
    source += "    return 1;\n";
    source += "}\n";

    writeFileOrExit(dir + "/manifest.json", manifest);
    writeFileOrExit(dir + "/src/" + name + ".c", source);
    std::cout << "nruna: scaffolded " << dir << "\n"
              << "  - manifest.json (api: " << name << ".ping)\n"
              << "  - src/" << name << ".c\n"
              << "next: nruna build " << dir << "\n";
    return 0;
}

std::string readWhole(const std::string& path, bool& ok) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        ok = false;
        return {};
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    ok = !file.bad();
    return buffer.str();
}

int commandBuild(const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cerr << "nruna: usage: nruna build <directory>\n";
        return 2;
    }
    const std::string dir = args[0];
    bool ok = false;
    const std::string manifestText = readWhole(dir + "/manifest.json", ok);
    if (!ok) {
        std::cerr << "nruna: " << dir << "/manifest.json not found\n";
        return 1;
    }
    runa::importer::NativeManifest manifest;
    try {
        manifest = runa::importer::parseNativeManifest(manifestText);
    } catch (const runa::importer::ManifestError& problem) {
        std::cerr << "nruna: invalid manifest: " << problem.message << "\n";
        return 1;
    }

    // Collect the C/C++ sources under <dir>/src.
    std::vector<std::string> sources;
#if defined(_WIN32)
    {
        WIN32_FIND_DATAA data;
        HANDLE handle = ::FindFirstFileA((dir + "/src/*").c_str(), &data);
        if (handle != INVALID_HANDLE_VALUE) {
            do {
                const std::string name = data.cFileName;
                const bool isC = name.size() > 2 && name.rfind(".c") == name.size() - 2;
                const bool isCc = name.size() > 3 && name.rfind(".cc") == name.size() - 3;
                const bool isCpp = name.size() > 4 && name.rfind(".cpp") == name.size() - 4;
                if (isC || isCc || isCpp) {
                    sources.push_back(dir + "/src/" + name);
                }
            } while (::FindNextFileA(handle, &data));
            ::FindClose(handle);
        }
    }
#else
    if (DIR* handle = ::opendir((dir + "/src").c_str())) {
        while (dirent* entry = ::readdir(handle)) {
            const std::string name = entry->d_name;
            const bool isC = name.size() > 2 && name.rfind(".c") == name.size() - 2;
            const bool isCc = name.size() > 3 && name.rfind(".cc") == name.size() - 3;
            const bool isCpp = name.size() > 4 && name.rfind(".cpp") == name.size() - 4;
            if (isC || isCc || isCpp) {
                sources.push_back(dir + "/src/" + name);
            }
        }
        ::closedir(handle);
    }
#endif
    if (sources.empty()) {
        std::cerr << "nruna: no .c/.cpp sources under " << dir << "/src\n";
        return 1;
    }

    const std::string key = platformKey();
    std::string binaryPath;
    for (const auto& target : manifest.targets) {
        if (target.first == key) binaryPath = target.second;
    }
    if (binaryPath.empty()) {
        std::cerr << "nruna: manifest has no target for '" << key
                  << "'. Add:\n      \"" << key << "\": \"" << key << "/"
                  << defaultBinaryName(manifest.name) << "\"\n      under "
                  << "\"native.targets\" in " << dir << "/manifest.json\n";
        return 1;
    }

    const std::string output = dir + "/" + binaryPath;
    makeDirs(runa::tools::dirnameOf(output));

#if defined(_WIN32)
    std::string command = "gcc -shared -O2";
#elif defined(__APPLE__)
    std::string command = "cc -shared -fPIC -O2 -dynamiclib";
#else
    std::string command = "cc -shared -fPIC -O2";
#endif
    for (const std::string& source : sources) {
#if defined(_WIN32)
        command += " \"" + source + "\"";
#else
        command += " '" + source + "'";
#endif
    }
#if defined(_WIN32)
    command += " -o \"" + output + "\"";
#else
    command += " -o '" + output + "'";
#endif
    const int rc = std::system(command.c_str());
    if (rc != 0) {
        std::cerr << "nruna: compilation failed\n";
        return 1;
    }
    std::cout << "nruna: built " << output << "\n";
    return 0;
}

int commandCheck(const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cerr << "nruna: usage: nruna check <directory>\n";
        return 2;
    }
    const std::string dir = args[0];
    bool ok = false;
    const std::string manifestText = readWhole(dir + "/manifest.json", ok);
    if (!ok) {
        std::cerr << "nruna: " << dir << "/manifest.json not found\n";
        return 1;
    }
    runa::importer::NativeManifest manifest;
    try {
        manifest = runa::importer::parseNativeManifest(manifestText);
    } catch (const runa::importer::ManifestError& problem) {
        std::cerr << "nruna: invalid manifest: " << problem.message << "\n";
        return 1;
    }
    std::cout << "nruna: manifest OK (" << manifest.name << " " << manifest.version
              << ", " << manifest.functions.size() << " api function(s))\n";

    const std::string key = platformKey();
    std::string binaryPath;
    for (const auto& target : manifest.targets) {
        if (target.first == key) binaryPath = target.second;
    }
    if (binaryPath.empty()) {
        std::cout << "nruna: no target for '" << key << "' (cannot verify binary)\n";
        return 1;
    }
    const std::string binary = dir + "/" + binaryPath;
    try {
        const auto library = runa::native::NativeLoader::load(binary);
        std::size_t found = 0;
        for (const auto& function : manifest.functions) {
            if (runa::native::NativeLoader::lookup(library, function.symbol) != 0) {
                ++found;
            } else {
                std::cout << "nruna: MISSING symbol '" << function.symbol << "' ("
                          << function.name << ")\n";
            }
        }
        runa::native::NativeLoader::unload(library);
        std::cout << "nruna: binary OK (" << binary << "): " << found << "/"
                  << manifest.functions.size() << " symbols resolved\n";
        return found == manifest.functions.size() ? 0 : 1;
    } catch (const runa::native::NativeLoadError& problem) {
        std::cerr << "nruna: cannot load " << binary << ": " << problem.message << "\n";
        return 1;
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "-h" || args[0] == "--help") {
        printUsage(std::cout);
        return args.empty() ? 2 : 0;
    }
    const std::string command = args[0];
    const std::vector<std::string> rest(args.begin() + 1, args.end());
    if (command == "init") return commandInit(rest);
    if (command == "build") return commandBuild(rest);
    if (command == "check") return commandCheck(rest);
    std::cerr << "nruna: unknown command '" << command << "'\n";
    printUsage(std::cerr);
    return 2;
}
