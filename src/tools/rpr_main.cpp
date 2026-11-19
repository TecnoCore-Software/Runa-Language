// rpr — the Runa package tool.
//
//     rpr pack <directory> <output.rpr>    Build an RPR archive from a directory
//     rpr list <package.rpr>               Print the archive contents
//     rpr inspect <package.rpr>            Print manifest + package metadata
//     rpr extract <package.rpr> <dir>      Extract for development/debugging
//
// `pack` requires a valid manifest.json in the source directory and validates
// it before packing. The compile/run pipeline never needs `extract` — it
// reads RPR archives directly from memory (extract exists for development).
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <dirent.h>
#endif
#include <sys/stat.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "import/importer.hpp"
#include "import/json.hpp"
#include "import/manifest.hpp"
#include "import/rpr.hpp"

namespace {

bool isRegularFile(const std::string& path) {
    struct stat info;
    return ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool isDirectory(const std::string& path) {
    struct stat info;
    return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

std::string readWholeFile(const std::string& path, bool& ok) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        ok = false;
        return {};
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    if (file.bad()) {
        ok = false;
        return {};
    }
    ok = true;
    return buffer.str();
}

// Collects every regular file under `directory` with archive-style relative
// paths (sorted for determinism; the writer sorts again regardless).
void collectFiles(const std::string& directory, const std::string& prefix,
                  std::vector<std::pair<std::string, std::string>>& out) {
    std::vector<std::string> names;
#if defined(_WIN32)
    {
        WIN32_FIND_DATAA data;
        HANDLE handle = ::FindFirstFileA((directory + "/*").c_str(), &data);
        if (handle != INVALID_HANDLE_VALUE) {
            do {
                const std::string name = data.cFileName;
                if (name == "." || name == "..") continue;
                names.push_back(name);
            } while (::FindNextFileA(handle, &data));
            ::FindClose(handle);
        }
    }
#else
    {
        DIR* handle = ::opendir(directory.c_str());
        if (handle != nullptr) {
            while (dirent* entry = ::readdir(handle)) {
                const std::string name = entry->d_name;
                if (name == "." || name == "..") continue;
                names.push_back(name);
            }
            ::closedir(handle);
        }
    }
#endif
    std::sort(names.begin(), names.end());
    for (const std::string& name : names) {
        const std::string full = directory + "/" + name;
        struct stat info;
        if (::stat(full.c_str(), &info) != 0) continue;
        const std::string relative = prefix.empty() ? name : prefix + "/" + name;
        if (S_ISDIR(info.st_mode)) {
            collectFiles(full, relative, out);
        } else if (S_ISREG(info.st_mode)) {
            bool ok = false;
            const std::string content = readWholeFile(full, ok);
            if (ok) out.emplace_back(relative, content);
        }
    }
}

void printUsage(std::ostream& out) {
    out << "Usage:\n"
           "  rpr pack <directory> <output.rpr>    build an RPR package\n"
           "  rpr list <package.rpr>               list the package contents\n"
           "  rpr inspect <package.rpr>            show manifest + metadata\n"
           "  rpr extract <package.rpr> <dir>      extract for development\n";
}

// Validates a manifest with the schema that matches its declared type.
// Returns an error message ("" when valid).
std::string validateManifest(const std::string& manifestText) {
    runa::json::ParseError parseError;
    const auto root = runa::json::parse(manifestText, parseError);
    if (root == nullptr) {
        return "manifest.json is not valid JSON (" + parseError.message + " at " +
               std::to_string(parseError.line) + ":" + std::to_string(parseError.column) +
               ")";
    }
    try {
        const auto typeField = root->find("type");
        const std::string type =
            (typeField != nullptr && typeField->isString()) ? typeField->str : "";
        if (type == "native") {
            runa::importer::parseNativeManifest(manifestText);
        } else {
            runa::importer::parseLibraryManifest(manifestText);
        }
    } catch (const runa::importer::ManifestError& problem) {
        return problem.message;
    }
    return "";
}

int commandPack(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        std::cerr << "rpr: usage: rpr pack <directory> <output.rpr>\n";
        return 2;
    }
    const std::string& directory = args[0];
    const std::string& output = args[1];
    if (!isDirectory(directory)) {
        std::cerr << "rpr: not a directory: " << directory << "\n";
        return 1;
    }
    const std::string manifestPath = directory + "/manifest.json";
    if (!isRegularFile(manifestPath)) {
        std::cerr << "rpr: missing manifest.json in " << directory << "\n";
        return 1;
    }
    bool ok = false;
    const std::string manifestText = readWholeFile(manifestPath, ok);
    if (!ok) {
        std::cerr << "rpr: cannot read " << manifestPath << "\n";
        return 1;
    }
    const std::string problem = validateManifest(manifestText);
    if (!problem.empty()) {
        std::cerr << "rpr: invalid manifest.json: " << problem << "\n";
        return 1;
    }

    std::vector<std::pair<std::string, std::string>> files;
    collectFiles(directory, "", files);
    // manifest.json travels as the dedicated section, not a file entry.
    files.erase(std::remove_if(files.begin(), files.end(),
                               [](const auto& file) { return file.first == "manifest.json"; }),
                files.end());

    try {
        const std::string archive = runa::rpr::build(manifestText, files);
        std::ofstream out(output, std::ios::binary);
        if (!out) {
            std::cerr << "rpr: cannot write " << output << "\n";
            return 1;
        }
        out.write(archive.data(), static_cast<std::streamsize>(archive.size()));
        out.close();
        if (!out) {
            std::cerr << "rpr: failed while writing " << output << "\n";
            return 1;
        }
        std::cout << "rpr: packed " << files.size() << " file(s) from " << directory
                  << " into " << output << " (" << archive.size() << " bytes)\n";
    } catch (const runa::rpr::RprError& error) {
        std::cerr << "rpr: " << error.message << "\n";
        return 1;
    }
    return 0;
}

int commandList(const std::string& path) {
    bool ok = false;
    const std::string bytes = readWholeFile(path, ok);
    if (!ok) {
        std::cerr << "rpr: cannot open " << path << "\n";
        return 1;
    }
    try {
        const auto archive = runa::rpr::Archive::load(bytes);
        std::cout << "manifest.json  (special, " << archive.manifest().size()
                  << " bytes)\n";
        for (const runa::rpr::FileEntry& entry : archive.files()) {
            std::cout << entry.path << "  " << entry.size << " bytes\n";
        }
        std::cout << "(" << archive.files().size() << " file(s))\n";
    } catch (const runa::rpr::RprError& error) {
        std::cerr << "rpr: " << path << ": " << error.message << "\n";
        return 1;
    }
    return 0;
}

int commandInspect(const std::string& path) {
    bool ok = false;
    const std::string bytes = readWholeFile(path, ok);
    if (!ok) {
        std::cerr << "rpr: cannot open " << path << "\n";
        return 1;
    }
    try {
        const auto archive = runa::rpr::Archive::load(bytes);
        const std::string& manifestText = archive.manifest();
        runa::json::ParseError parseError;
        const auto root = runa::json::parse(manifestText, parseError);
        if (root == nullptr) {
            std::cerr << "rpr: " << path << ": manifest is not valid JSON ("
                      << parseError.message << ")\n";
            return 1;
        }
        std::uint64_t total = 0;
        for (const runa::rpr::FileEntry& entry : archive.files()) total += entry.size;

        std::cout << "package : " << path << "\n";
        std::cout << "format  : RPR version 1, " << archive.files().size()
                  << " file(s), " << total << " data bytes\n";
        const auto name = root->find("name");
        const auto version = root->find("version");
        const auto type = root->find("type");
        const auto entryField = root->find("entry");
        if (name != nullptr && name->isString()) std::cout << "name    : " << name->str << "\n";
        if (version != nullptr && version->isString())
            std::cout << "version : " << version->str << "\n";
        if (type != nullptr && type->isString()) std::cout << "type    : " << type->str << "\n";
        if (entryField != nullptr && entryField->isString())
            std::cout << "entry   : " << entryField->str << "\n";

        const auto native = root->find("native");
        if (native != nullptr && native->isObject()) {
            const auto targets = native->find("targets");
            if (targets != nullptr && targets->isObject()) {
                std::cout << "targets :";
                for (const auto& target : targets->objectEntries) {
                    std::cout << " " << target.first;
                }
                std::cout << "\n";
            }
            const auto api = native->find("api");
            if (api != nullptr && api->isObject()) {
                const auto functions = api->find("functions");
                if (functions != nullptr && functions->isArray()) {
                    std::cout << "api     : " << functions->array.size()
                              << " exported function(s)\n";
                    for (const auto& function : functions->array) {
                        if (!function->isObject()) continue;
                        const auto functionName = function->find("name");
                        const auto symbol = function->find("symbol");
                        if (functionName != nullptr && functionName->isString()) {
                            std::cout << "  - " << functionName->str;
                            if (symbol != nullptr && symbol->isString()) {
                                std::cout << " (symbol: " << symbol->str << ")";
                            }
                            std::cout << "\n";
                        }
                    }
                }
            }
        }
        std::cout << "files   :\n";
        for (const runa::rpr::FileEntry& file : archive.files()) {
            std::cout << "  " << file.path << "  " << file.size << " bytes\n";
        }
    } catch (const runa::rpr::RprError& error) {
        std::cerr << "rpr: " << path << ": " << error.message << "\n";
        return 1;
    }
    return 0;
}

int commandExtract(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        std::cerr << "rpr: usage: rpr extract <package.rpr> <directory>\n";
        return 2;
    }
    const std::string& path = args[0];
    const std::string& destination = args[1];
    bool ok = false;
    const std::string bytes = readWholeFile(path, ok);
    if (!ok) {
        std::cerr << "rpr: cannot open " << path << "\n";
        return 1;
    }
    try {
        const auto archive = runa::rpr::Archive::load(bytes);
        runa::importer::makeDirectories(destination);
        {
            std::ofstream manifestOut(destination + "/manifest.json", std::ios::binary);
            manifestOut.write(archive.manifest().data(),
                              static_cast<std::streamsize>(archive.manifest().size()));
            manifestOut.close();
            if (!manifestOut) {
                std::cerr << "rpr: cannot write " << destination << "/manifest.json\n";
                return 1;
            }
        }
        std::size_t written = 0;
        for (const runa::rpr::FileEntry& entry : archive.files()) {
            std::string content;
            if (!archive.readFile(entry.path, content)) continue;
            const std::string target = destination + "/" + entry.path;
            const std::size_t slash = target.rfind('/');
            if (slash != std::string::npos) {
                runa::importer::makeDirectories(target.substr(0, slash));
            }
            std::ofstream out(target, std::ios::binary);
            out.write(content.data(), static_cast<std::streamsize>(content.size()));
            out.close();
            if (!out) {
                std::cerr << "rpr: cannot write " << target << "\n";
                return 1;
            }
            ++written;
        }
        std::cout << "rpr: wrote manifest.json + " << written << " file(s) to "
                  << destination << "\n";
    } catch (const runa::rpr::RprError& error) {
        std::cerr << "rpr: " << path << ": " << error.message << "\n";
        return 1;
    }
    return 0;
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
    if (command == "pack") return commandPack(rest);
    if (command == "list") {
        if (rest.size() != 1) {
            std::cerr << "rpr: usage: rpr list <package.rpr>\n";
            return 2;
        }
        return commandList(rest[0]);
    }
    if (command == "inspect") {
        if (rest.size() != 1) {
            std::cerr << "rpr: usage: rpr inspect <package.rpr>\n";
            return 2;
        }
        return commandInspect(rest[0]);
    }
    if (command == "extract") return commandExtract(rest);
    std::cerr << "rpr: unknown command '" << command << "'\n";
    printUsage(std::cerr);
    return 2;
}
