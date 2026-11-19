// Runa — Compiler (AST → Bytecode).
//
// The Compiler is completely separate from the VM and never executes anything
// (baseline spec, section 11). It walks the AST produced by the Parser and
// emits bytecode instructions for the Runa VM.
//
// Function system (v0.6):
//   - PASS 1 collects every top-level function declaration into a registry
//     (duplicate names, built-in-name conflicts and duplicate parameters are
//     CompileErrors). Because the registry is complete before any code is
//     emitted, functions may be called before they are declared in the file.
//   - PASS 2 emits the main program code (declarations contribute nothing
//     here — defining a function must not run anything) and Halt.
//   - PASS 3 emits each function body after the main code, records its entry
//     offset in the chunk's function table and appends an implicit
//     ReturnNone (a body that falls off the end produces "none").
//
// Class system (v0.7):
//   - PASS 1 also registers every class/interface (nested classes get a
//     qualified internal name "Car::Engine"), then validates the whole
//     hierarchy: parents (cycles, interfaces-as-parents), member duplicates,
//     modifier rules (abstract/override/static/private), override targets,
//     and — for every CONCRETE class — completeness of all inherited abstract
//     methods and implemented interface contracts. All of this happens before
//     any code is emitted, so 'new'/'is'/member access may appear anywhere.
//   - PASS 4 emits, per class (registration order): method bodies, the
//     constructor, one instance-field-initializer block (each field default
//     is evaluated and InitField into the new instance) and one static-field-
//     initializer block (StoreStatic per field). The VM chains these blocks
//     and the constructor together when executing 'new' and runs the static
//     blocks once at program start.
//
// Loop system (v0.8):
//   - 'loop <cond> [ body ]' lowers to:  condStart: <cond>; JumpIfFalse(end);
//     body; Jump(condStart); end: — the SAME conditional-jump shape the
//     if/elif/else chains already use. NO new opcodes are needed.
//   - 'for i = <start> to <end> [ body ]' lowers to: evaluate start → store
//     into i; evaluate end ONCE → store into a compiler-internal hidden slot
//     ('#for_endN#', impossible to spell in source); condStart: i <= end;
//     JumpIfFalse(end); body; incr: i = i + 1; Jump(condStart); end:.
//   - 'break'/'continue' compile to plain Jump instructions whose targets are
//     backpatched when the enclosing loop finishes (the same backpatching
//     style as if/elif/else). The compiler keeps a STACK of loop contexts,
//     so nested loops always patch the NEAREST loop; entering a function or
//     method body clears the stack, so break/continue can never cross a
//     function boundary — that is a CompileError, as is any break/continue
//     outside a loop.
//
// Contexts enforced here:
//   - `return` outside a function/method is a CompileError; `return <value>`
//     inside a constructor is a CompileError (constructors return nothing).
//   - nested declarations are CompileErrors: functions may not nest, and
//     class/interface declarations only live at the top level or directly
//     inside a class body.
//   - inside a function/method, assignments write LOCALS and reads check
//     locals first, then globals; at the top level, assignments write
//     globals — exactly as before.
//   - 'self' is only available in instance methods and constructors (never
//     in static methods or field initializers); 'super.name(...)' only in
//     instance methods/constructors of a class WITH a parent.
//   - Method calls on dynamic receivers are dispatched at RUNTIME (Runa has
//     no static types), but statically-known sites are validated here:
//     self./super./ClassName. member access, overload selection by argument
//     count, and private-member visibility (private members are accessible
//     only from the declaring class itself).
//   - Classes are not values: a class name cannot be assigned to, read or
//     called; instantiation goes through 'new' and static members through
//     'ClassName.member'.
#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/ast.hpp"
#include "bytecode/bytecode.hpp"
#include "errors/errors.hpp"

namespace runa::importer {
class ImportResolver;
struct LoadedPackage;
struct LoadedNativePackage;
struct NativeFunction;
}  // namespace runa::importer

namespace runa::compiler {

// Options for the whole-program (package-aware) compilation.
struct CompileOptions {
    // Directory of the main source file: the project root. cimport paths in
    // the main unit resolve against this directory.
    std::string projectRoot;
    // Directory where native packages live (and where RPR-embedded native
    // packages are extracted to): the native root for the VM.
    std::string nativeRoot;
    // Package resolver (search paths + RPR loading). When nullptr, any
    // import statement resolves against empty search paths (deterministic
    // "package not found" errors) and cimport resolves inside projectRoot.
    importer::ImportResolver* resolver = nullptr;
};

class Compiler {
public:
    // Single-unit compilation (kept for compatibility: programs without
    // package statements compile exactly as before).
    bytecode::Chunk compile(const ast::Program& program);
    // Whole-program compilation: resolves `import`/`cimport` declarations,
    // compiles every dependency unit and links them into one chunk.
    bytecode::Chunk compileProgram(const ast::Program& program,
                                   const CompileOptions& options);

private:
    // ---- package system state (v0.9) ---------------------------------------
    // One loaded package unit (a compiled-in library).
    struct PackageUnit {
        std::string name;      // package name ("" for the main unit)
        std::string prefix;    // "#pkg.math#" ("" for the main unit)
        std::unique_ptr<ast::Program> ownedProgram;  // packages own their AST
        const ast::Program* programRef = nullptr;    // the main unit borrows
        std::size_t firstClassIndex = 0;        // class range in the shared registry
        std::size_t classCount = 0;
        std::uint32_t initEntryOffset = bytecode::kNoOffset;

        const ast::Program& program() const {
            return ownedProgram != nullptr ? *ownedProgram : *programRef;
        }
    };

    // What an importer unit needs to know about an imported package.
    struct PackageView {
        std::string prefix;
        std::map<std::string, std::size_t> functions;  // name -> arity
        std::map<std::string, int> classes;            // name -> class index
    };

    // A selective-import binding: localName -> (package prefix, symbol).
    struct SelectiveBinding {
        std::string packageName;
        std::string prefix;
        std::string symbol;
        bool isClass = false;
        int classIndex = -1;
    };

    // Per-unit compile context (unit_ points at the active one).
    struct UnitContext {
        std::string prefix;              // "" for main
        std::string packageName;         // "" for main
        bool isMain = true;
        std::unordered_map<std::string, PackageView> imports;      // alias -> view
        std::unordered_map<std::string, SelectiveBinding> selective;
        // cimport namespaces: namespace -> (function name -> native table index)
        std::map<std::string, std::map<std::string, std::uint32_t>> cimports;
        std::set<std::string> cimportedPaths;  // duplicate cimport detection
        std::set<std::string> reservedNames;   // import/selective/cimport names
    };

    // Whole-program state used by compileProgram.
    std::vector<PackageUnit> units_;
    // Per-unit declaration registries (snapshots taken after registration;
    // activated per unit while emitting/validating that unit's code).
    std::vector<std::unordered_map<
        std::string, std::vector<const ast::FunctionDeclaration*>>>
        unitFunctions_;
    std::vector<std::unordered_map<std::string, int>> unitTopLevelClasses_;
    int activeUnitIndex_ = 0;
    std::map<std::string, std::size_t> unitsByPackage_;
    std::set<std::string> loadingPackages_;      // import-cycle detection
    std::vector<std::unique_ptr<const importer::LoadedPackage>> loadedPackages_;
    std::unordered_map<std::string, PackageView> packageViews_;
    std::vector<UnitContext> unitContexts_;
    UnitContext* unit_ = nullptr;                // active unit (nullptr = legacy)
    std::string projectRoot_;
    std::string nativeRoot_;
    importer::ImportResolver* resolver_ = nullptr;
    // Native FFI registry being built (moved into the chunk at the end).
    std::vector<bytecode::NativeFunctionInfo> nativeFunctionsBuffer_;

    // Renders a name with the active unit's hidden prefix ("" prefix = the
    // name is untouched, so import-free programs compile byte-identically).
    std::string unitName(const std::string& name) const;

    // Package-graph construction + binding.
    void buildUnitGraph(const ast::Program& mainProgram);
    void loadPackageUnit(const std::string& name, const errors::SourceLocation& at);
    void registerUnitDeclarations(PackageUnit& unit, int unitIndex);
    void bindUnitContexts();
    void bindNormalImport(UnitContext& context, const std::string& name,
                          const errors::SourceLocation& at);
    void bindSelectiveImport(UnitContext& context,
                             const ast::SelectiveImportStatement& statement,
                             std::set<std::string>& localNames);
    void bindCimport(UnitContext& context, const ast::CimportStatement& statement,
                     const importer::LoadedPackage* package,
                     std::set<std::string>& localNames);
    std::uint32_t registerNativeFunction(
        const importer::LoadedNativePackage& nativePackage,
        const importer::NativeFunction& function,
        const errors::SourceLocation& at);
    bool isReservedUnitName(const std::string& name) const;

    // Activates one unit's compile context + registries (emission/validation).
    void activateUnit(int unitIndex);
    // Emission entry points used per unit.
    void emitMainTopLevel(const ast::Program& program, bytecode::Chunk& chunk);
    void emitUnitInitBody(PackageUnit& unit, int unitIndex, bytecode::Chunk& chunk);
    void emitUnitFunctions(PackageUnit& unit, int unitIndex, bytecode::Chunk& chunk);

    // Import-aware resolution helper: resolves `alias.Member` chains rooted
    // at an imported package or a selectively imported class (e.g.
    // math.Vector / math.Vector::…). Returns true when the expression IS
    // package-rooted (classIndex then holds the resolved class); returns
    // false when it is not package-related, so the normal paths apply.
    bool resolvePackageClassPath(const ast::Expression& expression, int& classIndex,
                                 std::string& displayName) const;
    // Emits a native call after compile-time arity validation.
    void compileNativeCall(
        bytecode::Chunk& chunk, std::uint32_t nativeIndex,
        const std::vector<std::unique_ptr<ast::Expression>>& arguments,
        const ast::Node& at, const std::string& displayName);

    // ---- compiler-side class registry (pass 1) ---------------------------
    struct ClassField {
        std::string name;
        errors::SourceLocation location;
        bool isStatic = false;
        bool isPrivate = false;
        const ast::Expression* initializer = nullptr;
    };

    struct ClassMethod {
        std::string name;
        errors::SourceLocation location;
        bool isStatic = false;
        bool isPrivate = false;
        bool isAbstract = false;
        bool isOverride = false;
        bool isConstructor = false;
        const std::vector<ast::Parameter>* parameters = nullptr;
        const std::vector<std::unique_ptr<ast::Statement>>* body = nullptr;
    };

    struct ClassEntry {
        std::string name;            // simple name (Engine)
        std::string qualifiedName;   // Car::Engine (== name for top-level)
        const ast::ClassDeclaration* declaration = nullptr;
        int unitIndex = 0;           // owning compilation unit (package system)
        std::vector<ClassField> fields;
        std::vector<ClassMethod> methods;  // includes the constructor, if any
        std::vector<int> scopeChain;       // class indices, innermost (self) first
        std::set<std::string> nestedSimpleNames;  // immediate nested classes
        int parentIndex = -1;              // resolved parent (or -1)
        std::vector<int> interfaceIndices; // resolved implements list
    };

    struct ResolvedMember {
        int ownerIndex = -1;
        const ClassMethod* method = nullptr;
        const ClassField* field = nullptr;
    };

    // Pass-1 registry: function name → ALL its overloads (spec section 16:
    // the signature is Name + Arity, so same-name/different-arity functions
    // coexist; the call site picks by argument count). Complete before any
    // code is emitted, which makes forward references valid.
    std::unordered_map<std::string, std::vector<const ast::FunctionDeclaration*>>
        declaredFunctions_;
    // Class registry (registration order = bytecode class table order).
    std::vector<ClassEntry> classes_;
    std::unordered_map<std::string, int> classesByQualifiedName_;
    std::unordered_map<std::string, int> topLevelClassNames_;

    // ---- compilation context ---------------------------------------------
    // True while compiling a function/method/constructor body (and its nested
    // blocks): enables `return`, local variables and LoadVar/StoreLocal.
    bool inFunction_ = false;
    // The class whose member is being compiled (nullptr = not in a class).
    const ClassEntry* ownerClass_ = nullptr;
    // True while compiling a static method: 'self'/'super' are unavailable.
    bool inStaticMethod_ = false;
    // True while compiling a constructor: `return <value>` is an error.
    bool inConstructor_ = false;
    // if/elif/else block depth at the top level — class/interface
    // declarations are not allowed inside blocks or functions.
    int nesting_ = 0;

    // ---- loop compilation context ---------------------------------------
    // One entry per loop currently being compiled; the innermost loop is
    // last. break/continue always affect loopStack_.back() — this single
    // rule gives the nearest-loop discipline for free, and clearing the
    // stack when a function/method body starts forbids crossing boundaries.
    struct LoopContext {
        // Offsets of the u32 operands of 'break' jumps — patched to the first
        // instruction after the loop when the loop is done.
        std::vector<std::size_t> breakPatches;
        // Offsets of the u32 operands of 'continue' jumps — patched to the
        // continue target (condition re-evaluation for 'loop', the counter
        // increment for 'for') before the body is done.
        std::vector<std::size_t> continuePatches;
    };
    std::vector<LoopContext> loopStack_;
    // Serial number for compiler-internal for-loop temporaries. The '#'
    // prefix/suffix cannot appear in source identifiers, so these slots can
    // never collide with user variables.
    int forTempCounter_ = 0;

    // ---- passes ------------------------------------------------------------
    void registerFunctionDeclarations(const ast::Program& program);
    void registerClassDeclarations(const ast::Program& program);
    void registerClassDeclaration(const ast::ClassDeclaration& declaration,
                                  const std::vector<int>& enclosing);
    void validateClassHierarchy();
    void validateClassMembers(ClassEntry& entry);

    // ---- emission ----------------------------------------------------------
    void compileStatement(bytecode::Chunk& chunk, const ast::Statement& statement);
    void compileAssignment(bytecode::Chunk& chunk, const ast::AssignmentStatement& statement);
    void compileIf(bytecode::Chunk& chunk, const ast::IfStatement& statement);
    void compileLoop(bytecode::Chunk& chunk, const ast::LoopStatement& statement);
    void compileFor(bytecode::Chunk& chunk, const ast::ForStatement& statement);
    void compileBreak(bytecode::Chunk& chunk, const ast::BreakStatement& statement);
    void compileContinue(bytecode::Chunk& chunk, const ast::ContinueStatement& statement);
    // Read/write a variable slot with the current scope's rule: locals inside
    // functions/methods (LoadVar/StoreLocal), globals at the top level
    // (LoadGlobal/StoreGlobal). Used for the for-loop counter and its hidden
    // end-value slot.
    void storeVariableSlot(bytecode::Chunk& chunk, const std::string& name);
    void loadVariableSlot(bytecode::Chunk& chunk, const std::string& name);
    void compileFunctionDeclaration(bytecode::Chunk& chunk,
                                    const ast::FunctionDeclaration& declaration);
    void compileReturn(bytecode::Chunk& chunk, const ast::ReturnStatement& statement);
    void compileExpression(bytecode::Chunk& chunk, const ast::Expression& expression);
    void compileCallExpression(bytecode::Chunk& chunk, const ast::FunctionCallExpr& call);
    void compileMemberAccess(bytecode::Chunk& chunk, const ast::MemberAccessExpr& access);
    void compileMethodCall(bytecode::Chunk& chunk, const ast::MethodCallExpr& call);
    void compileNewObject(bytecode::Chunk& chunk, const ast::NewExpr& creation);
    void compileSuperCall(bytecode::Chunk& chunk, const ast::SuperCallExpr& call);
    void compileIsExpr(bytecode::Chunk& chunk, const ast::IsExpr& check);
    void compileCollectionLiteral(bytecode::Chunk& chunk,
                                  const ast::CollectionLiteral& literal);
    void compileMemberAssignmentTarget(bytecode::Chunk& chunk,
                                       const ast::MemberAccessExpr& target,
                                       const ast::Expression& value);
    void emitClassMachine(bytecode::Chunk& chunk);
    void emitClassMethods(bytecode::Chunk& chunk, bytecode::ClassInfo& info,
                          const ClassEntry& entry);
    void emitFieldInitializerBlock(bytecode::Chunk& chunk, bytecode::ClassInfo& info,
                                   const ClassEntry& entry);
    void emitStaticInitializerBlock(bytecode::Chunk& chunk, bytecode::ClassInfo& info,
                                    const ClassEntry& entry);

    // ---- registry helpers ----------------------------------------------------
    // Resolves a simple class name against the given scope chain (innermost
    // class first, then enclosing classes, then the top level).
    int resolveClass(const std::string& name, const std::vector<int>& scopeChain) const;
    // The scope chain for code compiled for `entry`'s members.
    std::vector<int> memberScopeChain() const;
    // Resolves "Outer::Inner::Deepest" from the GLOBAL scope (spec 51/52).
    int resolveQualifiedNestedClass(const std::string& qualified) const;
    bool isClassNameInScope(const std::string& name) const;

    // Walks the parent chain of `start` looking for a matching method.
    // includePrivate=false skips private members (visibility).
    ResolvedMember findMethodInChain(int start, const std::string& name,
                                     std::size_t arity, bool wantStatic,
                                     bool includePrivate) const;
    ResolvedMember findFieldInChain(int start, const std::string& name,
                                    bool wantStatic) const;
    int parentIndex(const ClassEntry& entry) const { return entry.parentIndex; }
    // Index of the class currently being compiled (-1 = none).
    int ownerClassIndex() const;

    // True if `name` is a declared user function or a built-in function name.
    bool isFunctionName(const std::string& name) const;
    bool isUserFunctionName(const std::string& name) const;

    // Core reserved names (printr / none): reject them in every binding
    // position (DECISIONS #114/#115).
    void ensureNotCoreReserved(const std::string& name, const ast::Node& at,
                               const std::string& binding);

    [[noreturn]] void fail(const ast::Node& node, const std::string& message);
};

}  // namespace runa::compiler
