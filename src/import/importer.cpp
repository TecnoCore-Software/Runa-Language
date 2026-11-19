#include "import/importer.hpp"

#include <sys/stat.h>

#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace runa::importer {

void failAt(const errors::SourceLocation& at, const std::string& message) {
    errors::RunaError error;
    error.kind = errors::ErrorKind::CompileError;
    error.location = at;
    error.message = message;
    throw errors::RunaErrorException(error);
}

namespace {

std::string directoryOf(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

std::string joinPath(const std::string& directory, const std::string& name) {
    if (directory.empty()) return name;
    if (directory.back() == '/' || directory.back() == '\\') return directory + name;
    return directory + "/" + name;
}

bool fileExists(const std::string& path) {
    struct stat info;
    return ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool directoryExists(const std::string& path) {
    struct stat info;
    return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

std::string readFileOrEmpty(const std::string& path, bool& ok) {
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

}  // namespace

ImportResolver::ImportResolver(std::vector<std::string> searchPaths)
    : searchPaths_(std::move(searchPaths)) {}

LoadedPackage ImportResolver::loadPackage(const std::string& name,
                                          const errors::SourceLocation& at) {
    // Search paths in order; first match wins (documented precedence).
    std::string foundPath;
    for (const std::string& directory : searchPaths_) {
        const std::string candidate = joinPath(directory, name + ".rpr");
        if (fileExists(candidate)) {
            foundPath = candidate;
            break;
        }
    }
    if (foundPath.empty()) {
        std::string searched;
        for (std::size_t i = 0; i < searchPaths_.size(); ++i) {
            searched += std::string(i == 0 ? "" : ", ") + searchPaths_[i];
        }
        if (searched.empty()) searched = "(no search paths available)";
        failAt(at, "package not found: '" + name + "' (searched: " + searched + ")");
    }

    bool ok = false;
    const std::string bytes = readFileOrEmpty(foundPath, ok);
    if (!ok) {
        failAt(at, "cannot read package file '" + foundPath + "'");
    }

    rpr::Archive archive;
    try {
        archive = rpr::Archive::load(bytes);
    } catch (const rpr::RprError& problem) {
        failAt(at, "invalid RPR archive '" + foundPath + "': " + problem.message);
    }

    LibraryManifest manifest;
    try {
        manifest = parseLibraryManifest(archive.manifest());
    } catch (const ManifestError& problem) {
        failAt(at, "invalid manifest in '" + foundPath + "': " + problem.message);
    }
    if (manifest.name != name) {
        failAt(at, "package name mismatch: '" + foundPath + "' declares \"" +
                       manifest.name + "\" but was imported as '" + name +
                       "' (the manifest name and the file name must match)");
    }

    LoadedPackage package;
    package.name = manifest.name;
    package.archivePath = foundPath;
    package.manifest = std::move(manifest);
    package.archive = std::move(archive);
    return package;
}

LoadedNativePackage ImportResolver::loadProjectNativePackage(
    const std::string& path, const std::string& projectRoot,
    const errors::SourceLocation& at) {
    const std::string packageDir = joinPath(projectRoot, path);
    const std::string manifestPath = joinPath(packageDir, "manifest.json");
    if (!directoryExists(packageDir)) {
        failAt(at, "native package not found: '" + path +
                       "' (expected the directory " + packageDir + ")");
    }
    if (!fileExists(manifestPath)) {
        failAt(at, "native manifest not found: '" + path + "/manifest.json' (expected " +
                       manifestPath + ")");
    }
    bool ok = false;
    const std::string manifestText = readFileOrEmpty(manifestPath, ok);
    if (!ok) failAt(at, "cannot read native manifest '" + manifestPath + "'");

    LoadedNativePackage package;
    package.cimportPath = path;
    package.packageDir = path;
    package.diskRoot = packageDir;
    try {
        package.manifest = parseNativeManifest(manifestText);
    } catch (const ManifestError& problem) {
        failAt(at, "invalid native manifest '" + manifestPath + "': " + problem.message);
    }
    return package;
}

LoadedNativePackage ImportResolver::loadEmbeddedNativePackage(
    const std::string& path, const LoadedPackage& package,
    const errors::SourceLocation& at) {
    const std::string manifestEntry = path + "/manifest.json";
    std::string manifestText;
    if (!package.archive.readFile(manifestEntry, manifestText)) {
        failAt(at, "native package not found: '" + path +
                       "' inside " + package.archivePath +
                       " (expected the entry " + manifestEntry + ")");
    }

    LoadedNativePackage native;
    native.cimportPath = path;
    native.packageDir = path;
    native.embeddedArchive = &package.archive;
    try {
        native.manifest = parseNativeManifest(manifestText);
    } catch (const ManifestError& problem) {
        failAt(at, "invalid native manifest '" + package.archivePath + "/" +
                       manifestEntry + "': " + problem.message);
    }
    return native;
}

void makeDirectories(const std::string& path) {
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

std::size_t extractArchivePrefix(const rpr::Archive& archive, const std::string& prefix,
                                 const std::string& destination) {
    // Normalize the prefix (no leading/trailing slash) for entry matching.
    std::string normalized = prefix;
    while (!normalized.empty() && normalized.front() == '/') normalized.erase(0, 1);
    while (!normalized.empty() && normalized.back() == '/') normalized.pop_back();
    const std::string prefixWithSlash = normalized.empty() ? "" : normalized + "/";

    std::size_t written = 0;
    for (const rpr::FileEntry& entry : archive.files()) {
        if (entry.path.rfind(prefixWithSlash, 0) != 0) continue;
        const std::string relative = entry.path.substr(prefixWithSlash.size());
        if (relative.empty()) continue;
        const std::string target = joinPath(destination, relative);
        // Skip existing files: disk state wins, extraction is idempotent.
        if (fileExists(target)) continue;
        std::string content;
        if (!archive.readFile(entry.path, content)) continue;
        makeDirectories(directoryOf(target));
        std::ofstream out(target, std::ios::binary);
        if (!out) {
            failAt(errors::SourceLocation{},
                   "cannot write native package file '" + target + "'");
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out) {
            failAt(errors::SourceLocation{},
                   "failed while writing native package file '" + target + "'");
        }
        ++written;
    }
    return written;
}

}  // namespace runa::importer
