// Runa — RPR (Runa Package Archive) format, Runa 1.0.
//
// RPR is the official package format of the Runa ecosystem (spec 1.0 section
// 91 + DECISIONS #119): a ZIP-based container holding
//
//     manifest.json   (the package manifest — required)
//     init.ru         (the entry source referenced by the manifest)
//     optional further source / native files
//
// The toolchain reads packages DIRECTLY FROM MEMORY — normal `import` never
// extracts a package to the filesystem. The writer emits deterministic,
// uncompressed ("stored") ZIP archives: byte-identical output for identical
// inputs, readable by every standard ZIP tool. Deflate entries are rejected
// with a clean, precise error.
//
// The pre-1.0 binary container (magic "RPRF") is REJECTED with an explicit
// message and there is NO migration (DECISIONS #119).
//
// Validation is strict and total: bad signatures, truncated structures,
// unsupported compression methods, path traversal ("..", absolute paths,
// backslashes, empty segments), duplicate paths, CRC mismatches and missing
// manifests all produce a clean error — a malformed archive can never crash
// the reader or corrupt memory.
#include "import/rpr.hpp"

#include <algorithm>
#include <cstring>

namespace runa::rpr {

namespace {

constexpr char kZipLocalSignature[4] = {'P', 'K', 0x03, 0x04};
constexpr char kZipCentralSignature[4] = {'P', 'K', 0x01, 0x02};
constexpr char kZipEocdSignature[4] = {'P', 'K', 0x05, 0x06};
constexpr char kLegacyMagic[4] = {'R', 'P', 'R', 'F'};

constexpr std::uint16_t kVersionNeeded = 20;  // 2.0 — covers stored entries

std::uint16_t readU16(const std::string& bytes, std::size_t at) {
    return static_cast<std::uint8_t>(bytes[at]) |
           (static_cast<std::uint16_t>(static_cast<std::uint8_t>(bytes[at + 1])) << 8);
}

std::uint32_t readU32(const std::string& bytes, std::size_t at) {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[at])) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[at + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[at + 2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[at + 3])) << 24);
}

void writeU16(std::string& out, std::uint16_t value) {
    out += static_cast<char>(value & 0xFF);
    out += static_cast<char>((value >> 8) & 0xFF);
}

void writeU32(std::string& out, std::uint32_t value) {
    out += static_cast<char>(value & 0xFF);
    out += static_cast<char>((value >> 8) & 0xFF);
    out += static_cast<char>((value >> 16) & 0xFF);
    out += static_cast<char>((value >> 24) & 0xFF);
}

// Standard CRC-32 (IEEE 802.3, polynomial 0xEDB88320) — the ZIP checksum.
std::uint32_t crc32(const std::string& data) {
    static std::uint32_t table[256];
    static bool tableReady = false;
    if (!tableReady) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t crc = i;
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc & 1) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
            }
            table[i] = crc;
        }
        tableReady = true;
    }
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const char c : data) {
        crc = table[(crc ^ static_cast<std::uint8_t>(c)) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

struct ZipEntry {
    std::string path;
    std::string data;
};

void appendStoredEntry(std::string& out, const ZipEntry& entry,
                       std::uint32_t& headerOffsetOut, std::string& centralOut) {
    const std::uint32_t crc = crc32(entry.data);
    const std::uint32_t size = static_cast<std::uint32_t>(entry.data.size());
    const std::uint32_t headerOffset = static_cast<std::uint32_t>(out.size());
    headerOffsetOut = headerOffset;

    // Local file header.
    out += std::string(kZipLocalSignature, 4);
    writeU16(out, kVersionNeeded);
    writeU16(out, 0);   // flags
    writeU16(out, 0);   // method: stored
    writeU16(out, 0);   // mod time (fixed: deterministic archives)
    writeU16(out, 0x21);  // mod date 1980-01-01 (fixed: deterministic archives)
    writeU32(out, crc);
    writeU32(out, size);  // compressed size
    writeU32(out, size);  // uncompressed size
    writeU16(out, static_cast<std::uint16_t>(entry.path.size()));
    writeU16(out, 0);  // extra length
    out += entry.path;
    out += entry.data;

    // Central directory record.
    centralOut += std::string(kZipCentralSignature, 4);
    writeU16(centralOut, kVersionNeeded);  // version made by
    writeU16(centralOut, kVersionNeeded);  // version needed
    writeU16(centralOut, 0);   // flags
    writeU16(centralOut, 0);   // method: stored
    writeU16(centralOut, 0);   // mod time
    writeU16(centralOut, 0x21);  // mod date
    writeU32(centralOut, crc);
    writeU32(centralOut, size);
    writeU32(centralOut, size);
    writeU16(centralOut, static_cast<std::uint16_t>(entry.path.size()));
    writeU16(centralOut, 0);  // extra length
    writeU16(centralOut, 0);  // comment length
    writeU16(centralOut, 0);  // disk number start
    writeU16(centralOut, 0);  // internal attributes
    writeU32(centralOut, 0);  // external attributes
    writeU32(centralOut, headerOffset);
    centralOut += entry.path;
}

[[noreturn]] void fail(const std::string& message) {
    throw RprError{message};
}

}  // namespace

std::string validateArchivePath(const std::string& path) {
    if (path.empty()) return "empty archive path";
    if (path.size() > 4096) return "archive path too long";
    if (path.front() == '/') return "absolute archive path '" + path + "'";
    if (path.find('\\') != std::string::npos) {
        return "backslash in archive path '" + path + "' (use '/')'";
    }
    if (path.find('\0') != std::string::npos) {
        return "NUL byte in archive path '" + path + "'";
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        const std::size_t segmentLength = end - start;
        if (segmentLength == 0) {
            return "empty segment in archive path '" + path + "'";
        }
        const std::string segment = path.substr(start, segmentLength);
        if (segment == "." || segment == "..") {
            return "path traversal in archive path '" + path + "'";
        }
        if (end == path.size()) break;
        start = end + 1;
    }
    return "";
}

std::string build(const std::string& manifest,
                  const std::vector<std::pair<std::string, std::string>>& files) {
    // Sorted path order keeps archives deterministic; duplicates are a
    // builder-side bug and are rejected loudly.
    std::vector<ZipEntry> entries;
    entries.reserve(files.size() + 1);
    entries.push_back({"manifest.json", manifest});
    for (const auto& file : files) {
        const std::string problem = validateArchivePath(file.first);
        if (!problem.empty()) fail(problem);
        for (const ZipEntry& existing : entries) {
            if (existing.path == file.first) {
                fail("duplicate archive path '" + file.first + "'");
            }
        }
        entries.push_back({file.first, file.second});
    }
    std::sort(entries.begin(), entries.end(),
              [](const ZipEntry& a, const ZipEntry& b) { return a.path < b.path; });

    std::string out;
    std::string central;
    for (const ZipEntry& entry : entries) {
        std::uint32_t headerOffset = 0;
        appendStoredEntry(out, entry, headerOffset, central);
    }
    const std::uint32_t centralOffset = static_cast<std::uint32_t>(out.size());
    out += central;
    // End of central directory.
    out += std::string(kZipEocdSignature, 4);
    writeU16(out, 0);  // this disk
    writeU16(out, 0);  // disk with central directory
    writeU16(out, static_cast<std::uint16_t>(entries.size()));
    writeU16(out, static_cast<std::uint16_t>(entries.size()));
    writeU32(out, static_cast<std::uint32_t>(central.size()));
    writeU32(out, centralOffset);
    writeU16(out, 0);  // comment length
    return out;
}

Archive Archive::load(std::string bytes) {
    Archive archive;
    archive.bytes_ = std::move(bytes);
    const std::string& data = archive.bytes_;

    if (data.size() >= 4 && std::memcmp(data.data(), kLegacyMagic, 4) == 0) {
        fail("old RPRF container format is not supported in Runa 1.0 "
             "(rebuild the package with 'rpr pack')");
    }
    if (data.size() < 22) fail("archive too small to be a ZIP container");

    // ---- locate the end-of-central-directory record ------------------------
    std::size_t eocd = std::string::npos;
    const std::size_t scanFloor = data.size() >= 65557 ? data.size() - 65557 : 0;
    for (std::size_t at = data.size() - 22 + 1; at-- > scanFloor;) {
        if (std::memcmp(data.data() + at, kZipEocdSignature, 4) == 0) {
            const std::uint16_t commentLength = readU16(data, at + 20);
            if (at + 22 + commentLength == data.size()) {
                eocd = at;  // comment length accounts for exactly the tail
                break;
            }
        }
    }
    if (eocd == std::string::npos) {
        fail("not a ZIP container (no end-of-central-directory record)");
    }
    const std::uint16_t entryCount = readU16(data, eocd + 10);
    const std::uint32_t centralSize = readU32(data, eocd + 12);
    const std::uint32_t centralOffset = readU32(data, eocd + 16);
    if (centralOffset > data.size() ||
        centralSize > data.size() - centralOffset) {
        fail("corrupt ZIP container (central directory outside the archive)");
    }
    if (static_cast<std::size_t>(entryCount) > 1u << 20) {
        fail("corrupt ZIP container (implausible entry count)");
    }

    // ---- central directory entries ----------------------------------------
    std::size_t cursor = centralOffset;
    std::size_t manifestSize = 0;
    const std::size_t centralEnd = centralOffset + centralSize;
    for (std::uint16_t index = 0; index < entryCount; ++index) {
        if (cursor + 46 > centralEnd ||
            std::memcmp(data.data() + cursor, kZipCentralSignature, 4) != 0) {
            fail("corrupt ZIP container (bad central directory entry)");
        }
        const std::uint16_t method = readU16(data, cursor + 10);
        const std::uint32_t crc = readU32(data, cursor + 16);
        const std::uint32_t compressedSize = readU32(data, cursor + 20);
        const std::uint32_t uncompressedSize = readU32(data, cursor + 24);
        const std::uint16_t nameLength = readU16(data, cursor + 28);
        const std::uint32_t localOffset = readU32(data, cursor + 42);
        if (cursor + 46 + nameLength > centralEnd) {
            fail("corrupt ZIP container (truncated entry name)");
        }
        const std::string name = data.substr(cursor + 46, nameLength);
        cursor += 46 + nameLength;

        const std::string problem = validateArchivePath(name);
        if (!problem.empty()) fail(problem);
        if (method != 0) {
            fail("unsupported ZIP compression method " + std::to_string(method) +
                 " for '" + name + "' (only stored entries are supported)");
        }
        if (compressedSize != uncompressedSize) {
            fail("corrupt ZIP container ('" + name +
                 "' claims inconsistent sizes for a stored entry)");
        }
        if (localOffset + 30 > data.size()) {
            fail("corrupt ZIP container ('" + name +
                 "' points outside the archive)");
        }
        if (std::memcmp(data.data() + localOffset, kZipLocalSignature, 4) != 0) {
            fail("corrupt ZIP container (bad local header for '" + name + "')");
        }
        const std::uint16_t localNameLength = readU16(data, localOffset + 26);
        const std::uint16_t localExtraLength = readU16(data, localOffset + 28);
        const std::size_t dataStart =
            localOffset + 30 + localNameLength + localExtraLength;
        if (dataStart + uncompressedSize > data.size()) {
            fail("corrupt ZIP container ('" + name +
                 "' data runs outside the archive)");
        }
        std::string content = data.substr(dataStart, uncompressedSize);
        if (crc32(content) != crc) {
            fail("ZIP entry '" + name + "' fails its CRC-32 check");
        }

        if (name == "manifest.json") {
            archive.manifest_ = std::move(content);
            manifestSize = uncompressedSize;
            continue;
        }
        for (const FileEntry& existing : archive.files_) {
            if (existing.path == name) {
                fail("duplicate archive path '" + name + "'");
            }
        }
        FileEntry entry;
        entry.path = name;
        entry.offset = dataStart;
        entry.size = uncompressedSize;
        archive.files_.push_back(std::move(entry));
    }
    (void)manifestSize;
    if (archive.manifest_.empty() && manifestSize == 0) {
        fail("package has no manifest.json entry");
    }
    return archive;
}

bool Archive::readFile(const std::string& path, std::string& out) const {
    for (const FileEntry& entry : files_) {
        if (entry.path == path) {
            out = bytes_.substr(static_cast<std::size_t>(entry.offset),
                                static_cast<std::size_t>(entry.size));
            return true;
        }
    }
    return false;
}

}  // namespace runa::rpr
