// Runa — RPR (Runa Package Archive) format.
//
// RPR is the official package format of the Runa ecosystem: a self-describing,
// versioned binary container that the toolchain reads DIRECTLY FROM MEMORY —
// normal `import` never extracts a package to the filesystem.
//
// Container layout (all integers little-endian):
//
//     magic        : 4 bytes  'R' 'P' 'R' 'F'
//     version      : u32      (currently 1)
//     flags        : u32      (bit0 reserved for future compression; must be 0)
//     manifestSize : u32
//     manifest     : manifestSize bytes (the package's manifest.json content)
//     fileCount    : u32
//     per entry:
//         pathLength : u32
//         path       : pathLength bytes ('/'-separated, relative, validated)
//         offset     : u64 (absolute position of the file data in the archive)
//         size       : u64
//     file data    : concatenated payloads referenced by the table
//
// Validation is strict and total: bad magic, unsupported version, unknown
// flags, path traversal ("..", absolute paths, backslashes, empty segments),
// duplicate paths, offsets/sizes outside the archive, overlapping entries and
// truncated tables all produce a clean, precise error — a malformed archive
// can never crash the reader or corrupt memory.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace runa::rpr {

constexpr std::uint32_t FORMAT_VERSION = 1;
constexpr std::uint32_t FLAG_COMPRESSED = 0x1;  // reserved, not supported yet

struct FileEntry {
    std::string path;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

// Thrown for every malformed archive / invalid path. Message is user-facing.
struct RprError {
    std::string message;
};

// Validates a relative archive path. Valid: 'init.ru', 'native/backend/lib.so'.
// Invalid: absolute paths, '..', '.', backslashes, empty segments, NUL bytes.
// Returns the error text (empty string when the path is valid).
std::string validateArchivePath(const std::string& path);

// An archive held fully in memory. Copying is cheap enough for packages and
// keeps the reader free of lifetime hazards.
class Archive {
public:
    // An empty archive (no manifest, no files) — valid but useless until
    // load() replaces it. Exists so packages can be default-constructed.
    Archive() = default;

    // Parses and validates. Throws RprError on any problem.
    static Archive load(std::string bytes);

    const std::string& manifest() const { return manifest_; }
    const std::vector<FileEntry>& files() const { return files_; }

    // Reads one entry into `out`. Returns false when the path does not exist
    // (manifest.json is NOT part of the file table — use manifest()).
    bool readFile(const std::string& path, std::string& out) const;

private:
    std::string bytes_;
    std::string manifest_;
    std::vector<FileEntry> files_;
};

// Builds an archive from the given files (paths are validated; duplicates are
// rejected; entries are laid out in sorted path order for determinism).
// `manifest` becomes the dedicated manifest section.
std::string build(const std::string& manifest,
                  const std::vector<std::pair<std::string, std::string>>& files);

}  // namespace runa::rpr
