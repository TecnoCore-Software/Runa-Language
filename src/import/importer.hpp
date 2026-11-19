// Runa — package importer (resolver + loaders).
//
// The importer owns every place a package can come from:
//
//   Normal import (`import math` / `as "math" import {..}`):
//       <name>.rpr located in the search paths, in order:
//           1. <project root>/runalibs
//           2. <executable dir>/runalibs/internal
//           3. <executable dir>/runalibs/external
//       First match wins (documented precedence). The archive is read fully
//       into memory — normal import NEVER extracts anything.
//
//   cimport (native packages), two contexts:
//       - project context:  <project root>/<cimport path>/manifest.json on disk
//       - package context:  <cimport path>/manifest.json INSIDE the importing
//                           package's RPR (package-relative by definition)
//
//   Native binary placement at run time: <native root>/<package dir>/<target>.
//   A native package embedded in an RPR must be extracted to the native root
//   before it can be dlopen'd (dlopen requires a real file); the COMPILER does
//   that once, skipping existing files. Runa code is never extracted.
#pragma once

#include <string>
#include <vector>

#include "errors/errors.hpp"
#include "import/manifest.hpp"
#include "import/rpr.hpp"

namespace runa::importer {

struct LoadedPackage {
    std::string name;           // package name (manifest "name" == file stem)
    std::string archivePath;    // where the .rpr came from (for messages)
    LibraryManifest manifest;
    rpr::Archive archive;       // keeps the package bytes in memory
};

// A native package resolved in one of the two contexts.
struct LoadedNativePackage {
    std::string cimportPath;    // exactly as written in the source
    std::string packageDir;     // relative dir for the natives table: "native/<name>"
    std::string name;           // manifest "name" (== namespace binding)
    NativeManifest manifest;

    // Project context: binaries live under this filesystem directory.
    std::string diskRoot;       // empty when embedded
    // Package context: binaries live inside this archive under packageDir.
    const rpr::Archive* embeddedArchive = nullptr;
};

class ImportResolver {
public:
    explicit ImportResolver(std::vector<std::string> searchPaths);

    // Loads `<name>.rpr` from the search paths (first match wins). Throws a
    // structured CompileError located at `at` for every failure mode: not
    // found, unreadable, invalid RPR, invalid manifest, name mismatch.
    LoadedPackage loadPackage(const std::string& name,
                              const errors::SourceLocation& at);

    // Project-context cimport: `<projectRoot>/<path>/manifest.json` on disk.
    LoadedNativePackage loadProjectNativePackage(const std::string& path,
                                                 const std::string& projectRoot,
                                                 const errors::SourceLocation& at);

    // Package-context cimport: `<path>/manifest.json` inside `package`.
    LoadedNativePackage loadEmbeddedNativePackage(const std::string& path,
                                                  const LoadedPackage& package,
                                                  const errors::SourceLocation& at);

    const std::vector<std::string>& searchPaths() const { return searchPaths_; }

private:
    std::vector<std::string> searchPaths_;
};

// Throws a structured CompileError located at `at`.
[[noreturn]] void failAt(const errors::SourceLocation& at, const std::string& message);

// Extracts every archive entry under `prefix` into `destination` (creating
// directories as needed). Existing files are left untouched (disk wins) —
// the call is idempotent. Returns the number of files written.
std::size_t extractArchivePrefix(const rpr::Archive& archive, const std::string& prefix,
                                 const std::string& destination);

// Creates all directories of a filesystem path (like mkdir -p).
void makeDirectories(const std::string& path);

}  // namespace runa::importer
