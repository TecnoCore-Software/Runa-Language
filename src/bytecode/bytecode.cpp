#include "bytecode/bytecode.hpp"

#include <cstring>
#include <stdexcept>

#include "runtime/runtime.hpp"  // formatFloatDisplay (constant disassembly)

namespace runa::bytecode {

const char* opCodeName(OpCode op) {
    switch (op) {
        case OpCode::LoadConst:   return "LoadConst";
        case OpCode::LoadGlobal:  return "LoadGlobal";
        case OpCode::StoreGlobal: return "StoreGlobal";
        case OpCode::Call:        return "Call";
        case OpCode::Add:         return "Add";
        case OpCode::Sub:         return "Sub";
        case OpCode::Mul:         return "Mul";
        case OpCode::Div:         return "Div";
        case OpCode::Mod:         return "Mod";
        case OpCode::Halt:        return "Halt";
        case OpCode::Eq:          return "Eq";
        case OpCode::Ne:          return "Ne";
        case OpCode::Gt:          return "Gt";
        case OpCode::Lt:          return "Lt";
        case OpCode::Ge:          return "Ge";
        case OpCode::Le:          return "Le";
        case OpCode::JumpIfFalse: return "JumpIfFalse";
        case OpCode::Jump:        return "Jump";
        case OpCode::ReturnValue: return "ReturnValue";
        case OpCode::ReturnNone:  return "ReturnNone";
        case OpCode::Pop:         return "Pop";
        case OpCode::LoadVar:     return "LoadVar";
        case OpCode::StoreLocal:  return "StoreLocal";
        case OpCode::NewObject:   return "NewObject";
        case OpCode::GetField:    return "GetField";
        case OpCode::SetField:    return "SetField";
        case OpCode::InitField:   return "InitField";
        case OpCode::GetSelf:     return "GetSelf";
        case OpCode::InvokeVirtual: return "InvokeVirtual";
        case OpCode::InvokeStatic:  return "InvokeStatic";
        case OpCode::InvokeSuper:   return "InvokeSuper";
        case OpCode::GetStatic:   return "GetStatic";
        case OpCode::StoreStatic: return "StoreStatic";
        case OpCode::IsType:      return "IsType";
        case OpCode::EqIdent:     return "EqIdent";
        case OpCode::CallNative:  return "CallNative";
        case OpCode::BuildCollection: return "BuildCollection";
        case OpCode::Negate:      return "Negate";
        case OpCode::Not:         return "Not";
        case OpCode::And:         return "And";
        case OpCode::Or:          return "Or";
        case OpCode::JumpIfFalseKeep: return "JumpIfFalseKeep";
        case OpCode::JumpIfTrueKeep:  return "JumpIfTrueKeep";
    }
    return "<invalid>";
}

std::string Constant::describe() const {
    switch (kind) {
        case Kind::String:  return '"' + str + '"';
        case Kind::Number:  return std::to_string(number);
        case Kind::Boolean: return boolean ? "true" : "false";
        case Kind::Null:    return "null";
        case Kind::Float:   return runtime::formatFloatDisplay(floating);
    }
    return "?";
}

std::uint32_t Chunk::addStringConstant(const std::string& value) {
    Constant constant;
    constant.kind = Constant::Kind::String;
    constant.str = value;
    constants.push_back(std::move(constant));
    return static_cast<std::uint32_t>(constants.size() - 1);
}

std::uint32_t Chunk::addNumberConstant(std::int64_t value) {
    Constant constant;
    constant.kind = Constant::Kind::Number;
    constant.number = value;
    constants.push_back(std::move(constant));
    return static_cast<std::uint32_t>(constants.size() - 1);
}

std::uint32_t Chunk::addFloatConstant(double value) {
    Constant constant;
    constant.kind = Constant::Kind::Float;
    constant.floating = value;
    constants.push_back(std::move(constant));
    return static_cast<std::uint32_t>(constants.size() - 1);
}

std::uint32_t Chunk::addBooleanConstant(bool value) {
    Constant constant;
    constant.kind = Constant::Kind::Boolean;
    constant.boolean = value;
    constants.push_back(std::move(constant));
    return static_cast<std::uint32_t>(constants.size() - 1);
}

std::uint32_t Chunk::addName(std::string name) {
    // Deduplicate: the same identifier (e.g. a variable read and written)
    // should reference a single pool entry.
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == name) return static_cast<std::uint32_t>(i);
    }
    names.push_back(std::move(name));
    return static_cast<std::uint32_t>(names.size() - 1);
}

void Chunk::writeOp(OpCode op) {
    code.push_back(static_cast<std::uint8_t>(op));
}

void Chunk::writeU32(std::uint32_t value) {
    code.push_back(static_cast<std::uint8_t>(value & 0xFF));
    code.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    code.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    code.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

void Chunk::patchU32(std::size_t offset, std::uint32_t value) {
    code[offset]     = static_cast<std::uint8_t>(value & 0xFF);
    code[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    code[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    code[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
}

// ---- disassembly (development aid) -----------------------------------------

namespace {

std::string escapeForDump(const std::string& text) {
    std::string out;
    for (char c : text) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            case '\r': out += "\\r";  break;
            default:   out += c;
        }
    }
    return out;
}

std::string padRight(const std::string& text, std::size_t width) {
    if (text.size() >= width) return text;
    return text + std::string(width - text.size(), ' ');
}

std::uint32_t readU32At(const Chunk& chunk, std::size_t offset) {
    return static_cast<std::uint32_t>(chunk.code[offset]) |
           (static_cast<std::uint32_t>(chunk.code[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(chunk.code[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(chunk.code[offset + 3]) << 24);
}

// Renders one constant for disassembly (escape only the string payload).
std::string describeConstantForDump(const Constant& constant) {
    if (constant.kind == Constant::Kind::String) {
        return '"' + escapeForDump(constant.str) + '"';
    }
    return constant.describe();
}

}  // namespace

std::size_t formatInstructionAt(const Chunk& chunk, std::size_t offset, std::ostream& out) {
    if (offset >= chunk.code.size()) {
        out << "<offset out of range>";
        return offset;
    }
    OpCode op = static_cast<OpCode>(chunk.code[offset]);
    switch (op) {
        case OpCode::LoadConst: {
            if (offset + 5 > chunk.code.size()) {
                out << "LoadConst <truncated operand>";
                return chunk.code.size();
            }
            std::uint32_t index = readU32At(chunk, offset + 1);
            out << padRight("LoadConst", 12) << index << "  ; ";
            if (index < chunk.constants.size()) {
                out << describeConstantForDump(chunk.constants[index]);
            } else {
                out << "<missing constant>";
            }
            return offset + 5;
        }
        case OpCode::LoadGlobal:
        case OpCode::StoreGlobal: {
            if (offset + 5 > chunk.code.size()) {
                out << opCodeName(op) << " <truncated operand>";
                return chunk.code.size();
            }
            std::uint32_t index = readU32At(chunk, offset + 1);
            out << padRight(opCodeName(op), 12) << index << "  ; ";
            if (index < chunk.names.size()) {
                out << chunk.names[index];
            } else {
                out << "<missing name>";
            }
            return offset + 5;
        }
        case OpCode::Call: {
            if (offset + 9 > chunk.code.size()) {
                out << "Call <truncated operands>";
                return chunk.code.size();
            }
            std::uint32_t nameIndex = readU32At(chunk, offset + 1);
            std::uint32_t argc = readU32At(chunk, offset + 5);
            out << padRight("Call", 12) << argc << "  ; ";
            if (nameIndex < chunk.names.size()) {
                out << chunk.names[nameIndex] << " (argc=" << argc << ")";
            } else {
                out << "<missing name> (argc=" << argc << ")";
            }
            return offset + 9;
        }
        case OpCode::Add:
        case OpCode::Sub:
        case OpCode::Mul:
        case OpCode::Div:
        case OpCode::Mod:
        case OpCode::Eq:
        case OpCode::Ne:
        case OpCode::Gt:
        case OpCode::Lt:
        case OpCode::Ge:
        case OpCode::Le:
        case OpCode::Halt:
            // Operand-free single-byte instructions.
            out << opCodeName(op);
            return offset + 1;
        case OpCode::JumpIfFalse:
        case OpCode::JumpIfFalseKeep:
        case OpCode::JumpIfTrueKeep:
        case OpCode::Jump: {
            if (offset + 5 > chunk.code.size()) {
                out << opCodeName(op) << " <truncated operand>";
                return chunk.code.size();
            }
            std::uint32_t target = readU32At(chunk, offset + 1);
            out << padRight(opCodeName(op), 12) << target;
            return offset + 5;
        }
        case OpCode::LoadVar:
        case OpCode::StoreLocal: {
            if (offset + 5 > chunk.code.size()) {
                out << opCodeName(op) << " <truncated operand>";
                return chunk.code.size();
            }
            std::uint32_t index = readU32At(chunk, offset + 1);
            out << padRight(opCodeName(op), 12) << index << "  ; ";
            if (index < chunk.names.size()) {
                out << chunk.names[index];
            } else {
                out << "<missing name>";
            }
            return offset + 5;
        }
        case OpCode::ReturnValue:
        case OpCode::ReturnNone:
        case OpCode::Pop:
        case OpCode::GetSelf:
        case OpCode::EqIdent:
        case OpCode::Negate:
        case OpCode::Not:
        case OpCode::And:
        case OpCode::Or:
            // Operand-free single-byte instructions.
            out << opCodeName(op);
            return offset + 1;
        case OpCode::CallNative: {
            if (offset + 9 > chunk.code.size()) {
                out << "CallNative <truncated operands>";
                return chunk.code.size();
            }
            std::uint32_t nativeIndex = readU32At(chunk, offset + 1);
            std::uint32_t argc = readU32At(chunk, offset + 5);
            out << padRight("CallNative", 12) << argc << "  ; ";
            if (nativeIndex < chunk.nativeFunctions.size()) {
                const NativeFunctionInfo& fn = chunk.nativeFunctions[nativeIndex];
                out << fn.namespaceName << "." << fn.name << " (argc=" << argc << ")"
                    << " [" << fn.symbol << " @ " << fn.packageDir << "]";
            } else {
                out << "<missing native function> (argc=" << argc << ")";
            }
            return offset + 9;
        }
        case OpCode::BuildCollection: {
            if (offset + 5 > chunk.code.size()) {
                out << "BuildCollection <truncated operand>";
                return chunk.code.size();
            }
            std::uint32_t count = readU32At(chunk, offset + 1);
            out << padRight("BuildCollection", 12) << count << "  ; element count";
            return offset + 5;
        }
        case OpCode::NewObject: {
            if (offset + 9 > chunk.code.size()) {
                out << "NewObject <truncated operands>";
                return chunk.code.size();
            }
            std::uint32_t classIndex = readU32At(chunk, offset + 1);
            std::uint32_t argc = readU32At(chunk, offset + 5);
            out << padRight("NewObject", 12) << argc << "  ; ";
            if (classIndex < chunk.classes.size()) {
                out << chunk.classes[classIndex].name << " (argc=" << argc << ")";
            } else {
                out << "<missing class> (argc=" << argc << ")";
            }
            return offset + 9;
        }
        case OpCode::GetField:
        case OpCode::SetField:
        case OpCode::InitField: {
            if (offset + 5 > chunk.code.size()) {
                out << opCodeName(op) << " <truncated operand>";
                return chunk.code.size();
            }
            std::uint32_t index = readU32At(chunk, offset + 1);
            out << padRight(opCodeName(op), 12) << index << "  ; ";
            if (index < chunk.names.size()) {
                out << chunk.names[index];
            } else {
                out << "<missing name>";
            }
            return offset + 5;
        }
        case OpCode::InvokeVirtual:
        case OpCode::InvokeSuper: {
            if (offset + 9 > chunk.code.size()) {
                out << opCodeName(op) << " <truncated operands>";
                return chunk.code.size();
            }
            std::uint32_t nameIndex = readU32At(chunk, offset + 1);
            std::uint32_t argc = readU32At(chunk, offset + 5);
            out << padRight(opCodeName(op), 12) << argc << "  ; ";
            if (nameIndex < chunk.names.size()) {
                out << chunk.names[nameIndex] << " (argc=" << argc << ")";
            } else {
                out << "<missing name> (argc=" << argc << ")";
            }
            return offset + 9;
        }
        case OpCode::InvokeStatic: {
            if (offset + 13 > chunk.code.size()) {
                out << "InvokeStatic <truncated operands>";
                return chunk.code.size();
            }
            std::uint32_t classIndex = readU32At(chunk, offset + 1);
            std::uint32_t nameIndex = readU32At(chunk, offset + 5);
            std::uint32_t argc = readU32At(chunk, offset + 9);
            out << padRight("InvokeStatic", 12) << argc << "  ; ";
            if (classIndex < chunk.classes.size()) {
                out << chunk.classes[classIndex].name;
            } else {
                out << "<missing class>";
            }
            out << ".";
            if (nameIndex < chunk.names.size()) {
                out << chunk.names[nameIndex];
            } else {
                out << "<missing name>";
            }
            out << " (argc=" << argc << ")";
            return offset + 13;
        }
        case OpCode::GetStatic:
        case OpCode::StoreStatic: {
            if (offset + 9 > chunk.code.size()) {
                out << opCodeName(op) << " <truncated operands>";
                return chunk.code.size();
            }
            std::uint32_t classIndex = readU32At(chunk, offset + 1);
            std::uint32_t nameIndex = readU32At(chunk, offset + 5);
            out << padRight(opCodeName(op), 12) << "  ; ";
            if (classIndex < chunk.classes.size()) {
                out << chunk.classes[classIndex].name;
            } else {
                out << "<missing class>";
            }
            out << ".";
            if (nameIndex < chunk.names.size()) {
                out << chunk.names[nameIndex];
            } else {
                out << "<missing name>";
            }
            return offset + 9;
        }
        case OpCode::IsType: {
            if (offset + 5 > chunk.code.size()) {
                out << "IsType <truncated operand>";
                return chunk.code.size();
            }
            std::uint32_t classIndex = readU32At(chunk, offset + 1);
            out << padRight("IsType", 12) << classIndex << "  ; ";
            if (classIndex < chunk.classes.size()) {
                out << chunk.classes[classIndex].name;
            } else {
                out << "<missing class>";
            }
            return offset + 5;
        }
        default:
            out << "<unknown opcode " << static_cast<int>(chunk.code[offset]) << ">";
            return offset + 1;
    }
}

void disassemble(const Chunk& chunk, std::ostream& out) {
    out << "== Runa bytecode (format v" << FORMAT_VERSION << ", provisional) ==\n";

    out << "constants (" << chunk.constants.size() << "):\n";
    for (std::size_t i = 0; i < chunk.constants.size(); ++i) {
        out << "  [" << i << "] " << describeConstantForDump(chunk.constants[i]) << '\n';
    }

    out << "names (" << chunk.names.size() << "):\n";
    for (std::size_t i = 0; i < chunk.names.size(); ++i) {
        out << "  [" << i << "] " << chunk.names[i] << '\n';
    }

    out << "functions (" << chunk.functions.size() << "):\n";
    for (std::size_t i = 0; i < chunk.functions.size(); ++i) {
        const FunctionInfo& fn = chunk.functions[i];
        out << "  [" << i << "] " << fn.name << '(';
        for (std::size_t p = 0; p < fn.parameters.size(); ++p) {
            if (p > 0) out << ", ";
            out << fn.parameters[p];
        }
        out << ") -> code @0x" << std::hex << fn.entryOffset << std::dec << '\n';
    }

    out << "classes (" << chunk.classes.size() << "):\n";
    for (std::size_t i = 0; i < chunk.classes.size(); ++i) {
        const ClassInfo& cls = chunk.classes[i];
        out << "  [" << i << "] " << cls.name;
        if (cls.isInterface) out << " (interface)";
        else if (cls.isAbstract) out << " (abstract)";
        if (cls.parentIndex != kNoParent) {
            if (cls.parentIndex < chunk.classes.size()) {
                out << " : " << chunk.classes[cls.parentIndex].name;
            } else {
                out << " : <bad parent index " << cls.parentIndex << ">";
            }
        }
        out << '\n';
        for (const ClassFieldInfo& field : cls.fields) {
            out << "      field " << (field.isStatic ? "static " : "")
                << (field.isPrivate ? "private " : "") << field.name
                << " -> init @0x" << std::hex << field.initializerOffset
                << std::dec << '\n';
        }
        for (const ClassMethodInfo& method : cls.methods) {
            out << "      "
                << (method.isConstructor
                        ? std::string("constructor")
                        : (method.isStatic ? std::string("static ") : std::string("")) +
                              (method.isPrivate ? std::string("private ") : std::string("")) +
                              (method.isAbstract ? std::string("abstract ") : std::string("")) +
                              std::string("method ") + method.name)
                << '(';
            for (std::size_t p = 0; p < method.parameters.size(); ++p) {
                if (p > 0) out << ", ";
                out << method.parameters[p];
            }
            out << ") -> ";
            if (method.entryOffset == kNoOffset) {
                out << "(abstract, no body)";
            } else {
                out << "code @0x" << std::hex << method.entryOffset << std::dec;
            }
            out << '\n';
        }
    }

    // v0.9: per-unit startup order + the native FFI registry.
    out << "inits (" << chunk.inits.size() << "):\n";
    for (std::size_t i = 0; i < chunk.inits.size(); ++i) {
        const InitInfo& unit = chunk.inits[i];
        out << "  [" << i << "] "
            << (unit.packageName.empty() ? std::string("<main>") : unit.packageName)
            << ": classes [" << unit.firstClassIndex << ".."
            << (unit.firstClassIndex + unit.classCount) << ")";
        if (unit.initEntryOffset == kNoOffset) {
            out << ", no init body (main)\n";
        } else {
            out << ", init @0x" << std::hex << unit.initEntryOffset << std::dec << '\n';
        }
    }
    out << "natives (" << chunk.nativeFunctions.size() << "):\n";
    for (std::size_t i = 0; i < chunk.nativeFunctions.size(); ++i) {
        const NativeFunctionInfo& fn = chunk.nativeFunctions[i];
        out << "  [" << i << "] " << fn.namespaceName << "." << fn.name << " -> "
            << fn.symbol << " @ " << fn.packageDir << " (";
        for (std::size_t p = 0; p < fn.parameterTypes.size(); ++p) {
            if (p > 0) out << ", ";
            out << static_cast<int>(fn.parameterTypes[p]);
        }
        out << ") -> " << static_cast<int>(fn.returnType) << "\n";
    }

    out << "code (" << chunk.code.size() << " bytes):\n";
    std::size_t offset = 0;
    while (offset < chunk.code.size()) {
        out << "  ";
        out.width(4);
        out.fill('0');
        out << std::hex << offset << std::dec;
        out << "  ";
        offset = formatInstructionAt(chunk, offset, out);
        out << '\n';
    }
}

// ---- binary serialization (provisional v1) -----------------------------------

namespace {

void writeU32(std::ostream& out, std::uint32_t value) {
    const char bytes[4] = {
        static_cast<char>(value & 0xFF),
        static_cast<char>((value >> 8) & 0xFF),
        static_cast<char>((value >> 16) & 0xFF),
        static_cast<char>((value >> 24) & 0xFF),
    };
    out.write(bytes, 4);
}

void writeI64(std::ostream& out, std::int64_t value) {
    std::uint64_t raw = static_cast<std::uint64_t>(value);
    const char bytes[8] = {
        static_cast<char>(raw & 0xFF),
        static_cast<char>((raw >> 8) & 0xFF),
        static_cast<char>((raw >> 16) & 0xFF),
        static_cast<char>((raw >> 24) & 0xFF),
        static_cast<char>((raw >> 32) & 0xFF),
        static_cast<char>((raw >> 40) & 0xFF),
        static_cast<char>((raw >> 48) & 0xFF),
        static_cast<char>((raw >> 56) & 0xFF),
    };
    out.write(bytes, 8);
}

void writeU64(std::ostream& out, std::uint64_t raw) {
    const char bytes[8] = {
        static_cast<char>(raw & 0xFF),
        static_cast<char>((raw >> 8) & 0xFF),
        static_cast<char>((raw >> 16) & 0xFF),
        static_cast<char>((raw >> 24) & 0xFF),
        static_cast<char>((raw >> 32) & 0xFF),
        static_cast<char>((raw >> 40) & 0xFF),
        static_cast<char>((raw >> 48) & 0xFF),
        static_cast<char>((raw >> 56) & 0xFF),
    };
    out.write(bytes, 8);
}

std::uint32_t readU32(std::istream& in) {
    unsigned char bytes[4];
    in.read(reinterpret_cast<char*>(bytes), 4);
    if (!in) throw std::runtime_error("truncated bytecode file");
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::int64_t readI64(std::istream& in) {
    unsigned char bytes[8];
    in.read(reinterpret_cast<char*>(bytes), 8);
    if (!in) throw std::runtime_error("truncated bytecode file");
    std::uint64_t raw = 0;
    for (int i = 7; i >= 0; --i) {
        raw = (raw << 8) | static_cast<std::uint64_t>(bytes[i]);
    }
    return static_cast<std::int64_t>(raw);
}

std::uint64_t readU64(std::istream& in) {
    unsigned char bytes[8];
    in.read(reinterpret_cast<char*>(bytes), 8);
    if (!in) throw std::runtime_error("truncated bytecode file");
    std::uint64_t raw = 0;
    for (int i = 7; i >= 0; --i) {
        raw = (raw << 8) | static_cast<std::uint64_t>(bytes[i]);
    }
    return raw;
}

void writeConstants(std::ostream& out, const std::vector<Constant>& constants) {
    writeU32(out, static_cast<std::uint32_t>(constants.size()));
    for (const Constant& constant : constants) {
        out.put(static_cast<char>(constant.kind));
        switch (constant.kind) {
            case Constant::Kind::String:
                writeU32(out, static_cast<std::uint32_t>(constant.str.size()));
                out.write(constant.str.data(),
                          static_cast<std::streamsize>(constant.str.size()));
                break;
            case Constant::Kind::Number:
                writeI64(out, constant.number);
                break;
            case Constant::Kind::Float: {
                // Bit-exact: the double's IEEE-754 pattern travels as a u64
                // (little-endian like everything else) so the value that
                // comes back is identical.
                std::uint64_t bits = 0;
                static_assert(sizeof(bits) == sizeof(constant.floating),
                              "double must be 64-bit");
                std::memcpy(&bits, &constant.floating, sizeof(bits));
                writeU64(out, bits);
                break;
            }
            case Constant::Kind::Boolean:
                out.put(constant.boolean ? 1 : 0);
                break;
            case Constant::Kind::Null:
                // No payload.
                break;
        }
    }
}

std::vector<Constant> readConstants(std::istream& in) {
    std::uint32_t count = readU32(in);
    std::vector<Constant> constants;
    constants.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        int kindByte = in.get();
        if (kindByte == EOF) throw std::runtime_error("truncated bytecode file");
        Constant constant;
        constant.kind = static_cast<Constant::Kind>(kindByte);
        switch (constant.kind) {
            case Constant::Kind::String: {
                std::uint32_t length = readU32(in);
                if (length > (1u << 26)) {
                    throw std::runtime_error("corrupt bytecode file (entry too large)");
                }
                constant.str.resize(length);
                if (length > 0) {
                    in.read(&constant.str[0], static_cast<std::streamsize>(length));
                    if (!in) throw std::runtime_error("truncated bytecode file");
                }
                break;
            }
            case Constant::Kind::Number:
                constant.number = readI64(in);
                break;
            case Constant::Kind::Float: {
                const std::uint64_t bits = readU64(in);
                std::memcpy(&constant.floating, &bits, sizeof(constant.floating));
                break;
            }
            case Constant::Kind::Boolean: {
                int flag = in.get();
                if (flag == EOF) throw std::runtime_error("truncated bytecode file");
                constant.boolean = (flag != 0);
                break;
            }
            case Constant::Kind::Null:
                // No payload.
                break;
            default:
                throw std::runtime_error("corrupt bytecode file (bad constant kind)");
        }
        constants.push_back(std::move(constant));
    }
    return constants;
}

void writePool(std::ostream& out, const std::vector<std::string>& pool) {
    writeU32(out, static_cast<std::uint32_t>(pool.size()));
    for (const std::string& entry : pool) {
        writeU32(out, static_cast<std::uint32_t>(entry.size()));
        out.write(entry.data(), static_cast<std::streamsize>(entry.size()));
    }
}

void writeFunctions(std::ostream& out, const std::vector<FunctionInfo>& functions) {
    writeU32(out, static_cast<std::uint32_t>(functions.size()));
    for (const FunctionInfo& fn : functions) {
        writeU32(out, static_cast<std::uint32_t>(fn.name.size()));
        out.write(fn.name.data(), static_cast<std::streamsize>(fn.name.size()));
        writeU32(out, static_cast<std::uint32_t>(fn.parameters.size()));
        for (const std::string& parameter : fn.parameters) {
            writeU32(out, static_cast<std::uint32_t>(parameter.size()));
            out.write(parameter.data(), static_cast<std::streamsize>(parameter.size()));
        }
        writeU32(out, fn.entryOffset);
    }
}

void writeClasses(std::ostream& out, const std::vector<ClassInfo>& classes) {
    writeU32(out, static_cast<std::uint32_t>(classes.size()));
    for (const ClassInfo& cls : classes) {
        writeU32(out, static_cast<std::uint32_t>(cls.name.size()));
        out.write(cls.name.data(), static_cast<std::streamsize>(cls.name.size()));
        writeU32(out, cls.parentIndex);
        std::uint8_t flags = 0;
        if (cls.isAbstract) flags |= 0x01;
        if (cls.isInterface) flags |= 0x02;
        out.put(static_cast<char>(flags));
        writeU32(out, static_cast<std::uint32_t>(cls.interfaceNames.size()));
        for (const std::string& interfaceName : cls.interfaceNames) {
            writeU32(out, static_cast<std::uint32_t>(interfaceName.size()));
            out.write(interfaceName.data(),
                      static_cast<std::streamsize>(interfaceName.size()));
        }
        writeU32(out, static_cast<std::uint32_t>(cls.fields.size()));
        for (const ClassFieldInfo& field : cls.fields) {
            std::uint8_t fieldFlags = 0;
            if (field.isStatic) fieldFlags |= 0x01;
            if (field.isPrivate) fieldFlags |= 0x02;
            out.put(static_cast<char>(fieldFlags));
            writeU32(out, static_cast<std::uint32_t>(field.name.size()));
            out.write(field.name.data(), static_cast<std::streamsize>(field.name.size()));
            writeU32(out, field.initializerOffset);
        }
        writeU32(out, static_cast<std::uint32_t>(cls.methods.size()));
        for (const ClassMethodInfo& method : cls.methods) {
            writeU32(out, static_cast<std::uint32_t>(method.name.size()));
            out.write(method.name.data(), static_cast<std::streamsize>(method.name.size()));
            std::uint8_t methodFlags = 0;
            if (method.isStatic) methodFlags |= 0x01;
            if (method.isPrivate) methodFlags |= 0x02;
            if (method.isAbstract) methodFlags |= 0x04;
            if (method.isConstructor) methodFlags |= 0x08;
            out.put(static_cast<char>(methodFlags));
            writeU32(out, static_cast<std::uint32_t>(method.parameters.size()));
            for (const std::string& parameter : method.parameters) {
                writeU32(out, static_cast<std::uint32_t>(parameter.size()));
                out.write(parameter.data(), static_cast<std::streamsize>(parameter.size()));
            }
            writeU32(out, method.entryOffset);
        }
    }
}

std::vector<std::string> readPool(std::istream& in) {
    std::uint32_t count = readU32(in);
    std::vector<std::string> pool;
    pool.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t length = readU32(in);
        if (length > (1u << 26)) {
            throw std::runtime_error("corrupt bytecode file (entry too large)");
        }
        std::string entry(length, '\0');
        if (length > 0) {
            in.read(&entry[0], static_cast<std::streamsize>(length));
            if (!in) throw std::runtime_error("truncated bytecode file");
        }
        pool.push_back(std::move(entry));
    }
    return pool;
}

void writeInits(std::ostream& out, const std::vector<InitInfo>& inits) {
    writeU32(out, static_cast<std::uint32_t>(inits.size()));
    for (const InitInfo& unit : inits) {
        writeU32(out, static_cast<std::uint32_t>(unit.packageName.size()));
        out.write(unit.packageName.data(),
                  static_cast<std::streamsize>(unit.packageName.size()));
        writeU32(out, unit.initEntryOffset);
        writeU32(out, unit.firstClassIndex);
        writeU32(out, unit.classCount);
    }
}

void writeNatives(std::ostream& out, const std::vector<NativeFunctionInfo>& natives) {
    writeU32(out, static_cast<std::uint32_t>(natives.size()));
    for (const NativeFunctionInfo& fn : natives) {
        writeU32(out, static_cast<std::uint32_t>(fn.namespaceName.size()));
        out.write(fn.namespaceName.data(),
                  static_cast<std::streamsize>(fn.namespaceName.size()));
        writeU32(out, static_cast<std::uint32_t>(fn.name.size()));
        out.write(fn.name.data(), static_cast<std::streamsize>(fn.name.size()));
        writeU32(out, static_cast<std::uint32_t>(fn.symbol.size()));
        out.write(fn.symbol.data(), static_cast<std::streamsize>(fn.symbol.size()));
        writeU32(out, static_cast<std::uint32_t>(fn.packageDir.size()));
        out.write(fn.packageDir.data(),
                  static_cast<std::streamsize>(fn.packageDir.size()));
        writeU32(out, static_cast<std::uint32_t>(fn.targets.size()));
        for (const auto& target : fn.targets) {
            writeU32(out, static_cast<std::uint32_t>(target.first.size()));
            out.write(target.first.data(),
                      static_cast<std::streamsize>(target.first.size()));
            writeU32(out, static_cast<std::uint32_t>(target.second.size()));
            out.write(target.second.data(),
                      static_cast<std::streamsize>(target.second.size()));
        }
        out.put(static_cast<char>(fn.returnType));
        writeU32(out, static_cast<std::uint32_t>(fn.parameterTypes.size()));
        for (std::uint8_t type : fn.parameterTypes) {
            out.put(static_cast<char>(type));
        }
    }
}

std::vector<InitInfo> readInits(std::istream& in) {
    std::uint32_t count = readU32(in);
    if (count > (1u << 16)) {
        throw std::runtime_error("corrupt bytecode file (too many init units)");
    }
    std::vector<InitInfo> inits;
    inits.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        InitInfo unit;
        std::uint32_t nameLength = readU32(in);
        if (nameLength > (1u << 16)) {
            throw std::runtime_error("corrupt bytecode file (init name too large)");
        }
        unit.packageName.resize(nameLength);
        if (nameLength > 0) {
            in.read(&unit.packageName[0], static_cast<std::streamsize>(nameLength));
            if (!in) throw std::runtime_error("truncated bytecode file");
        }
        unit.initEntryOffset = readU32(in);
        unit.firstClassIndex = readU32(in);
        unit.classCount = readU32(in);
        inits.push_back(std::move(unit));
    }
    return inits;
}

std::vector<NativeFunctionInfo> readNatives(std::istream& in) {
    std::uint32_t count = readU32(in);
    if (count > (1u << 16)) {
        throw std::runtime_error("corrupt bytecode file (too many native functions)");
    }
    std::vector<NativeFunctionInfo> natives;
    natives.reserve(count);
    const auto readString = [&in](const char* what) {
        std::uint32_t length = readU32(in);
        if (length > (1u << 20)) {
            throw std::runtime_error(std::string("corrupt bytecode file (") + what +
                                     " too large)");
        }
        std::string value(length, '\0');
        if (length > 0) {
            in.read(&value[0], static_cast<std::streamsize>(length));
            if (!in) throw std::runtime_error("truncated bytecode file");
        }
        return value;
    };
    for (std::uint32_t i = 0; i < count; ++i) {
        NativeFunctionInfo fn;
        fn.namespaceName = readString("native namespace");
        fn.name = readString("native function name");
        fn.symbol = readString("native symbol");
        fn.packageDir = readString("native package dir");
        std::uint32_t targetCount = readU32(in);
        if (targetCount > (1u << 12)) {
            throw std::runtime_error("corrupt bytecode file (too many native targets)");
        }
        for (std::uint32_t t = 0; t < targetCount; ++t) {
            std::string key = readString("native target key");
            std::string path = readString("native target path");
            fn.targets.emplace_back(std::move(key), std::move(path));
        }
        int returnByte = in.get();
        if (returnByte == EOF) throw std::runtime_error("truncated bytecode file");
        if (returnByte < nativeTypes::Int8 || returnByte > nativeTypes::Void) {
            throw std::runtime_error("corrupt bytecode file (invalid native type)");
        }
        fn.returnType = static_cast<std::uint8_t>(returnByte);
        std::uint32_t parameterCount = readU32(in);
        if (parameterCount > 16) {
            throw std::runtime_error("corrupt bytecode file (too many native parameters)");
        }
        for (std::uint32_t p = 0; p < parameterCount; ++p) {
            int typeByte = in.get();
            if (typeByte == EOF) throw std::runtime_error("truncated bytecode file");
            if (typeByte < nativeTypes::Int8 || typeByte > nativeTypes::Void) {
                throw std::runtime_error("corrupt bytecode file (invalid native type)");
            }
            fn.parameterTypes.push_back(static_cast<std::uint8_t>(typeByte));
        }
        natives.push_back(std::move(fn));
    }
    return natives;
}

std::vector<ClassInfo> readClasses(std::istream& in) {
    std::uint32_t count = readU32(in);
    if (count > (1u << 20)) {
        throw std::runtime_error("corrupt bytecode file (too many classes)");
    }
    std::vector<ClassInfo> classes;
    classes.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        ClassInfo cls;
        std::uint32_t nameLength = readU32(in);
        if (nameLength > (1u << 20)) {
            throw std::runtime_error("corrupt bytecode file (class name too large)");
        }
        cls.name.resize(nameLength);
        if (nameLength > 0) {
            in.read(&cls.name[0], static_cast<std::streamsize>(nameLength));
            if (!in) throw std::runtime_error("truncated bytecode file");
        }
        cls.parentIndex = readU32(in);
        int flags = in.get();
        if (flags == EOF) throw std::runtime_error("truncated bytecode file");
        cls.isAbstract = (flags & 0x01) != 0;
        cls.isInterface = (flags & 0x02) != 0;

        std::uint32_t interfaceCount = readU32(in);
        if (interfaceCount > (1u << 16)) {
            throw std::runtime_error("corrupt bytecode file (too many interfaces)");
        }
        cls.interfaceNames.reserve(interfaceCount);
        for (std::uint32_t n = 0; n < interfaceCount; ++n) {
            std::uint32_t length = readU32(in);
            if (length > (1u << 20)) {
                throw std::runtime_error("corrupt bytecode file (interface name too large)");
            }
            std::string interfaceName(length, '\0');
            if (length > 0) {
                in.read(&interfaceName[0], static_cast<std::streamsize>(length));
                if (!in) throw std::runtime_error("truncated bytecode file");
            }
            cls.interfaceNames.push_back(std::move(interfaceName));
        }

        std::uint32_t fieldCount = readU32(in);
        if (fieldCount > (1u << 16)) {
            throw std::runtime_error("corrupt bytecode file (too many fields)");
        }
        cls.fields.reserve(fieldCount);
        for (std::uint32_t f = 0; f < fieldCount; ++f) {
            ClassFieldInfo field;
            int fieldFlags = in.get();
            if (fieldFlags == EOF) throw std::runtime_error("truncated bytecode file");
            field.isStatic = (fieldFlags & 0x01) != 0;
            field.isPrivate = (fieldFlags & 0x02) != 0;
            std::uint32_t length = readU32(in);
            if (length > (1u << 20)) {
                throw std::runtime_error("corrupt bytecode file (field name too large)");
            }
            field.name.resize(length);
            if (length > 0) {
                in.read(&field.name[0], static_cast<std::streamsize>(length));
                if (!in) throw std::runtime_error("truncated bytecode file");
            }
            field.initializerOffset = readU32(in);
            cls.fields.push_back(std::move(field));
        }

        std::uint32_t methodCount = readU32(in);
        if (methodCount > (1u << 16)) {
            throw std::runtime_error("corrupt bytecode file (too many methods)");
        }
        cls.methods.reserve(methodCount);
        for (std::uint32_t m = 0; m < methodCount; ++m) {
            ClassMethodInfo method;
            std::uint32_t length = readU32(in);
            if (length > (1u << 20)) {
                throw std::runtime_error("corrupt bytecode file (method name too large)");
            }
            method.name.resize(length);
            if (length > 0) {
                in.read(&method.name[0], static_cast<std::streamsize>(length));
                if (!in) throw std::runtime_error("truncated bytecode file");
            }
            int methodFlags = in.get();
            if (methodFlags == EOF) throw std::runtime_error("truncated bytecode file");
            method.isStatic = (methodFlags & 0x01) != 0;
            method.isPrivate = (methodFlags & 0x02) != 0;
            method.isAbstract = (methodFlags & 0x04) != 0;
            method.isConstructor = (methodFlags & 0x08) != 0;
            std::uint32_t parameterCount = readU32(in);
            if (parameterCount > (1u << 16)) {
                throw std::runtime_error("corrupt bytecode file (too many parameters)");
            }
            method.parameters.reserve(parameterCount);
            for (std::uint32_t p = 0; p < parameterCount; ++p) {
                std::uint32_t parameterLength = readU32(in);
                if (parameterLength > (1u << 20)) {
                    throw std::runtime_error("corrupt bytecode file (parameter name too large)");
                }
                std::string parameter(parameterLength, '\0');
                if (parameterLength > 0) {
                    in.read(&parameter[0], static_cast<std::streamsize>(parameterLength));
                    if (!in) throw std::runtime_error("truncated bytecode file");
                }
                method.parameters.push_back(std::move(parameter));
            }
            method.entryOffset = readU32(in);
            cls.methods.push_back(std::move(method));
        }
        classes.push_back(std::move(cls));
    }
    return classes;
}

std::vector<FunctionInfo> readFunctions(std::istream& in) {
    std::uint32_t count = readU32(in);
    if (count > (1u << 20)) {
        throw std::runtime_error("corrupt bytecode file (too many functions)");
    }
    std::vector<FunctionInfo> functions;
    functions.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        FunctionInfo fn;
        std::uint32_t nameLength = readU32(in);
        if (nameLength > (1u << 20)) {
            throw std::runtime_error("corrupt bytecode file (function name too large)");
        }
        fn.name.resize(nameLength);
        if (nameLength > 0) {
            in.read(&fn.name[0], static_cast<std::streamsize>(nameLength));
            if (!in) throw std::runtime_error("truncated bytecode file");
        }
        std::uint32_t parameterCount = readU32(in);
        if (parameterCount > (1u << 16)) {
            throw std::runtime_error("corrupt bytecode file (too many parameters)");
        }
        fn.parameters.reserve(parameterCount);
        for (std::uint32_t p = 0; p < parameterCount; ++p) {
            std::uint32_t length = readU32(in);
            if (length > (1u << 20)) {
                throw std::runtime_error("corrupt bytecode file (parameter name too large)");
            }
            std::string parameter(length, '\0');
            if (length > 0) {
                in.read(&parameter[0], static_cast<std::streamsize>(length));
                if (!in) throw std::runtime_error("truncated bytecode file");
            }
            fn.parameters.push_back(std::move(parameter));
        }
        fn.entryOffset = readU32(in);
        functions.push_back(std::move(fn));
    }
    return functions;
}

}  // namespace

void serialize(const Chunk& chunk, std::ostream& out) {
    out.write("RUNA", 4);
    writeU32(out, FORMAT_VERSION);
    writeConstants(out, chunk.constants);
    writePool(out, chunk.names);
    writeFunctions(out, chunk.functions);
    writeClasses(out, chunk.classes);
    writeInits(out, chunk.inits);
    writeNatives(out, chunk.nativeFunctions);
    writeU32(out, static_cast<std::uint32_t>(chunk.code.size()));
    out.write(reinterpret_cast<const char*>(chunk.code.data()),
              static_cast<std::streamsize>(chunk.code.size()));
}

Chunk deserialize(std::istream& in) {
    char magic[4];
    in.read(magic, 4);
    if (!in || magic[0] != 'R' || magic[1] != 'U' || magic[2] != 'N' || magic[3] != 'A') {
        throw std::runtime_error("not a Runa bytecode file (bad magic)");
    }
    std::uint32_t version = readU32(in);
    if (version != FORMAT_VERSION) {
        throw std::runtime_error(
            "unsupported bytecode format version " + std::to_string(version) +
            " (this toolchain supports version " + std::to_string(FORMAT_VERSION) +
            ") — recompile the program with the current runac");
    }
    Chunk chunk;
    chunk.constants = readConstants(in);
    chunk.names = readPool(in);
    chunk.functions = readFunctions(in);
    chunk.classes = readClasses(in);
    chunk.inits = readInits(in);
    chunk.nativeFunctions = readNatives(in);

    // Structural validation of the function table (defense in depth: the
    // compiler never produces any of these, but hand-crafted files must not
    // reach the VM unchecked).
    for (std::size_t i = 0; i < chunk.functions.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            // Overloads share a name and differ by parameter count (spec 16).
            if (chunk.functions[i].name == chunk.functions[j].name &&
                chunk.functions[i].parameters.size() ==
                    chunk.functions[j].parameters.size()) {
                throw std::runtime_error(
                    "corrupt bytecode file (duplicate function '" +
                    chunk.functions[i].name + "' with " +
                    std::to_string(chunk.functions[i].parameters.size()) +
                    " parameter(s))");
            }
        }
        const FunctionInfo& fn = chunk.functions[i];
        for (std::size_t p = 0; p + 1 < fn.parameters.size(); ++p) {
            for (std::size_t q = p + 1; q < fn.parameters.size(); ++q) {
                if (fn.parameters[p] == fn.parameters[q]) {
                    throw std::runtime_error("corrupt bytecode file (duplicate parameter name in function '" +
                                             fn.name + "')");
                }
            }
        }
    }

    std::uint32_t codeSize = readU32(in);
    if (codeSize > (1u << 26)) {
        throw std::runtime_error("corrupt bytecode file (code too large)");
    }
    chunk.code.resize(codeSize);
    if (codeSize > 0) {
        in.read(reinterpret_cast<char*>(chunk.code.data()),
                static_cast<std::streamsize>(codeSize));
        if (!in) throw std::runtime_error("truncated bytecode file");
    }

    // Entry offsets can only be validated once the code size is known.
    for (const FunctionInfo& fn : chunk.functions) {
        if (fn.entryOffset >= codeSize) {
            throw std::runtime_error("corrupt bytecode file (function '" + fn.name +
                                     "' has an entry offset outside the code)");
        }
    }

    // Structural validation of the class table (defense in depth).
    for (std::size_t i = 0; i < chunk.classes.size(); ++i) {
        const ClassInfo& cls = chunk.classes[i];
        if (cls.parentIndex != kNoParent && cls.parentIndex >= chunk.classes.size()) {
            throw std::runtime_error("corrupt bytecode file (class '" + cls.name +
                                     "' has a parent index outside the class table)");
        }
        for (const ClassFieldInfo& field : cls.fields) {
            if (field.initializerOffset != kNoOffset &&
                field.initializerOffset >= codeSize) {
                throw std::runtime_error("corrupt bytecode file (field '" + field.name +
                                         "' of class '" + cls.name +
                                         "' has an initializer offset outside the code)");
            }
        }
        for (const ClassMethodInfo& method : cls.methods) {
            if (method.entryOffset != kNoOffset && method.entryOffset >= codeSize) {
                throw std::runtime_error("corrupt bytecode file (method '" + method.name +
                                         "' of class '" + cls.name +
                                         "' has an entry offset outside the code)");
            }
        }
    }

    // v0.9: validate the init table (ranges + entry offsets).
    for (const InitInfo& unit : chunk.inits) {
        if (unit.initEntryOffset != kNoOffset && unit.initEntryOffset >= codeSize) {
            throw std::runtime_error("corrupt bytecode file (init entry of unit '" +
                                     unit.packageName +
                                     "' lies outside the code)");
        }
        if (unit.firstClassIndex > chunk.classes.size() ||
            unit.classCount > chunk.classes.size() - unit.firstClassIndex) {
            throw std::runtime_error("corrupt bytecode file (unit '" + unit.packageName +
                                     "' owns classes outside the class table)");
        }
    }

    // v0.9: validate the natives table (path safety).
    for (const NativeFunctionInfo& fn : chunk.nativeFunctions) {
        if (fn.packageDir.empty() || fn.packageDir.find("..") != std::string::npos ||
            fn.packageDir.front() == '/' || fn.packageDir.back() == '/') {
            throw std::runtime_error("corrupt bytecode file (invalid native package dir '" +
                                     fn.packageDir + "')");
        }
    }
    return chunk;
}

}  // namespace runa::bytecode
