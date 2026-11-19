// runavm — the Runa virtual machine executable.
//
//     main.rbc  →  runavm  →  program execution
//
// The VM executes bytecode only; it never sees .ru source files
// (baseline spec, section 13).
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "bytecode/bytecode.hpp"
#include "errors/errors.hpp"
#include "runtime/runtime.hpp"
#include "tools/tool_common.hpp"
#include "vm/vm.hpp"
namespace {

void printUsage(std::ostream& out) {
    out << "Usage: runavm <file.rbc> [options]\n"
           "\n"
           "Executes a compiled Runa bytecode file.\n"
           "\n"
           "Options:\n"
           "  --trace          Print every executed instruction (development aid)\n"
           "  --dump-globals   Print global variables after execution (development aid)\n"
           "  -h, --help       Show this help\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string input;
    bool trace = false;
    bool dumpGlobals = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "-h" || arg == "--help") {
            printUsage(std::cout);
            return 0;
        } else if (arg == "--trace") {
            trace = true;
        } else if (arg == "--dump-globals") {
            dumpGlobals = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "runavm: unknown option '" << arg << "'\n";
            printUsage(std::cerr);
            return 2;
        } else if (input.empty()) {
            input = arg;
        } else {
            std::cerr << "runavm: unexpected extra argument '" << arg << "'\n";
            return 2;
        }
    }

    if (input.empty()) {
        printUsage(std::cerr);
        return 2;
    }

    std::string blob;
    if (!runa::tools::readFile(input, blob, "runavm")) return 2;

    runa::bytecode::Chunk chunk;
    try {
        std::istringstream in(blob);
        chunk = runa::bytecode::deserialize(in);
    } catch (const std::runtime_error& failure) {
        std::cerr << "runavm: " << failure.what() << '\n';
        return 1;
    }

    runa::vm::Options options;
    options.trace = trace;
    // Native packages resolve relative to the program artifact's directory.
    options.nativeRoot = runa::tools::dirnameOf(input);

    try {
        runa::vm::VirtualMachine vm(options);
        vm.run(chunk);
        if (dumpGlobals) {
            runa::runtime::printGlobals(vm.globals(), std::cout);
        }
    } catch (const runa::errors::RunaErrorException& failure) {
        runa::errors::report(failure.error());
        return 1;
    }
    return 0;
}
