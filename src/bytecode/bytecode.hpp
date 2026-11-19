// Runa — Bytecode.
//
// Bytecode is the intermediate representation between the Compiler and the VM.
// It is NOT CPU machine code: it is designed only for the Runa Virtual Machine.
//
// ⚠ PROVISIONAL: the binary container format and the instruction set are
// intentionally minimal and must NOT be considered final. The format is
// versioned and extended only when the language syntax grows (baseline spec,
// section 12). Version 1 added typed constants (string/number/boolean);
// version-0 files are rejected with a clear error. The arithmetic opcodes
// (Add..Mod) were later added WITHOUT a container change; the comparison and
// jump opcodes (Eq..Le, JumpIfFalse, Jump) were appended AFTER Halt so the
// numeric value of every existing opcode stays stable.
//
// Version 2 (the function system) adds a `functions` table between the names
// pool and the code. Because the LAYOUT changes, the version bumps to 2 and
// version-1 (and older) files are rejected with a clear message — recompile.
// New function opcodes were again appended after Jump so every existing
// opcode number stays stable across the whole history.
//
// Version 3 (the class system) adds a `classes` table after the functions
// table. The layout changes again, so the version bumps to 3; version-2 and
// older files are rejected — recompile. New class opcodes were appended after
// StoreLocal, keeping every earlier opcode number stable.
//
// Version 3 remained the current version for the loop system (v0.8): loops
// compile onto the EXISTING Jump / JumpIfFalse instructions (the same shape
// as if/elif/else), so no opcode was appended and the container layout is
// unchanged — old v3 files stay valid and no re-encode is needed.
//
// Version 4 (the package system, v0.9) adds two tables after the classes
// table: `inits` (per-unit startup: the class index range of each unit and
// that unit's package-init code entry) and `natives` (the FFI metadata the
// VM needs to resolve cimport'ed native packages: package dir, platform
// targets and typed function signatures). The LAYOUT changes, so the version
// bumps to 4; version-3 and older files are rejected with a clear message —
// recompile. Exactly ONE opcode was appended after EqIdent (CallNative), so
// every existing opcode number stays stable across the whole history.
//
// Version 5 (the collection system, v1.0-rc) appends exactly ONE opcode
// (BuildCollection) after CallNative. The container LAYOUT is unchanged from
// version 4 — the version still bumps so old VMs reject collection programs
// with the clear "unsupported bytecode format version" message instead of an
// unknown-opcode failure mid-execution; version-4 and older files are
// rejected — recompile. Every existing opcode number stays stable.
//
// Version 6 (float constants, Runa 1.0 spec section 5) adds the Float kind
// to the constant pool (kind byte 4). No opcode was appended.
//
// Version 7 (unary operators, logical operators and string concatenation,
// DECISIONS #126/#127/#128/#129) appends SIX opcodes after BuildCollection:
// Negate, Not, And, Or, JumpIfFalseKeep and JumpIfTrueKeep. The container
// LAYOUT is unchanged — the version bumps per the v5 rule (an old VM must
// reject programs that use the new syntax with the clear version message
// instead of an unknown-opcode failure); version-6 and older files are
// rejected — recompile. Every existing opcode number stays stable.
//
// Provisional container layout (all integers little-endian):
//
//     magic     : 4 bytes  'R' 'U' 'N' 'A'
//     version   : u32      (currently 7)
//     constants : u32 count, then per entry:
//                   u8 kind (0 = string, 1 = number, 2 = boolean, 3 = null)
//                   string : u32 byte-length + bytes
//                   number : 8 bytes int64 (two's complement, LE)
//                   boolean: 1 byte (0 = false, 1 = true)
//                   null   : (no payload)
//     names     : u32 count, then per entry: u32 byte-length + bytes
//     functions : u32 count, then per entry:
//                   u32 name byte-length + bytes
//                   u32 parameter count, then per parameter:
//                     u32 byte-length + bytes
//                   u32 entry offset (code position of the function body)
//     classes   : u32 count, then per entry:
//                   u32 name byte-length + bytes
//                   u32 parent index (0xFFFFFFFF = none)
//                   u8 flags (bit0 = abstract, bit1 = interface)
//                   u32 field count, then per field:
//                     u8 flags (bit0 = static, bit1 = private)
//                     u32 name byte-length + bytes
//                     u32 initializer offset (0xFFFFFFFF = none)
//                   u32 method count, then per method:
//                     u32 name byte-length + bytes
//                     u8 flags (bit0 = static, bit1 = private,
//                               bit2 = abstract, bit3 = constructor)
//                     u32 parameter count, then per parameter:
//                       u32 byte-length + bytes
//                     u32 entry offset (0xFFFFFFFF = abstract)
//     inits     : u32 count, then per entry (dependency-ordered units):
//                   u32 package name byte-length + bytes ("" = main unit)
//                   u32 init entry offset (0xFFFFFFFF = main: no init body)
//                   u32 first class index
//                   u32 class count
//     natives   : u32 count, then per entry:
//                   u32 namespace byte-length + bytes
//                   u32 api name byte-length + bytes
//                   u32 symbol byte-length + bytes
//                   u32 package dir byte-length + bytes
//                   u32 target count, then per target:
//                     u32 platform key byte-length + bytes
//                     u32 binary path byte-length + bytes
//                   u8 return type
//                   u32 parameter count, then per parameter: u8 type
//     code      : u32 byte-count + bytes
//
// This module is self-contained on purpose: it does NOT depend on the runtime
// value model. The VM converts constants into runtime values when executing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <istream>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace runa::bytecode {

constexpr std::uint32_t FORMAT_VERSION = 7;
constexpr std::uint32_t kNoParent = 0xFFFFFFFFu;
constexpr std::uint32_t kNoOffset = 0xFFFFFFFFu;

enum class OpCode : std::uint8_t {
    LoadConst,   // operand: u32 constant index      → push the constant
    LoadGlobal,  // operand: u32 name index          → push global <name>
    StoreGlobal, // operand: u32 name index          → pop value into global <name>
    Call,        // operands: u32 name index, u32 argc → pop argc args, call builtin
    Add,         // pop rhs, pop lhs → push (lhs + rhs)
    Sub,         // pop rhs, pop lhs → push (lhs - rhs)
    Mul,         // pop rhs, pop lhs → push (lhs * rhs)
    Div,         // pop rhs, pop lhs → push (lhs / rhs)  (integer division)
    Mod,         // pop rhs, pop lhs → push (lhs % rhs)  (remainder)
    Halt,        // stop execution
    // --- appended after Halt so older opcode numbers remain stable ---
    Eq,          // pop rhs, pop lhs → push boolean (lhs == rhs)
    Ne,          // pop rhs, pop lhs → push boolean (lhs != rhs)
    Gt,          // pop rhs, pop lhs → push boolean (lhs > rhs)   (numbers)
    Lt,          // pop rhs, pop lhs → push boolean (lhs < rhs)   (numbers)
    Ge,          // pop rhs, pop lhs → push boolean (lhs >= rhs)  (numbers)
    Le,          // pop rhs, pop lhs → push boolean (lhs <= rhs)  (numbers)
    JumpIfFalse, // operand: u32 target offset → pop value; if not boolean true,
                 // continue execution at <target> (used by `if`)
    Jump,        // operand: u32 target offset → continue execution at <target>
                 // (used by if/elif/else chains to skip remaining branches)
    // --- appended for the function system; older opcode numbers stay stable ---
    ReturnValue, // pop value → return it from the current function to the caller
    ReturnNone,  // return from the current function producing the distinct
                 // "none" value (bare return / end of a function body)
    Pop,         // pop and discard the top value (used by bare call statements)
    LoadVar,     // operand: u32 name index → push local <name> of the current
                 // call frame, falling back to global <name>
    StoreLocal,  // operand: u32 name index → pop value into the current call
                 // frame's local <name> (never touches globals)
    // --- appended for the class system; older opcode numbers stay stable ---
    NewObject,   // operands: u32 class index, u32 argc → pop argc args, create
                 // an instance of the class, run its field initializers and
                 // constructor, push the new object
    GetField,    // operand: u32 name index → pop object, push field <name>
    SetField,    // operand: u32 name index → pop value, pop object, set field
    InitField,   // operand: u32 name index → pop value into the field <name>
                 // of the instance being constructed (internal, used inside
                 // field-initializer blocks)
    GetSelf,     // push the current frame's self reference
    InvokeVirtual,  // operands: u32 name index, u32 argc → pop argc args and
                    // the receiver, virtually dispatch method <name> from the
                    // receiver's runtime class, push the result
    InvokeStatic,   // operands: u32 class index, u32 name index, u32 argc →
                    // pop argc args, call the static method (no self)
    InvokeSuper,    // operands: u32 name index, u32 argc → pop argc args and
                    // dispatch method <name> starting at the PARENT of the
                    // current method's class, with self unchanged
    GetStatic,   // operands: u32 class index, u32 name index → push the value
                 // of the class-level (static) field
    StoreStatic, // operands: u32 class index, u32 name index → pop value into
                 // the class-level (static) field
    IsType,      // operand: u32 class index → pop value, push boolean
                 // (value is an object compatible with the class/interface)
    EqIdent,     // pop rhs, pop lhs → push boolean (lhs === rhs, object
                 // identity for objects)
    // --- appended for the package system (v0.9); older opcodes stay stable ---
    CallNative,  // operands: u32 native function index, u32 argc → pop argc
                 // args, call the native (FFI) function through the native
                 // bridge, push the converted result. The VM resolves the
                 // binary via the natives table; the compiler validated the
                 // API (existence, arity, types) at compile time.
    // --- appended for the collection system (v1.0-rc); older opcodes stable ---
    BuildCollection,  // operand: u32 element count → pop <count> values (in
                      // source order, top of stack = last element), build one
                      // set-like collection value (duplicates silently
                      // collapsed, one element type enforced at run time)
                      // and push it.
    // --- appended for unary/logical operators + string concat (v7);
    // --- older opcodes stay stable ---
    Negate,      // pop value → push its negation (numbers/floats; -Int64.Min
                 // is a clean RuntimeError)
    Not,         // pop boolean → push its logical negation ('not')
    And,         // pop rhs, pop lhs → push boolean (lhs and rhs); both must
                 // be booleans. Emitted only on the non-short-circuit path
                 // of 'and' (see JumpIfFalseKeep).
    Or,          // pop rhs, pop lhs → push boolean (lhs or rhs); both must
                 // be booleans. Emitted only on the non-short-circuit path
                 // of 'or' (see JumpIfTrueKeep).
    JumpIfFalseKeep,  // operand: u32 target offset → like JumpIfFalse, but
                      // the value STAYS on the stack when the jump is taken
                      // (short-circuit 'and' keeps the false as the result)
    JumpIfTrueKeep,   // operand: u32 target offset → jump when the top of
                      // stack is boolean true, KEEPING it on the stack
                      // (short-circuit 'or' keeps the true as the result)
};

const char* opCodeName(OpCode op);

// A typed compile-time constant. Kind mirrors the runtime value kinds that
// can appear as literals in source code (extensible: add a kind + payload).
struct Constant {
    enum class Kind : std::uint8_t { String = 0, Number = 1, Boolean = 2, Null = 3,
                                     Float = 4 };

    Kind kind = Kind::String;
    std::string str;          // payload for Kind::String
    std::int64_t number = 0;  // payload for Kind::Number
    bool boolean = false;     // payload for Kind::Boolean
    double floating = 0.0;    // payload for Kind::Float (bit-exact u64 write)

    // Human-readable form for disassembly, e.g.  "Hi"  |  123  |  true  | null  | 1.5
    std::string describe() const;
};

// One registered function: the compiler fills this when it compiles a
// function declaration; the VM builds its call registry from this table.
struct FunctionInfo {
    std::string name;
    std::vector<std::string> parameters;
    std::uint32_t entryOffset = 0;  // code offset where the body starts
};

// One registered class (or interface): the compiler fills this when it
// compiles a class declaration; the VM builds its class registry from this
// table. Field initializers, methods and the constructor are emitted as code
// blocks AFTER the main program; their entry offsets are recorded here.
struct ClassFieldInfo {
    std::string name;
    bool isStatic = false;
    bool isPrivate = false;
    std::uint32_t initializerOffset = kNoOffset;  // evaluates the default value
};

struct ClassMethodInfo {
    std::string name;
    bool isStatic = false;
    bool isPrivate = false;
    bool isAbstract = false;
    bool isConstructor = false;
    std::vector<std::string> parameters;
    std::uint32_t entryOffset = kNoOffset;  // kNoOffset = abstract
};

struct ClassInfo {
    std::string name;
    std::uint32_t parentIndex = kNoParent;
    bool isAbstract = false;
    bool isInterface = false;
    std::vector<std::string> interfaceNames;  // implements list (validation)
    std::vector<ClassFieldInfo> fields;
    std::vector<ClassMethodInfo> methods;     // includes the constructor
};

// Native FFI type vocabulary as carried in bytecode. Values match the
// manifest metadata after validation.
namespace nativeTypes {
constexpr std::uint8_t Int8 = 1;
constexpr std::uint8_t UInt8 = 2;
constexpr std::uint8_t Int16 = 3;
constexpr std::uint8_t UInt16 = 4;
constexpr std::uint8_t Int32 = 5;
constexpr std::uint8_t UInt32 = 6;
constexpr std::uint8_t Int64 = 7;
constexpr std::uint8_t UInt64 = 8;
constexpr std::uint8_t Float32 = 9;
constexpr std::uint8_t Float64 = 10;
constexpr std::uint8_t Bool = 11;
constexpr std::uint8_t Char = 12;
constexpr std::uint8_t Pointer = 13;
constexpr std::uint8_t CString = 14;
constexpr std::uint8_t Void = 15;
}  // namespace nativeTypes

// One cimport'ed native function, resolved at compile time. The VM uses this
// at run time to pick the platform binary, look up the symbol and convert
// values across the boundary.
struct NativeFunctionInfo {
    std::string namespaceName;  // binding name (manifest "name")
    std::string name;           // Runa-side API name
    std::string symbol;         // exported C symbol
    std::string packageDir;     // relative dir from the native root: native/<name>
    // platform key -> binary path relative to packageDir (from the manifest)
    std::vector<std::pair<std::string, std::string>> targets;
    std::uint8_t returnType = nativeTypes::Void;
    std::vector<std::uint8_t> parameterTypes;
};

// One compilation unit's startup record. Units are stored in dependency order
// (dependencies first, the main unit last); the VM runs each unit's class
// static initializers and then its package-init code, in this order.
struct InitInfo {
    std::string packageName;  // "" for the main unit
    std::uint32_t initEntryOffset = kNoOffset;  // kNoOffset = main (no init body)
    std::uint32_t firstClassIndex = 0;
    std::uint32_t classCount = 0;  // classes owned by this unit (contiguous)
};

// One compiled unit of bytecode. `code` holds the main program (terminated by
// Halt) followed by every function body, then every class-related code block
// (field initializers, static initializers, methods, constructors) and then
// every package-init body.
struct Chunk {
    std::vector<std::uint8_t> code;   // instruction stream
    std::vector<Constant> constants;  // typed constant pool
    std::vector<std::string> names;   // name pool (variables + callee names)
    std::vector<FunctionInfo> functions;  // function registry (compile-time)
    std::vector<ClassInfo> classes;       // class registry (compile-time)
    std::vector<InitInfo> inits;          // per-unit startup order (v4)
    std::vector<NativeFunctionInfo> nativeFunctions;  // FFI registry (v4)

    std::uint32_t addStringConstant(const std::string& value);
    std::uint32_t addNumberConstant(std::int64_t value);
    std::uint32_t addFloatConstant(double value);
    std::uint32_t addBooleanConstant(bool value);
    std::uint32_t addName(std::string name);
    void writeOp(OpCode op);
    void writeU32(std::uint32_t value);
    void patchU32(std::size_t offset, std::uint32_t value);  // overwrite 4 LE bytes
};

// Development aid: prints one instruction at `offset` and returns the offset
// of the next instruction.
std::size_t formatInstructionAt(const Chunk& chunk, std::size_t offset, std::ostream& out);

// Development aid: prints a full human-readable disassembly of the chunk.
void disassemble(const Chunk& chunk, std::ostream& out);

// Binary serialization (provisional v1 format, see the file header).
void serialize(const Chunk& chunk, std::ostream& out);
Chunk deserialize(std::istream& in);  // throws std::runtime_error on bad input

}  // namespace runa::bytecode
