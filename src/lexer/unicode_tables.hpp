// GENERATED range tables backing lexer/unicode_tables.cpp.
// isXidStart / isXidContinue implement Unicode XID_Start / XID_Continue
// (the same properties Python's str.isidentifier uses).
#pragma once

#include <cstdint>

namespace runa::lexer {
namespace unicode {

struct CodePointRange {
    std::uint32_t first;
    std::uint32_t last;
};

// Binary search over a sorted, merged range table.
inline bool inRanges(const CodePointRange* ranges, std::size_t count,
                     std::uint32_t codePoint) {
    std::size_t lo = 0;
    std::size_t hi = count;
    while (lo < hi) {
        std::size_t mid = lo + (hi - lo) / 2;
        if (codePoint < ranges[mid].first) {
            hi = mid;
        } else if (codePoint > ranges[mid].last) {
            lo = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

bool isXidStart(std::uint32_t codePoint);
bool isXidContinue(std::uint32_t codePoint);

}  // namespace unicode
}  // namespace runa::lexer
