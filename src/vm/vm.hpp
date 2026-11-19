// Runa — Virtual Machine.
//
// The VM executes Runa bytecode and nothing else. It never sees .ru files,
// tokens or the AST (baseline spec, section 13). The architecture is
// stack-based: instructions pop operands from an operand stack and the VM
// keeps global variables in runtime storage.
//
// Function system (v0.6): the VM owns the call machinery.
//   - At startup it builds a function registry from the chunk's function
//     table (name → parameters + entry offset).
//   - `Call <name> <argc>` resolves user functions first, then built-ins.
//     A user-function call pops the already-evaluated arguments, pushes a
//     NEW call frame (return address, stack base, independent locals map
//     with the parameters bound) and jumps to the function's entry offset.
//   - `ReturnValue` / `ReturnNone` unwind the current frame: the returned
//     value (or the distinct "none" value) is pushed onto the caller's
//     stack and execution resumes right after the Call instruction.
//   - Recursion works because every invocation gets its own frame — frames
//     never share locals, and no global mutable state tracks locals.
//   - The call stack depth is limited; runaway recursion is a clean runtime
//     error, never a crash.
//
// Class system (v0.7): the VM owns the object machinery.
//   - At startup it builds a CLASS registry from the chunk's class table:
//     for every class a merged instance-method table (name+arity → code,
//     nearest declaration wins) and static-method table, a field-owner map
//     (for private-access checks), the transitive interface set, and empty
//     static storage.
//   - STATIC fields are initialized once, before <main> runs: each class
//     with static fields has a compiled static-initializer block; the VM
//     runs the blocks in class registration order.
//   - `NewObject <class> <argc>` creates an Instance (reference-counted —
//     freed automatically when no longer reachable), then runs a PLAN:
//     base→derived field-initializer blocks followed by the constructor
//     (constructors are not inherited). Every plan step is a normal frame
//     flagged as part of the plan; when it returns, the VM advances the
//     plan instead of pushing a return value. The finished instance is
//     pushed when the last step completes.
//   - Method dispatch: InvokeVirtual looks up (name, argc) in the receiver's
//     RUNTIME class merged table — virtual dispatch for overrides and
//     polymorphism. InvokeSuper starts the lookup at the parent of the
//     class where the CURRENT method was declared, keeping self unchanged.
//     InvokeStatic calls class-level methods (no self).
//   - `self` lives in the frame (with the owner class for private-member
//     checks); private members are accessible only from the declaring class.
//   - `null` is a value of its own; member access on null is a clean
//     runtime error, never a crash. `===` compares object identity; `==`
//     compares structure (same class + equal fields, with a depth limit);
//     `is` walks the runtime class chain (and interface set).
#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "bytecode/bytecode.hpp"
#include "errors/errors.hpp"
#include "runtime/native/ffi_bridge.hpp"
#include "runtime/native/native_loader.hpp"
#include "runtime/runtime.hpp"

namespace runa::vm {

// Maximum number of live call frames (including <main>). Runaway recursion
// fails with a clean "call stack overflow" runtime error instead of exhausting
// memory. Provisional limit — a designer decision may change it.
constexpr std::size_t kMaxCallDepth = 10000;

// Cycle collection trigger (DECISIONS #118): the runtime collects after this
// many object allocations since the last pass. Deterministic and never
// user-directed.
constexpr std::size_t kGcAllocationThreshold = 8192;

// Maximum recursion depth for structural object equality (== on objects).
// Cyclic field chains (a.self = a) terminate with a clean runtime error.
constexpr int kMaxEqualityDepth = 64;

struct Options {
    // Development aid: print every executed instruction before it runs.
    bool trace = false;
    // Stream used for program output (built-ins like printr write here).
    // Defaults to std::cout when null. Tools may redirect it.
    std::ostream* output = nullptr;
    // Root directory for native (cimport'ed) packages: <nativeRoot>/<packageDir>/
    // <platformKey>/<binary>. Tools set it to the program artifact's directory.
    std::string nativeRoot;
};

// One step of an object-construction or static-initialization plan.
struct FramePlanStep {
    enum class Kind { FieldInit, StaticInit, Constructor, PackageInit };

    Kind kind;
    std::uint32_t classIndex = 0;
    std::uint32_t entryOffset = 0;  // code offset of the block / constructor
    std::string unitPrefix;         // package prefix (PackageInit frames)
    std::string packageName;        // package name (PackageInit frames)
};

// A queued sequence of frames (object construction or program startup).
// Plans are strictly LIFO: a nested 'new' inside a constructor pushes a plan
// that completes (and is removed) before the outer plan advances.
struct FramePlan {
    enum class Kind { Construction, Startup };

    Kind kind;
    std::shared_ptr<runtime::Instance> instance;  // construction target
    std::vector<runtime::Value> constructorArgs;  // popped by NewObject
    std::vector<FramePlanStep> steps;
    std::size_t nextStep = 0;
    std::size_t resumeIp = 0;  // where the creator resumes once finished
};

// One active function/method invocation. Owns everything an invocation needs
// to run and resume its caller: the resume address, the operand-stack floor
// (values below it belong to the caller), the function identity, the
// invocation's independent local variables (parameters included) and — for
// methods/constructors — the current object plus the class whose code is
// running (for private-member checks and 'super').
struct CallFrame {
    std::size_t returnIp = 0;   // caller resume point (after the Call operands)
    std::size_t stackBase = 0;  // operand stack floor for this frame
    std::string functionName;
    std::unordered_map<std::string, runtime::Value> locals;
    // The hidden '#pkg.<name>#' prefix of the unit this frame executes in ("")
    // for the main unit). LoadVar's global fallback also tries the prefixed
    // name, so a library function can read its package's top-level variables.
    std::string unitPrefix;

    // Class-system state.
    std::shared_ptr<runtime::Instance> self;  // null = no self in this frame
    int ownerClass = -1;                      // class index (visibility/super)
    // Plan participation: when true, a return advances plans_[planIndex]
    // instead of pushing a return value (field initializers, constructors).
    bool planMember = false;
    std::size_t planIndex = 0;
};

// VM-side runtime view of one compiled class.
struct ClassRuntime {
    std::string name;           // simple display name
    std::string qualifiedName;  // Car::Engine
    std::uint32_t parent = bytecode::kNoParent;
    bool isAbstract = false;
    bool isInterface = false;

    struct MethodRT {
        std::uint32_t owner = 0;      // declaring class index
        std::uint32_t entry = 0;      // bytecode entry offset
        bool isStatic = false;
        bool isPrivate = false;
        std::vector<std::string> parameters;
    };
    // Merged tables: walking the chain base→leaf, the NEAREST declaration of
    // a (name, arity) signature wins — precomputed so virtual dispatch is a
    // single map lookup from the receiver's runtime class.
    std::map<std::pair<std::string, std::size_t>, MethodRT> instanceMethods;
    std::map<std::pair<std::string, std::size_t>, MethodRT> staticMethods;

    struct FieldRT {
        std::uint32_t owner = 0;  // declaring class index
        bool isStatic = false;
        bool isPrivate = false;
    };
    std::unordered_map<std::string, FieldRT> fieldOwners;
    std::unordered_map<std::string, runtime::Value> staticStorage;

    std::vector<std::uint32_t> interfaces;  // transitive interface indices
    std::uint32_t firstInstanceFieldInit = bytecode::kNoOffset;
    std::uint32_t firstStaticFieldInit = bytecode::kNoOffset;
    // Own-class constructors by parameter count (overloads, spec 30).
    std::unordered_map<std::size_t, const bytecode::ClassMethodInfo*>
        constructorsByArity;
};

class VirtualMachine {
public:
    explicit VirtualMachine(Options options = {});

    void run(const bytecode::Chunk& chunk);
    const runtime::Globals& globals() const { return globals_; }

private:
    Options options_;
    std::vector<runtime::Value> stack_;   // operand stack (shared, frame-floored)
    runtime::Globals globals_;
    std::size_t ip_ = 0;                  // instruction pointer
    std::vector<CallFrame> frames_;       // call stack (frames_.back() = current)
    std::unordered_map<std::string,
                       std::vector<const bytecode::FunctionInfo*>>
        functions_;  // name -> overloads (spec 1.0 section 16: Name + Arity)
    std::vector<ClassRuntime> classes_;
    std::vector<FramePlan> plans_;        // strictly LIFO (see FramePlan)
    std::uint64_t nextObjectId_ = 1;      // display serial (Car#1, Car#2, ...)
    bool mainStarted_ = false;
    std::size_t allocationsSinceCollect_ = 0;  // cycle-collector trigger

    std::ostream& outputStream() const;
    std::uint32_t readU32(const bytecode::Chunk& chunk);
    void executeCall(const bytecode::Chunk& chunk);
    void executeArithmetic(bytecode::OpCode op);
    void executeBuildCollection(const bytecode::Chunk& chunk);
    void executeComparison(bytecode::OpCode op);
    void executeUnary(bytecode::OpCode op);
    void executeLogical(bytecode::OpCode op);
    void finishReturn(runtime::Value value);
    void advancePlan(std::size_t planIndex);
    void beginMain();

    // Class machinery.
    void buildClassRegistry(const bytecode::Chunk& chunk);
    void runStaticInitializers(const bytecode::Chunk& chunk);
    void executeNewObject(const bytecode::Chunk& chunk);
    void executeGetField(const bytecode::Chunk& chunk, const std::string& name);
    void executeSetField(const bytecode::Chunk& chunk, const std::string& name);
    void executeInitField(const bytecode::Chunk& chunk, const std::string& name);
    void executeInvokeVirtual(const bytecode::Chunk& chunk, const std::string& name,
                              std::uint32_t argc);
    void executeInvokeStatic(const bytecode::Chunk& chunk, std::uint32_t classIndex,
                             const std::string& name, std::uint32_t argc);
    void executeInvokeSuper(const bytecode::Chunk& chunk, const std::string& name,
                            std::uint32_t argc);
    void executeIsType(std::uint32_t classIndex);
    void pushMethodFrame(const ClassRuntime::MethodRT& method,
                         const std::string& displayName,
                         std::shared_ptr<runtime::Instance> self, int ownerClass,
                         std::vector<runtime::Value> args);

    // Structural object equality (== on objects) with cycle protection.
    bool valuesEqualDeep(const runtime::Value& a, const runtime::Value& b, int depth);

    // Cycle-safety context for object equality (DECISIONS #117): pairs of
    // instances currently being compared. Empty at every top-level entry.
    std::vector<std::pair<const runtime::Instance*, const runtime::Instance*>>
        equalityPairs_;

    // Cycle collection (Runa 1.0 lifetime, spec 58-60 + DECISIONS #118).
    void maybeCollectGarbage();
    void collectGarbage();

    // Native bridge (v0.9): resolve + load + call cimport'ed functions.
    void executeCallNative(const bytecode::Chunk& chunk);
    native::NativeLibrary ensureNativeLibrary(const bytecode::NativeFunctionInfo& fn);

    // Class-chain walk with cycle protection (malformed bytecode defense).
    std::vector<std::uint32_t> classChain(std::uint32_t index) const;

    [[noreturn]] void runtimeFail(const std::string& message) const;

    // Loaded native libraries, keyed by package dir (loaded once, lazily).
    std::unordered_map<std::string, native::NativeLibrary> loadedNatives_;
};

}  // namespace runa::vm
