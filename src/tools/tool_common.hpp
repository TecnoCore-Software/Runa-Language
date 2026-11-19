// Small helpers shared by the Runa command-line tools.
// (Kept tiny on purpose so each tool's main stays readable.)
#pragma once

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace runa::tools {

// Reads a whole file into a string. Returns false and prints a tool-prefixed
// message if the file cannot be opened or read.
inline bool readFile(const std::string& path, std::string& out, const char* tool = "runa") {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << tool << ": cannot open file '" << path << "'\n"
                  << tool << ": check that the path is correct and the file exists\n";
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    if (file.bad()) {
        std::cerr << tool << ": error while reading '" << path << "'\n";
        return false;
    }
    out = buffer.str();
    return true;
}

// 'main.ru'  -> 'main.rbc'; anything else gets '.rbc' appended.
inline std::string defaultOutputPath(const std::string& input) {
    const std::string suffix = ".ru";
    if (input.size() >= suffix.size() &&
        input.compare(input.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return input.substr(0, input.size() - suffix.size()) + ".rbc";
    }
    return input + ".rbc";
}

// The directory part of a path ('' when there is no separator). Used to anchor
// the package search paths and the native root to the program's real location.
inline std::string dirnameOf(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

}  // namespace runa::tools
