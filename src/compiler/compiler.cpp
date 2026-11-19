#include "compiler/compiler.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <cstddef>
#include <unordered_set>

#include "import/importer.hpp"
#include "import/platform.hpp"
#include "import/json.hpp"
#include "import/manifest.hpp"
#include "import/rpr.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "runtime/native/ffi_bridge.hpp"
#include "stdlib/stdlib.hpp"

namespace runa::compiler {

using bytecode::Chunk;
using bytecode::OpCode;

// ============================================================================
// Package system (v0.9): whole-program compilation
// ============================================================================

std::string Compiler::unitName(const std::string& name) const {
    if (unit_ == nullptr || unit_->prefix.empty()) return name;
    return unit_->prefix + name;
}

bool Compiler::isReservedUnitName(const std::string& name) const {
    return unit_ != nullptr && unit_->reservedNames.count(name) > 0;
}

// Bytecode carries the FFI type as one byte; the values match the bridge's
// canonical FfiType enumeration 1:1 (checked by this explicit mapping).
static std::uint8_t ffiTypeToByte(native::FfiType type) {
    return static_cast<std::uint8_t>(type);
}

static native::FfiType manifestTypeToFfi(const std::string& type) {
    if (type == "int8") return native::FfiType::Int8;
    if (type == "uint8") return native::FfiType::UInt8;
    if (type == "int16") return native::FfiType::Int16;
    if (type == "uint16") return native::FfiType::UInt16;
    if (type == "int32") return native::FfiType::Int32;
    if (type == "uint32") return native::FfiType::UInt32;
    if (type == "int64") return native::FfiType::Int64;
    if (type == "uint64") return native::FfiType::UInt64;
    if (type == "float32") return native::FfiType::Float32;
    if (type == "float64") return native::FfiType::Float64;
    if (type == "bool") return native::FfiType::Bool;
    if (type == "char") return native::FfiType::Char;
    if (type == "pointer") return native::FfiType::Pointer;
    if (type == "cstring") return native::FfiType::CString;
    return native::FfiType::Void;
}

namespace {

// Reads a whole file; returns false when missing/unreadable (no exception).
bool readFileIfExists(const std::string& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    if (file.bad()) return false;
    out = buffer.str();
    return true;
}

}  // namespace

bytecode::Chunk Compiler::compile(const ast::Program& program) {
    CompileOptions options;  // no resolver: imports fail with a clean error
    return compileProgram(program, options);
}

bytecode::Chunk Compiler::compileProgram(const ast::Program& program,
                                         const CompileOptions& options) {
    bytecode::Chunk chunk;
    projectRoot_ = options.projectRoot;
    nativeRoot_ = options.nativeRoot;
    resolver_ = options.resolver;

    // Reset the whole-program state (a Compiler instance compiles once per
    // program, but stay safe against reuse).
    units_.clear();
    unitsByPackage_.clear();
    loadingPackages_.clear();
    loadedPackages_.clear();
    packageViews_.clear();
    unitContexts_.clear();
    nativeFunctionsBuffer_.clear();
    classes_.clear();
    classesByQualifiedName_.clear();
    declaredFunctions_.clear();
    topLevelClassNames_.clear();
    unit_ = nullptr;

    // ---- Phase A: unit graph (dependencies first, the main unit last) ------
    buildUnitGraph(program);
    unitContexts_.resize(units_.size());
    unitFunctions_.resize(units_.size());
    unitTopLevelClasses_.resize(units_.size());

    // ---- Phase B: per-unit declaration registration + class ranges ---------
    for (std::size_t i = 0; i < units_.size(); ++i) {
        registerUnitDeclarations(units_[i], static_cast<int>(i));
    }
    // Hierarchy validation runs on the MERGED registry: parents and
    // interfaces may live in other units (imported classes are resolvable).
    validateClassHierarchy();

    // ---- Phase C: bind import/cimport contexts ------------------------------
    bindUnitContexts();

    // ---- Phase D: emit -------------------------------------------------------
    // The VM enters at code offset 0, so the main unit's top-level code is
    // emitted first (Halt-terminated), then every package-init body, then
    // every unit's function bodies, then the class machinery.
    emitMainTopLevel(program, chunk);
    for (PackageUnit& unit : units_) {
        if (!unit.name.empty()) emitUnitInitBody(unit, static_cast<int>(&unit - units_.data()), chunk);
    }
    for (std::size_t u = 0; u < units_.size(); ++u) emitUnitFunctions(units_[u], static_cast<int>(u), chunk);
    emitClassMachine(chunk);

    // ---- Phase E: startup order + native table -------------------------------
    chunk.inits.reserve(units_.size());
    for (const PackageUnit& unit : units_) {
        bytecode::InitInfo info;
        info.packageName = unit.name;
        info.initEntryOffset = unit.initEntryOffset;
        info.firstClassIndex = static_cast<std::uint32_t>(unit.firstClassIndex);
        info.classCount = static_cast<std::uint32_t>(unit.classCount);
        chunk.inits.push_back(info);
    }
    chunk.nativeFunctions = std::move(nativeFunctionsBuffer_);
    unit_ = nullptr;
    return chunk;
}

// Loads one package unit (recursively, dependencies before dependents).
void Compiler::loadPackageUnit(const std::string& name,
                               const errors::SourceLocation& at) {
    if (unitsByPackage_.count(name) > 0) return;  // diamond dedup: load once
    if (loadingPackages_.count(name) > 0) {
        importer::failAt(at, "import cycle detected involving package '" + name + "'");
    }
    loadingPackages_.insert(name);

    if (resolver_ == nullptr) {
        importer::failAt(at, "package not found: '" + name + "' (no package search paths "
                   "are available in this compilation)");
    }
    auto loaded = std::make_unique<importer::LoadedPackage>(
        resolver_->loadPackage(name, at));

    // The entry source stays INSIDE the archive — nothing is extracted.
    std::string entrySource;
    if (!loaded->archive.readFile(loaded->manifest.entry, entrySource)) {
        importer::failAt(at, "library entry '" + loaded->manifest.entry +
                       "' not found in package '" + name + "'");
    }

    // Parse the entry as its own compilation unit. Errors inside the package
    // carry the package file label (math.rpr/init.ru:line:col).
    const std::string label = name + ".rpr/" + loaded->manifest.entry;
    std::unique_ptr<ast::Program> entryProgram;
    try {
        lexer::Lexer entryLexer(entrySource, label);
        std::vector<lexer::Token> entryTokens = entryLexer.tokenize();
        parser::Parser entryParser(std::move(entryTokens));
        entryProgram = std::make_unique<ast::Program>(entryParser.parseProgram());
    } catch (const errors::RunaErrorException&) {
        throw;
    }

    // Recurse into the package's own imports BEFORE registering it, so every
    // dependency unit precedes its dependents in units_ (topological order).
    for (const auto& statement : entryProgram->statements) {
        if (statement->kind == ast::NodeKind::ImportStatement) {
            loadPackageUnit(
                static_cast<const ast::ImportStatement&>(*statement).packageName,
                statement->location);
        } else if (statement->kind == ast::NodeKind::SelectiveImportStatement) {
            loadPackageUnit(
                static_cast<const ast::SelectiveImportStatement&>(*statement).packageName,
                statement->location);
        }
    }

    // Export view: functions are known from the AST immediately (classes are
    // filled after registration, when class indices exist).
    PackageView view;
    view.prefix = "#pkg." + name + "#";
    for (const auto& statement : entryProgram->statements) {
        if (statement->kind == ast::NodeKind::FunctionDeclaration) {
            const auto& declaration =
                static_cast<const ast::FunctionDeclaration&>(*statement);
            view.functions[declaration.name] = declaration.parameters.size();
        }
    }

    PackageUnit unit;
    unit.name = name;
    unit.prefix = view.prefix;
    unit.ownedProgram = std::move(entryProgram);
    units_.push_back(std::move(unit));
    unitsByPackage_[name] = units_.size() - 1;
    packageViews_[name] = std::move(view);

    loadedPackages_.push_back(std::move(loaded));
    loadingPackages_.erase(name);
}

void Compiler::buildUnitGraph(const ast::Program& mainProgram) {
    // The main unit carries no prefix; its dependencies are loaded first so
    // they take the earlier slots in units_ (and in the class registry).
    for (const auto& statement : mainProgram.statements) {
        if (statement->kind == ast::NodeKind::ImportStatement) {
            loadPackageUnit(
                static_cast<const ast::ImportStatement&>(*statement).packageName,
                statement->location);
        } else if (statement->kind == ast::NodeKind::SelectiveImportStatement) {
            loadPackageUnit(
                static_cast<const ast::SelectiveImportStatement&>(*statement).packageName,
                statement->location);
        }
    }
    PackageUnit mainUnit;
    mainUnit.name = "";
    mainUnit.prefix = "";
    mainUnit.programRef = &mainProgram;
    units_.push_back(std::move(mainUnit));
}

void Compiler::registerUnitDeclarations(PackageUnit& unit, int unitIndex) {
    unit.firstClassIndex = classes_.size();
    unit_ = &unitContexts_[static_cast<std::size_t>(unitIndex)];
    unit_->prefix = unit.prefix;
    unit_->packageName = unit.name;
    unit_->isMain = unit.name.empty();
    // Registries are per-unit: two packages may both declare 'helper'.
    declaredFunctions_.clear();
    topLevelClassNames_.clear();
    registerFunctionDeclarations(unit.program());
    registerClassDeclarations(unit.program());
    unit.classCount = classes_.size() - unit.firstClassIndex;
    // Snapshot the unit's registries for the emission/validation phases and
    // the package's export view.
    unitFunctions_[static_cast<std::size_t>(unitIndex)] = declaredFunctions_;
    unitTopLevelClasses_[static_cast<std::size_t>(unitIndex)] = topLevelClassNames_;
    if (!unit.name.empty()) {
        PackageView& view = packageViews_[unit.name];
        for (const auto& entry : topLevelClassNames_) {
            view.classes[entry.first] = entry.second;
        }
    }
    unit_ = nullptr;
}

void Compiler::activateUnit(int unitIndex) {
    unit_ = &unitContexts_[static_cast<std::size_t>(unitIndex)];
    activeUnitIndex_ = unitIndex;
    declaredFunctions_ = unitFunctions_[static_cast<std::size_t>(unitIndex)];
    topLevelClassNames_ = unitTopLevelClasses_[static_cast<std::size_t>(unitIndex)];
}

// ---- binding: make every import/cimport of a unit resolvable ----------------

void Compiler::bindUnitContexts() {
    for (std::size_t i = 0; i < units_.size(); ++i) {
        PackageUnit& unit = units_[i];
        UnitContext& context = unitContexts_[i];
        unit_ = &context;
        context.prefix = unit.prefix;
        context.packageName = unit.name;
        context.isMain = unit.name.empty();

        // Unit-local names that import bindings must not shadow.
        std::set<std::string> localNames;
        for (const auto& statement : unit.program().statements) {
            if (statement->kind == ast::NodeKind::FunctionDeclaration) {
                localNames.insert(
                    static_cast<const ast::FunctionDeclaration&>(*statement).name);
            } else if (statement->kind == ast::NodeKind::ClassDeclaration) {
                localNames.insert(
                    static_cast<const ast::ClassDeclaration&>(*statement).name);
            }
        }

        for (const auto& statement : unit.program().statements) {
            switch (statement->kind) {
                case ast::NodeKind::ImportStatement: {
                    const auto& importStatement =
                        static_cast<const ast::ImportStatement&>(*statement);
                    bindNormalImport(context, importStatement.packageName,
                                     statement->location);
                    break;
                }
                case ast::NodeKind::SelectiveImportStatement: {
                    const auto& selective =
                        static_cast<const ast::SelectiveImportStatement&>(*statement);
                    bindNormalImport(context, selective.packageName,
                                     statement->location);
                    bindSelectiveImport(context, selective, localNames);
                    break;
                }
                case ast::NodeKind::CimportStatement: {
                    const importer::LoadedPackage* package = nullptr;
                    if (!unit.name.empty()) {
                        package = loadedPackages_[i].get();
                    }
                    bindCimport(context,
                                static_cast<const ast::CimportStatement&>(*statement),
                                package, localNames);
                    break;
                }
                default:
                    break;
            }
        }
        unit_ = nullptr;
    }
}

void Compiler::bindNormalImport(UnitContext& context, const std::string& name,
                                const errors::SourceLocation& at) {
    if (context.imports.count(name) > 0) {
        // Re-importing the same package is IDEMPOTENT: 'import math' plus
        // 'as "math" import { sqrt }' coexist (namespace + selective
        // bindings). Duplicate SELECTIVE symbols still error (see
        // bindSelectiveImport). Documented in DECISIONS.md.
        return;
    }
    const auto view = packageViews_.find(name);
    if (view == packageViews_.end()) {
        importer::failAt(at, "internal error: package '" + name + "' was not loaded");
    }
    // Binding-name collision checks (imports bind the package name).
    if (stdlib::findBuiltin(name) != nullptr) {
        importer::failAt(at, "cannot import '" + name +
                       "' — the name is already used by a built-in function");
    }
    if (name == "none") {
        importer::failAt(at, "cannot import 'none' — none is not a source literal "
                             "in Runa");
    }
    if (context.reservedNames.count(name) > 0 ||
        context.selective.count(name) > 0 || context.cimports.count(name) > 0) {
        importer::failAt(at, "cannot import '" + name +
                       "' — the name is already used by another import in this unit");
    }
    context.imports.emplace(name, view->second);
    context.reservedNames.insert(name);
}

void Compiler::bindSelectiveImport(UnitContext& context,
                                   const ast::SelectiveImportStatement& statement,
                                   std::set<std::string>& localNames) {
    const auto view = packageViews_.find(statement.packageName);
    if (view == packageViews_.end()) {
        importer::failAt(statement.nameLocation,
               "internal error: package '" + statement.packageName + "' was not loaded");
    }
    for (const auto& symbol : statement.symbols) {
        SelectiveBinding binding;
        binding.packageName = statement.packageName;
        binding.prefix = view->second.prefix;
        binding.symbol = symbol.name;

        const auto fn = view->second.functions.find(symbol.name);
        const auto cls = view->second.classes.find(symbol.name);
        if (fn != view->second.functions.end()) {
            binding.isClass = false;
        } else if (cls != view->second.classes.end()) {
            binding.isClass = true;
            binding.classIndex = cls->second;
        } else {
            importer::failAt(symbol.location,
                   "imported symbol not found: '" + statement.packageName + "." +
                       symbol.name + "' (the package does not export it)");
        }

        // Collision discipline (deterministic, in declaration order).
        if (localNames.count(symbol.name) > 0) {
            importer::failAt(symbol.location,
                   "cannot import '" + symbol.name + "' — the name is already used " +
                       "by a function or class in this unit");
        }
        if (stdlib::findBuiltin(symbol.name) != nullptr) {
            importer::failAt(symbol.location,
                   "cannot import '" + symbol.name +
                       "' — the name is already used by a built-in function");
        }
        if (symbol.name == "none") {
            importer::failAt(symbol.location,
                   "cannot import 'none' — none is not a source literal in Runa");
        }
        if (context.reservedNames.count(symbol.name) > 0 ||
            context.selective.count(symbol.name) > 0) {
            importer::failAt(symbol.location,
                   "duplicate import binding '" + symbol.name +
                       "' (it is already provided by another import)");
        }
        context.selective.emplace(symbol.name, std::move(binding));
        context.reservedNames.insert(symbol.name);
    }
}

std::uint32_t Compiler::registerNativeFunction(
    const importer::LoadedNativePackage& nativePackage,
    const importer::NativeFunction& function, const errors::SourceLocation& at) {
    bytecode::NativeFunctionInfo info;
    info.namespaceName = nativePackage.manifest.name;
    info.name = function.name;
    info.symbol = function.symbol;
    info.packageDir = nativePackage.packageDir;
    info.targets = nativePackage.manifest.targets;

    // Compile-time FFI capability enforcement: every type the bridge really
    // executes is allowed; anything else fails here, never at run time.
    // (float32/float64 RETURNS are supported — they produce the new Float
    // value kind; float PARAMETERS are rejected below.)
    const native::FfiType returnFfi = manifestTypeToFfi(function.returnType);
    info.returnType = ffiTypeToByte(returnFfi);
    for (const std::string& parameterType : function.parameterTypes) {
        const native::FfiType ffi = manifestTypeToFfi(parameterType);
        if (ffi == native::FfiType::Float32 || ffi == native::FfiType::Float64) {
            importer::failAt(at, "native function '" + nativePackage.manifest.name + "." +
                           function.name + "': parameter type '" + parameterType +
                           "' is not supported by this version of the toolchain "
                           "(float parameters are planned for a future version; "
                           "float return values are supported)");
        }
        info.parameterTypes.push_back(ffiTypeToByte(ffi));
    }
    nativeFunctionsBuffer_.push_back(std::move(info));
    return static_cast<std::uint32_t>(nativeFunctionsBuffer_.size() - 1);
}

void Compiler::bindCimport(UnitContext& context, const ast::CimportStatement& statement,
                           const importer::LoadedPackage* package,
                           std::set<std::string>& localNames) {
    const std::string path = statement.joinedPath();
    if (context.cimportedPaths.count(path) > 0) {
        importer::failAt(statement.location, "duplicate cimport of native package '" + path + "'");
    }
    context.cimportedPaths.insert(path);

    if (resolver_ == nullptr) {
        importer::failAt(statement.location,
               "cimport requires a package resolver (compile with the runa toolchain)");
    }

    // Context-aware resolution: project context for the main unit, package
    // context (inside the RPR) for library units. Never an OS-absolute path.
    importer::LoadedNativePackage nativePackage =
        (package == nullptr)
            ? resolver_->loadProjectNativePackage(path, projectRoot_, statement.location)
            : resolver_->loadEmbeddedNativePackage(path, *package, statement.location);

    // The binding name is the manifest's "name" (a validated identifier).
    const std::string& ns = nativePackage.manifest.name;
    if (localNames.count(ns) > 0) {
        importer::failAt(statement.location,
               "cannot cimport '" + path + "' — the binding name '" + ns +
                   "' is already used by a function or class in this unit");
    }
    if (stdlib::findBuiltin(ns) != nullptr) {
        importer::failAt(statement.location,
               "cannot cimport '" + path +
                   "' — the binding name '" + ns + "' is already used by a built-in");
    }
    if (context.reservedNames.count(ns) > 0 || context.selective.count(ns) > 0 ||
        context.imports.count(ns) > 0 || context.cimports.count(ns) > 0) {
        importer::failAt(statement.location,
               "cannot cimport '" + path +
                   "' — the binding name '" + ns + "' is already used in this unit");
    }

    // Current-platform target must exist NOW (early, precise failure). At
    // run time the VM re-resolves deterministically and re-checks.
    const std::string platformKey = platform::nativeTargetKey();
    const std::string* targetPath = nullptr;
    for (const auto& target : nativePackage.manifest.targets) {
        if (target.first == platformKey) {
            targetPath = &target.second;
            break;
        }
    }
    if (targetPath == nullptr) {
        std::string available;
        for (const auto& target : nativePackage.manifest.targets) {
            available += (available.empty() ? "" : ", ") + target.first;
        }
        if (available.empty()) available = "(none declared)";
        importer::failAt(statement.location,
               "native target not found for platform '" + platformKey +
                   "' in package '" + path + "' (available: " + available + ")");
    }
    if (nativePackage.diskRoot.empty() && package != nullptr) {
        // Embedded package: the binary must exist inside the archive, and the
        // native package is extracted once to the native root (dlopen needs a
        // real file; Runa code itself is never extracted).
        std::string binary;
        if (!package->archive.readFile(nativePackage.packageDir + "/" + *targetPath,
                                       binary)) {
            importer::failAt(statement.location,
                   "native binary missing inside " + package->archivePath + ": " +
                       nativePackage.packageDir + "/" + *targetPath);
        }
        if (nativeRoot_.empty()) {
            importer::failAt(statement.location,
                   "native root is not configured for this compilation (cannot "
                   "place the native package '" + path + "')");
        }
        // Extract the package's files PRESERVING the package dir layout, so
        // the runtime finds them at <nativeRoot>/<packageDir>/<target>.
        importer::extractArchivePrefix(package->archive, nativePackage.packageDir,
                                       nativeRoot_ + "/" + nativePackage.packageDir);
    } else if (!nativePackage.diskRoot.empty()) {
        std::string binary;
        if (!readFileIfExists(nativePackage.diskRoot + "/" + *targetPath, binary)) {
            importer::failAt(statement.location,
                   "native binary missing: " + nativePackage.diskRoot + "/" +
                       *targetPath);
        }
    }

    // Register every API function (validating FFI capability per function).
    std::map<std::string, std::uint32_t> functionIndices;
    for (const importer::NativeFunction& function : nativePackage.manifest.functions) {
        functionIndices[function.name] =
            registerNativeFunction(nativePackage, function, statement.location);
    }
    context.cimports.emplace(ns, std::move(functionIndices));
    context.reservedNames.insert(ns);
}

// ---- emission per unit -------------------------------------------------------

void Compiler::emitMainTopLevel(const ast::Program& program, Chunk& chunk) {
    // The main unit is the LAST one; its top-level statements form the main
    // code (offset 0). Declarations and imports contribute nothing here.
    activateUnit(static_cast<int>(units_.size()) - 1);
    for (const auto& statement : program.statements) {
        const ast::NodeKind kind = statement->kind;
        if (kind == ast::NodeKind::FunctionDeclaration ||
            kind == ast::NodeKind::ClassDeclaration ||
            kind == ast::NodeKind::ImportStatement ||
            kind == ast::NodeKind::SelectiveImportStatement ||
            kind == ast::NodeKind::CimportStatement) {
            continue;
        }
        compileStatement(chunk, *statement);
    }
    chunk.writeOp(OpCode::Halt);
    unit_ = nullptr;
}

// A package's top-level statements run ONCE at startup, in dependency order.
// They compile in GLOBAL mode (inFunction_ stays false): assignments create
// package globals (hidden '#pkg.<name>#...' slots), never init-body locals.
void Compiler::emitUnitInitBody(PackageUnit& unit, int unitIndex, Chunk& chunk) {
    activateUnit(unitIndex);
    bytecode::FunctionInfo info;
    info.name = "#init#" + unit.name + "#";
    info.entryOffset = static_cast<std::uint32_t>(chunk.code.size());
    chunk.functions.push_back(std::move(info));
    unit.initEntryOffset = info.entryOffset;

    for (const auto& statement : unit.program().statements) {
        const ast::NodeKind kind = statement->kind;
        if (kind == ast::NodeKind::FunctionDeclaration ||
            kind == ast::NodeKind::ClassDeclaration ||
            kind == ast::NodeKind::ImportStatement ||
            kind == ast::NodeKind::SelectiveImportStatement ||
            kind == ast::NodeKind::CimportStatement) {
            continue;
        }
        compileStatement(chunk, *statement);
    }
    chunk.writeOp(OpCode::ReturnNone);
    unit_ = nullptr;
}

void Compiler::emitUnitFunctions(PackageUnit& unit, int unitIndex, Chunk& chunk) {
    activateUnit(unitIndex);
    for (const auto& statement : unit.program().statements) {
        if (statement->kind != ast::NodeKind::FunctionDeclaration) continue;
        compileFunctionDeclaration(
            chunk, static_cast<const ast::FunctionDeclaration&>(*statement));
    }
    unit_ = nullptr;
}

// ============================================================================
// Import-aware resolution helpers
// ============================================================================

// Resolves `alias.A.B` chains rooted at an imported package or a selectively
// imported class. Walks the segments: every segment except the last must be
// a top-level class exported by the package; the LAST segment names the
// class we resolve (callers use it for statics / 'new' / 'is').
bool Compiler::resolvePackageClassPath(const ast::Expression& expression,
                                       int& classIndex,
                                       std::string& displayName) const {
    if (unit_ == nullptr) return false;

    // Unpack the member chain into (root, segments...).
    std::string root;
    std::vector<std::string> segments;
    const ast::Expression* current = &expression;
    while (current->kind == ast::NodeKind::MemberAccessExpr) {
        const auto& access = static_cast<const ast::MemberAccessExpr&>(*current);
        segments.push_back(access.memberName);
        current = access.object.get();
    }
    if (current->kind != ast::NodeKind::Identifier) return false;
    root = static_cast<const ast::Identifier&>(*current).name;
    std::reverse(segments.begin(), segments.end());
    if (segments.empty()) return false;  // bare identifier: not a class path

    // Case 1: root is an imported package (math.Vector.count).
    const auto imported = unit_->imports.find(root);
    if (imported != unit_->imports.end()) {
        const PackageView& view = imported->second;
        // Every segment except the last must resolve to a class.
        int currentClass = -1;
        std::string pathSoFar = root;
        for (std::size_t i = 0; i + 1 < segments.size(); ++i) {
            const std::string& segment = segments[i];
            pathSoFar += "." + segment;
            if (currentClass < 0) {
                const auto cls = view.classes.find(segment);
                if (cls == view.classes.end()) {
                    if (view.functions.count(segment) > 0) {
                        importer::failAt(expression.location,
                               "'" + pathSoFar + "' is a function — functions are "
                               "not values in Runa yet");
                    }
                    importer::failAt(expression.location,
                           "package '" + root + "' has no class '" + segment + "'");
                }
                currentClass = cls->second;
            } else {
                importer::failAt(expression.location,
                       "nested classes of an imported class are not accessible "
                       "('" + pathSoFar + "')");
            }
        }
        // The last segment must be a top-level class of the package.
        const auto cls = view.classes.find(segments.back());
        if (cls == view.classes.end()) {
            if (view.functions.count(segments.back()) > 0) {
                importer::failAt(expression.location,
                       "'" + root + "." + segments.back() +
                           "' is a function — functions are not values in Runa yet");
            }
            importer::failAt(expression.location,
                   "package '" + root + "' has no class '" + segments.back() + "'");
        }
        classIndex = cls->second;
        displayName = root + "." + segments.back();
        return true;
    }

    // Case 2: root is a selectively imported class (Vector.make(...)).
    const auto selective = unit_->selective.find(root);
    if (selective != unit_->selective.end() && selective->second.isClass) {
        if (segments.size() > 1) {
            importer::failAt(expression.location,
                   "nested classes of an imported class are not accessible ('" +
                       root + "." + segments.front() + "." + segments.back() + "')");
        }
        classIndex = selective->second.classIndex;
        displayName = root;
        return true;
    }

    // Case 3: root is a cimport namespace — namespaces expose functions only.
    if (unit_->cimports.count(root) > 0) {
        importer::failAt(expression.location,
               "'" + root + "' is a native package — it has no classes (native "
               "packages expose functions only)");
    }
    return false;
}

void Compiler::compileNativeCall(
    Chunk& chunk, std::uint32_t nativeIndex,
    const std::vector<std::unique_ptr<ast::Expression>>& arguments,
    const ast::Node& at, const std::string& displayName) {
    if (nativeIndex >= nativeFunctionsBuffer_.size()) {
        fail(at, "internal error: native function index out of range");
    }
    const bytecode::NativeFunctionInfo& info = nativeFunctionsBuffer_[nativeIndex];
    if (info.parameterTypes.size() != arguments.size()) {
        fail(at, "'" + displayName + "' expects " +
                     std::to_string(info.parameterTypes.size()) +
                     " argument(s), got " + std::to_string(arguments.size()));
    }
    for (const auto& argument : arguments) {
        compileExpression(chunk, *argument);
    }
    chunk.writeOp(OpCode::CallNative);
    chunk.writeU32(nativeIndex);
    chunk.writeU32(static_cast<std::uint32_t>(arguments.size()));
}

// ============================================================================
// PASS 1: registries
// ============================================================================

void Compiler::registerFunctionDeclarations(const ast::Program& program) {
    for (const auto& statement : program.statements) {
        if (statement->kind != ast::NodeKind::FunctionDeclaration) continue;
        const auto& declaration =
            static_cast<const ast::FunctionDeclaration&>(*statement);

        // Runa 1.0 (spec section 16): overloads are Name + Arity. Same name
        // with a DIFFERENT parameter count is a new overload; the same name
        // and arity is a duplicate.
        for (const auto* existing : declaredFunctions_[declaration.name]) {
            if (existing->parameters.size() == declaration.parameters.size()) {
                fail(declaration, "duplicate function '" + declaration.name + "' (" +
                                      std::to_string(declaration.parameters.size()) +
                                      " parameter(s)) — overloads must differ in arity");
            }
        }
        // NOTE: a user function MAY reuse a built-in name (sum, empty, ...).
        // Call resolution prefers the user function (the VM checks the user
        // registry first), so pre-collection programs that declare their own
        // `empty`/`sum`/... keep working; the collection built-ins are simply
        // shadowed in that unit. (Collection System compatibility decision.)
        // Top-level class names are reserved identifiers too.
        if (topLevelClassNames_.count(declaration.name) > 0) {
            fail(declaration, "cannot define function '" + declaration.name +
                                  "' — the name is already used by a class");
        }
        std::unordered_set<std::string> parameterNames;
        for (const ast::Parameter& parameter : declaration.parameters) {
            if (!parameterNames.insert(parameter.name).second) {
                fail(declaration, "duplicate parameter name '" + parameter.name +
                                      "' in function '" + declaration.name + "'");
            }
            ensureNotCoreReserved(parameter.name, declaration, "a parameter name");
        }
        ensureNotCoreReserved(declaration.name, declaration, "a function name");
        declaredFunctions_[declaration.name].push_back(&declaration);
    }
}

void Compiler::registerClassDeclarations(const ast::Program& program) {
    for (const auto& statement : program.statements) {
        if (statement->kind != ast::NodeKind::ClassDeclaration) continue;
        registerClassDeclaration(
            static_cast<const ast::ClassDeclaration&>(*statement), {});
    }
}

// Registers one class (or interface) and recursively its nested classes.
// `enclosing` holds the class indices of the enclosing classes (outermost
// first); the new class extends that chain with itself.
void Compiler::registerClassDeclaration(const ast::ClassDeclaration& declaration,
                                        const std::vector<int>& enclosing) {
    // Build the qualified name from the enclosing entries. Package units
    // prefix their top-level classes with the hidden '#pkg.x#' marker.
    std::string qualified = enclosing.empty()
                                ? unitName(declaration.name)
                                : classes_[static_cast<std::size_t>(enclosing.back())]
                                          .qualifiedName +
                                      "::" + declaration.name;

    if (classesByQualifiedName_.count(qualified) > 0) {
        fail(declaration, "duplicate class '" + declaration.name +
                              "' (a class with this name is already defined in the "
                              "same scope)");
    }
    if (enclosing.empty() && stdlib::findBuiltin(declaration.name) != nullptr) {
        fail(declaration, "cannot define class '" + declaration.name +
                              "' — the name is already used by a built-in function");
    }

    const int index = static_cast<int>(classes_.size());
    ClassEntry newEntry;
    newEntry.name = declaration.name;
    newEntry.qualifiedName = qualified;
    newEntry.declaration = &declaration;
    newEntry.unitIndex = activeUnitIndex_;
    newEntry.scopeChain = enclosing;
    newEntry.scopeChain.push_back(index);
    classes_.push_back(std::move(newEntry));
    classesByQualifiedName_[qualified] = index;
    if (enclosing.empty()) {
        topLevelClassNames_[declaration.name] = index;
    } else {
        classes_[static_cast<std::size_t>(enclosing.back())]
            .nestedSimpleNames.insert(declaration.name);
    }

    // Split members into fields/methods and collect nested classes. Nested
    // classes recurse AFTER the member loop: recursion may reallocate
    // classes_, which would invalidate a live `entry` reference.
    std::vector<const ast::ClassDeclaration*> nestedDeclarations;
    ClassEntry& entry = classes_[static_cast<std::size_t>(index)];
    for (const ast::ClassMember& member : declaration.members) {
        switch (member.kind) {
            case ast::ClassMember::Kind::Field: {
                ClassField field;
                field.name = member.name;
                field.location = member.nameLocation;
                field.isStatic = member.isStatic;
                field.isPrivate = member.isPrivate;
                field.initializer = member.initializer.get();
                entry.fields.push_back(std::move(field));
                break;
            }
            case ast::ClassMember::Kind::Method: {
                ClassMethod method;
                method.name = member.name;
                method.location = member.nameLocation;
                method.isStatic = member.isStatic;
                method.isPrivate = member.isPrivate;
                method.isAbstract = member.isAbstract;
                method.isOverride = member.isOverride;
                method.parameters = &member.parameters;
                method.body = &member.body;
                entry.methods.push_back(std::move(method));
                break;
            }
            case ast::ClassMember::Kind::Constructor: {
                ClassMethod method;
                method.name = "constructor";
                method.location = member.nameLocation;
                method.isConstructor = true;
                method.parameters = &member.parameters;
                method.body = &member.body;
                entry.methods.push_back(std::move(method));
                break;
            }
            case ast::ClassMember::Kind::NestedClass:
                nestedDeclarations.push_back(member.nested.get());
                break;
        }
    }
    const std::vector<int> scopeChainCopy = entry.scopeChain;
    for (const ast::ClassDeclaration* nested : nestedDeclarations) {
        registerClassDeclaration(*nested, scopeChainCopy);
    }
}

// ============================================================================
// PASS 1b: hierarchy + member validation
// ============================================================================

void Compiler::validateClassHierarchy() {
    for (std::size_t i = 0; i < classes_.size(); ++i) {
        ClassEntry& entry = classes_[i];
        const ast::ClassDeclaration& declaration = *entry.declaration;
        // Cross-unit registry: every class validates against ITS unit's
        // symbol tables (parents/interfaces resolve per unit).
        activateUnit(entry.unitIndex);

        // ---- flags --------------------------------------------------------
        if (entry.declaration->isInterface && entry.declaration->isAbstract) {
            fail(declaration, "an interface cannot be abstract");
        }

        // ---- parent --------------------------------------------------------
        if (!declaration.parentName.empty()) {
            // The parent resolves against the ENCLOSING scope (not the
            // class's own nested classes).
            std::vector<int> enclosing(entry.scopeChain.begin(),
                                       entry.scopeChain.end() - 1);
            const int parentIdx = resolveClass(declaration.parentName, enclosing);
            if (parentIdx < 0) {
                fail(declaration, "unknown parent class '" + declaration.parentName +
                                      "' for class '" + entry.qualifiedName + "'");
            }
            if (classes_[static_cast<std::size_t>(parentIdx)].declaration->isInterface) {
                fail(declaration, "class '" + entry.qualifiedName +
                                      "' cannot inherit from interface '" +
                                      declaration.parentName + "'");
            }
            // Cycle check: walk the (already resolved) parent chain.
            int cur = parentIdx;
            while (cur != -1) {
                if (cur == static_cast<int>(i)) {
                    fail(declaration, "inheritance cycle detected involving class '" +
                                          entry.qualifiedName + "'");
                }
                cur = classes_[static_cast<std::size_t>(cur)].parentIndex;
            }
            entry.parentIndex = parentIdx;
        }

        // ---- interfaces -----------------------------------------------------
        std::unordered_set<std::string> seenInterfaces;
        for (const std::string& interfaceName : declaration.interfaces) {
            const int interfaceIdx = resolveClass(interfaceName, entry.scopeChain);
            if (interfaceIdx < 0) {
                fail(declaration, "unknown interface '" + interfaceName +
                                      "' in the implements list of class '" +
                                      entry.qualifiedName + "'");
            }
            if (!classes_[static_cast<std::size_t>(interfaceIdx)]
                     .declaration->isInterface) {
                fail(declaration, "class '" + entry.qualifiedName +
                                      "' cannot implement '" + interfaceName +
                                      "' — it is not an interface");
            }
            if (!seenInterfaces.insert(interfaceName).second) {
                fail(declaration, "duplicate interface '" + interfaceName +
                                      "' in the implements list of class '" +
                                      entry.qualifiedName + "'");
            }
            entry.interfaceIndices.push_back(interfaceIdx);
        }

        // ---- members ---------------------------------------------------------
        validateClassMembers(entry);
    }

    // ---- completeness of concrete classes -----------------------------------
    // A CONCRETE class must provide an implementation for every abstract
    // method in its chain and every method required by its (transitive)
    // interfaces. Abstract classes may defer this to their descendants.
    for (std::size_t i = 0; i < classes_.size(); ++i) {
        const ClassEntry& entry = classes_[i];
        if (entry.declaration->isInterface || entry.declaration->isAbstract) continue;

        // Collect abstract signatures from the whole chain and concrete
        // signatures that satisfy them. Set semantics make the walk order
        // irrelevant.
        std::set<std::pair<std::string, std::size_t>> abstractSignatures;
        std::set<std::pair<std::string, std::size_t>> implemented;
        for (int cur = entry.parentIndex; cur != -1;
             cur = classes_[static_cast<std::size_t>(cur)].parentIndex) {
            const ClassEntry& ancestor = classes_[static_cast<std::size_t>(cur)];
            for (const ClassMethod& method : ancestor.methods) {
                if (method.isConstructor) continue;
                const auto signature =
                    std::make_pair(method.name, method.parameters->size());
                if (method.isAbstract) {
                    abstractSignatures.insert(signature);
                } else if (!method.isStatic) {
                    implemented.insert(signature);
                }
            }
        }
        // Concrete methods of the class itself satisfy inherited abstracts.
        for (const ClassMethod& method : entry.methods) {
            if (!method.isConstructor && !method.isStatic) {
                implemented.insert({method.name, method.parameters->size()});
            }
        }
        for (const auto& abstractSignature : abstractSignatures) {
            if (implemented.count(abstractSignature) == 0) {
                // Find the declaring class name for the message.
                std::string declarer = "?";
                for (int cur = entry.parentIndex; cur != -1;
                     cur = classes_[static_cast<std::size_t>(cur)].parentIndex) {
                    const ClassEntry& ancestor = classes_[static_cast<std::size_t>(cur)];
                    for (const ClassMethod& method : ancestor.methods) {
                        if (!method.isConstructor && method.name == abstractSignature.first &&
                            method.parameters->size() == abstractSignature.second) {
                            declarer = ancestor.qualifiedName;
                        }
                    }
                }
                fail(*entry.declaration,
                     "class '" + entry.qualifiedName + "' must implement abstract method '" +
                         abstractSignature.first + "' (" +
                         std::to_string(abstractSignature.second) +
                         " parameter(s)) inherited from '" + declarer + "'");
            }
        }

        // Interface contracts (transitive through the parent chain).
        std::vector<int> requiredInterfaces;
        for (int cur = static_cast<int>(i); cur != -1;
             cur = classes_[static_cast<std::size_t>(cur)].parentIndex) {
            for (int interfaceIdx : classes_[static_cast<std::size_t>(cur)].interfaceIndices) {
                requiredInterfaces.push_back(interfaceIdx);
            }
        }
        for (int interfaceIdx : requiredInterfaces) {
            const ClassEntry& interface = classes_[static_cast<std::size_t>(interfaceIdx)];
            for (const ClassMethod& contract : interface.methods) {
                const ResolvedMember found =
                    findMethodInChain(static_cast<int>(i), contract.name,
                                      contract.parameters->size(),
                                      /*wantStatic=*/false, /*includePrivate=*/true);
                if (found.method == nullptr || found.method->isAbstract ||
                    found.method->isPrivate) {
                    fail(*entry.declaration,
                         "class '" + entry.qualifiedName +
                             "' must implement method '" + contract.name + "' (" +
                             std::to_string(contract.parameters->size()) +
                             " parameter(s)) required by interface '" +
                             interface.qualifiedName + "'");
                }
            }
        }
    }
}

void Compiler::validateClassMembers(ClassEntry& entry) {
    const ast::ClassDeclaration& declaration = *entry.declaration;
    const bool isInterface = declaration.isInterface;

    std::set<std::pair<std::string, std::size_t>> methodSignatures;
    std::set<std::string> memberNames;

    for (const ClassField& field : entry.fields) {
        if (isInterface) {
            fail(declaration, "interfaces cannot have fields ('" + field.name + "')");
        }
        if (!memberNames.insert(field.name).second) {
            fail(declaration, "duplicate member name '" + field.name +
                                  "' in class '" + entry.qualifiedName + "'");
        }
    }

    for (const ClassMethod& method : entry.methods) {
        if (isInterface) {
            if (method.isConstructor) {
                fail(declaration, "interfaces cannot have a constructor");
            }
            if (method.isStatic || method.isPrivate || method.isAbstract ||
                method.isOverride) {
                fail(*entry.declaration,
                     "interface methods cannot have modifiers ('" + method.name + "')");
            }
        }

        if (method.isConstructor) {
            // Runa 1.0 (spec sections 29/30): constructor overloads by arity
            // are allowed; two constructors with the SAME arity are not.
            for (const ClassMethod& other : entry.methods) {
                if (&other == &method || !other.isConstructor) continue;
                if (other.parameters->size() == method.parameters->size()) {
                    fail(declaration, "duplicate constructor in class '" +
                                          entry.qualifiedName + "' (" +
                                          std::to_string(method.parameters->size()) +
                                          " parameter(s)) — overloads must differ in arity");
                }
            }
            memberNames.insert("constructor");
            if (declaration.isAbstract) {
                fail(declaration, "abstract class '" + entry.qualifiedName +
                                      "' cannot have a constructor (abstract classes "
                                      "cannot be instantiated)");
            }
            continue;
        }

        // A method name must not collide with a FIELD (or the constructor);
        // overloads of one METHOD name are fine — several methods may share
        // a name with different parameter counts.
        if (memberNames.count(method.name) > 0) {
            fail(declaration, "duplicate member name '" + method.name +
                                  "' in class '" + entry.qualifiedName + "' (fields and "
                                  "methods share one namespace)");
        }

        const auto signature =
            std::make_pair(method.name, method.parameters->size());
        if (!methodSignatures.insert(signature).second) {
            fail(declaration, "duplicate method '" + method.name + "' (" +
                                  std::to_string(method.parameters->size()) +
                                  " parameter(s)) in class '" + entry.qualifiedName + "'");
        }

        // Mixed static/instance overloads of one name are ambiguous.
        for (const ClassMethod& other : entry.methods) {
            if (&other == &method || other.isConstructor) continue;
            if (other.name == method.name && other.isStatic != method.isStatic) {
                fail(declaration, "cannot mix static and instance methods named '" +
                                      method.name + "' in class '" +
                                      entry.qualifiedName + "'");
            }
        }

        // Abstract rules.
        if (method.isAbstract) {
            if (!declaration.isAbstract) {
                fail(declaration, "abstract method '" + method.name +
                                      "' in non-abstract class '" +
                                      entry.qualifiedName + "'");
            }
            if (method.isPrivate) {
                fail(declaration, "an abstract method cannot be private ('" +
                                      method.name + "' in class '" +
                                      entry.qualifiedName + "')");
            }
            if (method.isStatic) {
                fail(declaration, "an abstract method cannot be static ('" +
                                      method.name + "' in class '" +
                                      entry.qualifiedName + "')");
            }
            if (method.isOverride) {
                fail(declaration, "an abstract method cannot be marked 'override' ('" +
                                      method.name + "' in class '" +
                                      entry.qualifiedName + "')");
            }
            continue;
        }

        // static + override makes no sense (statics are not dispatched).
        if (method.isStatic && method.isOverride) {
            fail(declaration, "static methods cannot be overridden ('" + method.name +
                                  "' in class '" + entry.qualifiedName + "')");
        }

        // Instance methods: check against the parent chain.
        if (!method.isStatic) {
            const ResolvedMember parentMatch =
                method.isOverride
                    ? findMethodInChain(entry.parentIndex, method.name,
                                        method.parameters->size(),
                                        /*wantStatic=*/false, /*includePrivate=*/false)
                    : ResolvedMember{};
            if (method.isOverride) {
                if (parentMatch.method == nullptr) {
                    fail(declaration, "cannot override '" + method.name + "' — no method "
                                      "with " + std::to_string(method.parameters->size()) +
                                          " parameter(s) is visible in a parent class of '" +
                                          entry.qualifiedName + "'");
                }
            } else {
                // A concrete method matching a VISIBLE CONCRETE parent method
                // must be marked 'override'. Implementing an ABSTRACT parent
                // method with a plain 'fun' is fine — that IS the
                // implementation the contract asks for. (Overloads — different
                // arity — are new methods and need no marker.)
                const ResolvedMember conflict =
                    findMethodInChain(entry.parentIndex, method.name,
                                      method.parameters->size(),
                                      /*wantStatic=*/false, /*includePrivate=*/false);
                if (conflict.method != nullptr && !conflict.method->isAbstract) {
                    fail(declaration, "method '" + method.name + "' (" +
                                          std::to_string(method.parameters->size()) +
                                          " parameter(s)) is already declared in parent "
                                          "class '" +
                                          classes_[static_cast<std::size_t>(conflict.ownerIndex)]
                                              .qualifiedName +
                                          "' — mark it with 'override' to override it");
                }
            }
        }
    }
}

// ============================================================================
// Registry helpers
// ============================================================================

int Compiler::resolveClass(const std::string& name,
                           const std::vector<int>& scopeChain) const {
    for (int idx : scopeChain) {
        const ClassEntry& entry = classes_[static_cast<std::size_t>(idx)];
        if (entry.nestedSimpleNames.count(name) > 0) {
            const std::string qualified = entry.qualifiedName + "::" + name;
            return classesByQualifiedName_.at(qualified);
        }
    }
    const auto topLevel = topLevelClassNames_.find(name);
    return (topLevel == topLevelClassNames_.end()) ? -1 : topLevel->second;
}

std::vector<int> Compiler::memberScopeChain() const {
    return ownerClass_ ? ownerClass_->scopeChain : std::vector<int>{};
}

// Resolves a fully-qualified nested class name from the GLOBAL scope:
// "Car::Engine" -> topLevel "Car", then nested "Engine", etc. Returns -1
// when any segment is unknown.
int Compiler::resolveQualifiedNestedClass(const std::string& qualified) const {
    std::vector<std::string> segments;
    std::size_t start = 0;
    while (start <= qualified.size()) {
        const std::size_t sep = qualified.find("::", start);
        if (sep == std::string::npos) {
            segments.push_back(qualified.substr(start));
            break;
        }
        segments.push_back(qualified.substr(start, sep - start));
        start = sep + 2;
    }
    if (segments.empty()) return -1;
    const auto topLevel = topLevelClassNames_.find(segments[0]);
    if (topLevel == topLevelClassNames_.end()) return -1;
    int current = topLevel->second;
    for (std::size_t i = 1; i < segments.size(); ++i) {
        const ClassEntry& entry = classes_[static_cast<std::size_t>(current)];
        if (entry.nestedSimpleNames.count(segments[i]) == 0) return -1;
        current = classesByQualifiedName_.at(entry.qualifiedName + "::" +
                                             segments[i]);
    }
    return current;
}

bool Compiler::isClassNameInScope(const std::string& name) const {
    return resolveClass(name, memberScopeChain()) >= 0;
}

Compiler::ResolvedMember Compiler::findMethodInChain(
    int start, const std::string& name, std::size_t arity, bool wantStatic,
    bool includePrivate) const {
    for (int cur = start; cur != -1; cur = classes_[static_cast<std::size_t>(cur)].parentIndex) {
        const ClassEntry& entry = classes_[static_cast<std::size_t>(cur)];
        for (const ClassMethod& method : entry.methods) {
            if (method.isConstructor) continue;
            if (method.name != name) continue;
            if (method.parameters->size() != arity) continue;
            if (method.isStatic != wantStatic) continue;
            if (!includePrivate && method.isPrivate) continue;
            ResolvedMember found;
            found.ownerIndex = cur;
            found.method = &method;
            return found;
        }
    }
    return ResolvedMember{};
}

Compiler::ResolvedMember Compiler::findFieldInChain(int start, const std::string& name,
                                                    bool wantStatic) const {
    for (int cur = start; cur != -1; cur = classes_[static_cast<std::size_t>(cur)].parentIndex) {
        const ClassEntry& entry = classes_[static_cast<std::size_t>(cur)];
        for (const ClassField& field : entry.fields) {
            if (field.name != name) continue;
            if (field.isStatic != wantStatic) continue;
            ResolvedMember found;
            found.ownerIndex = cur;
            found.field = &field;
            return found;
        }
    }
    return ResolvedMember{};
}

bool Compiler::isFunctionName(const std::string& name) const {
    return declaredFunctions_.count(name) > 0 || stdlib::findBuiltin(name) != nullptr;
}

// A USER-declared function only. Built-in names (sum, min, max, ...) are
// deliberately NOT in this set: assignments and for-loop variables may reuse
// them, because calls never consult the variable namespace — the Call opcode
// resolves built-ins by name independently of any variable with the same
// name. Blocking built-ins here would have broken pre-collection programs
// that use e.g. `sum` or `count` as a variable name (no-regression rule).
bool Compiler::isUserFunctionName(const std::string& name) const {
    return declaredFunctions_.count(name) > 0;
}

// Core reserved names (Runa 1.0 spec sections 0.15/8 + DECISIONS #114/#115):
//   'printr' is a core reserved feature — it cannot be shadowed or redefined
//   in any binding position (variables, for variables, parameters, user
//   functions). The other built-ins (sum, count, ...) remain shadowable.
//   'none' is a runtime value but NOT a source literal — it can never be
//   bound or read as a variable.
void Compiler::ensureNotCoreReserved(const std::string& name, const ast::Node& at,
                                     const std::string& binding) {
    if (name == "printr") {
        fail(at, "cannot use 'printr' as " + binding +
                 " — printr is a core reserved feature and cannot be shadowed "
                 "or redefined");
    }
    if (name == "none") {
        fail(at, "cannot use 'none' as " + binding +
                 " — none is not a source literal in Runa (it is produced by "
                 "printr, a bare 'return', and functions that end without a value)");
    }
}

void Compiler::fail(const ast::Node& node, const std::string& message) {
    errors::RunaError error;
    error.kind = errors::ErrorKind::CompileError;
    error.location = node.location;
    error.message = message;
    throw errors::RunaErrorException(error);
}


// ============================================================================
// Statement compilation
// ============================================================================

int Compiler::ownerClassIndex() const {
    if (ownerClass_ == nullptr) return -1;
    return ownerClass_->scopeChain.back();
}

void Compiler::compileStatement(Chunk& chunk, const ast::Statement& statement) {
    switch (statement.kind) {
        case ast::NodeKind::AssignmentStatement:
            compileAssignment(chunk, static_cast<const ast::AssignmentStatement&>(statement));
            break;
        case ast::NodeKind::ExpressionStatement:
            // Evaluate the expression (usually a call), then discard its
            // value so the operand stack stays balanced.
            compileExpression(
                chunk, *static_cast<const ast::ExpressionStatement&>(statement).expression);
            chunk.writeOp(OpCode::Pop);
            break;
        case ast::NodeKind::IfStatement:
            compileIf(chunk, static_cast<const ast::IfStatement&>(statement));
            break;
        case ast::NodeKind::LoopStatement:
            compileLoop(chunk, static_cast<const ast::LoopStatement&>(statement));
            break;
        case ast::NodeKind::ForStatement:
            compileFor(chunk, static_cast<const ast::ForStatement&>(statement));
            break;
        case ast::NodeKind::BreakStatement:
            compileBreak(chunk, static_cast<const ast::BreakStatement&>(statement));
            break;
        case ast::NodeKind::ContinueStatement:
            compileContinue(chunk, static_cast<const ast::ContinueStatement&>(statement));
            break;
        case ast::NodeKind::FunctionDeclaration: {
            const auto& declaration =
                static_cast<const ast::FunctionDeclaration&>(statement);
            fail(declaration, "nested function declarations are not supported yet ('" +
                                  declaration.name +
                                  "' must be declared at the top level of the program)");
        }
        case ast::NodeKind::ClassDeclaration: {
            const auto& declaration =
                static_cast<const ast::ClassDeclaration&>(statement);
            fail(declaration, "class declarations are only allowed at the top level of "
                              "the program or directly inside a class body ('" +
                                  declaration.name + "' is nested somewhere else)");
        }
        case ast::NodeKind::ReturnStatement:
            compileReturn(chunk, static_cast<const ast::ReturnStatement&>(statement));
            break;
        case ast::NodeKind::ImportStatement:
        case ast::NodeKind::SelectiveImportStatement:
        case ast::NodeKind::CimportStatement:
            // The top-level emission loops skip these; reaching here means
            // the statement sits inside a block or a function body.
            fail(statement, "import statements are only allowed at the top level "
                            "of the program");
            break;
        default:
            fail(statement, "unsupported statement in compiler (not implemented yet)");
    }
}

void Compiler::compileAssignment(Chunk& chunk, const ast::AssignmentStatement& statement) {
    // ---- variable target ---------------------------------------------------
    if (statement.target->kind == ast::NodeKind::Identifier) {
        const auto& target = static_cast<const ast::Identifier&>(*statement.target);
        if (isClassNameInScope(target.name)) {
            fail(statement, "cannot assign to '" + target.name +
                                "' — it is a class name (classes are not values)");
        }
        if (isUserFunctionName(target.name)) {
            fail(statement, "cannot assign to '" + target.name +
                                "' — it is a function name");
        }
        if (isReservedUnitName(target.name)) {
            fail(statement, "cannot assign to '" + target.name +
                                "' — it is an imported package or symbol name");
        }
        ensureNotCoreReserved(target.name, statement, "an assignment target");
        // Conceptually:  <evaluate RHS>  →  STORE name
        compileExpression(chunk, *statement.value);
        // Package units store GLOBALS under their hidden prefix; locals inside
        // functions keep raw names (the VM's frame prefix fallback resolves
        // package globals dynamically).
        const std::uint32_t nameIndex =
            chunk.addName(inFunction_ ? target.name : unitName(target.name));
        if (inFunction_) {
            chunk.writeOp(OpCode::StoreLocal);
        } else {
            chunk.writeOp(OpCode::StoreGlobal);
        }
        chunk.writeU32(nameIndex);
        return;
    }

    // ---- member target (self.speed = ... / car.name = ... / Car.count = ...) -
    if (statement.target->kind == ast::NodeKind::MemberAccessExpr) {
        compileMemberAssignmentTarget(
            chunk, static_cast<const ast::MemberAccessExpr&>(*statement.target),
            *statement.value);
        return;
    }

    fail(statement, "invalid assignment target (only variables and member "
                    "accesses like 'car.name' can be assigned to)");
}

// Assignment to a member: value first, then the receiver store. The receiver
// may be a class name (static field), self (checked here) or a dynamic
// expression (checked by the VM at run time).
void Compiler::compileMemberAssignmentTarget(Chunk& chunk,
                                             const ast::MemberAccessExpr& target,
                                             const ast::Expression& value) {
    const std::vector<int> scope = memberScopeChain();

    // ---- package-rooted static field write (math.Vector.count = v / Vector.count = v)
    {
        int classIdx = -1;
        std::string display;
        bool packagePath = false;
        if (target.object->kind == ast::NodeKind::MemberAccessExpr) {
            packagePath = resolvePackageClassPath(*target.object, classIdx, display);
        } else if (unit_ != nullptr && target.object->kind == ast::NodeKind::Identifier) {
            const auto& object = static_cast<const ast::Identifier&>(*target.object);
            const auto selective = unit_->selective.find(object.name);
            if (selective != unit_->selective.end() && selective->second.isClass) {
                classIdx = selective->second.classIndex;
                display = object.name;
                packagePath = true;
            } else if (unit_->imports.count(object.name) > 0) {
                fail(target, "'" + object.name +
                                 "' is a package — assign its classes' statics as '" +
                                 object.name + ".ClassName.field = ...'");
            } else if (unit_->cimports.count(object.name) > 0) {
                fail(target, "'" + object.name +
                                 "' is a native package — native packages have no "
                                 "assignable members");
            }
        }
        if (packagePath) {
            const ResolvedMember found =
                findFieldInChain(classIdx, target.memberName, /*wantStatic=*/true);
            if (found.field == nullptr) {
                if (findFieldInChain(classIdx, target.memberName, false).field !=
                    nullptr) {
                    fail(target, "'" + target.memberName +
                                     "' is an instance field of class '" + display +
                                     "' (assign it through an instance)");
                }
                fail(target, "class '" + display + "' has no static field '" +
                                 target.memberName + "'");
            }
            if (found.field->isPrivate && found.ownerIndex != ownerClassIndex()) {
                fail(target, "private static field '" + target.memberName +
                                 "' of class '" + display + "' is not accessible here");
            }
            compileExpression(chunk, value);
            chunk.writeOp(OpCode::StoreStatic);
            chunk.writeU32(static_cast<std::uint32_t>(classIdx));
            chunk.writeU32(chunk.addName(target.memberName));
            return;
        }
    }

    // ClassName.staticField = value
    if (target.object->kind == ast::NodeKind::Identifier) {
        const auto& object = static_cast<const ast::Identifier&>(*target.object);
        const int classIdx = resolveClass(object.name, scope);
        if (classIdx >= 0) {
            const ClassEntry& entry = classes_[static_cast<std::size_t>(classIdx)];
            const ResolvedMember found =
                findFieldInChain(classIdx, target.memberName, /*wantStatic=*/true);
            if (found.field == nullptr) {
                if (findFieldInChain(classIdx, target.memberName, false).field != nullptr) {
                    fail(target, "'" + target.memberName + "' is an instance field of "
                                 "class '" + entry.qualifiedName +
                                     "' (access it through an instance)");
                }
                fail(target, "class '" + entry.qualifiedName + "' has no static field '" +
                                 target.memberName + "'");
            }
            if (found.field->isPrivate && found.ownerIndex != ownerClassIndex()) {
                fail(target, "private static field '" + target.memberName + "' of class '" +
                                 classes_[static_cast<std::size_t>(found.ownerIndex)]
                                     .qualifiedName +
                                     "' is not accessible here");
            }
            compileExpression(chunk, value);
            chunk.writeOp(OpCode::StoreStatic);
            chunk.writeU32(static_cast<std::uint32_t>(classIdx));
            chunk.writeU32(chunk.addName(target.memberName));
            return;
        }
    }

    // self.field = value  (statically checkable)
    if (target.object->kind == ast::NodeKind::SelfExpr) {
        if (ownerClass_ == nullptr || !inFunction_) {
            fail(target, "'self' is only available inside instance methods and "
                         "constructors");
        }
        if (inStaticMethod_) {
            fail(target, "'self' is not available in a static method");
        }
        const ResolvedMember found =
            findFieldInChain(ownerClassIndex(), target.memberName, /*wantStatic=*/false);
        if (found.field == nullptr) {
            if (findFieldInChain(ownerClassIndex(), target.memberName, true).field !=
                nullptr) {
                fail(target, "'" + target.memberName + "' is a static field (assign it as '" +
                                 ownerClass_->qualifiedName + "." + target.memberName + "')");
            }
            fail(target, "class '" + ownerClass_->qualifiedName + "' has no field '" +
                             target.memberName + "'");
        }
        if (found.field->isPrivate && found.ownerIndex != ownerClassIndex()) {
            fail(target, "private field '" + target.memberName + "' of class '" +
                             classes_[static_cast<std::size_t>(found.ownerIndex)]
                                 .qualifiedName +
                                 "' is not accessible from '" + ownerClass_->qualifiedName +
                                 "'");
        }
        compileExpression(chunk, value);
        chunk.writeOp(OpCode::GetSelf);
        chunk.writeOp(OpCode::SetField);
        chunk.writeU32(chunk.addName(target.memberName));
        return;
    }

    // Dynamic receiver: checked at run time.
    compileExpression(chunk, value);
    compileExpression(chunk, *target.object);
    chunk.writeOp(OpCode::SetField);
    chunk.writeU32(chunk.addName(target.memberName));
}

void Compiler::compileIf(Chunk& chunk, const ast::IfStatement& statement) {
    // Same backpatching chain as before: condition → JumpIfFalse(placeholder)
    // → body → Jump(end); false targets patch to the next check.
    std::vector<std::size_t> endJumps;
    for (const auto& branch : statement.branches) {
        if (branch.condition != nullptr) {
            compileExpression(chunk, *branch.condition);
            chunk.writeOp(OpCode::JumpIfFalse);
            const std::size_t checkOffset = chunk.code.size();
            chunk.writeU32(0);
            for (const auto& bodyStatement : branch.body) {
                compileStatement(chunk, *bodyStatement);
            }
            chunk.writeOp(OpCode::Jump);
            endJumps.push_back(chunk.code.size());
            chunk.writeU32(0);
            chunk.patchU32(checkOffset, static_cast<std::uint32_t>(chunk.code.size()));
        } else {
            for (const auto& bodyStatement : branch.body) {
                compileStatement(chunk, *bodyStatement);
            }
        }
    }
    const std::uint32_t end = static_cast<std::uint32_t>(chunk.code.size());
    for (const std::size_t offset : endJumps) {
        chunk.patchU32(offset, end);
    }
}

// ============================================================================
// Loop compilation
// ============================================================================

void Compiler::storeVariableSlot(Chunk& chunk, const std::string& name) {
    const std::uint32_t nameIndex =
        chunk.addName(inFunction_ ? name : unitName(name));
    if (inFunction_) {
        chunk.writeOp(OpCode::StoreLocal);
    } else {
        chunk.writeOp(OpCode::StoreGlobal);
    }
    chunk.writeU32(nameIndex);
}

void Compiler::loadVariableSlot(Chunk& chunk, const std::string& name) {
    const std::uint32_t nameIndex = chunk.addName(name);  // raw: frame fallback
    if (inFunction_) {
        chunk.writeOp(OpCode::LoadVar);  // locals first, then globals (v0.6 rule)
    } else {
        chunk.writeOp(OpCode::LoadGlobal);
    }
    chunk.writeU32(nameIndex);
}

// loop <cond> [ body ]
//
//     condStart: <cond>            (pushed once per iteration)
//                JumpIfFalse end   (pops the condition)
//                <body>
//                Jump condStart
//     end:                         (breaks land here)
//
// 'continue' re-evaluates the condition, so its target is condStart. The
// stack stays balanced at every statement boundary: the condition is popped
// by JumpIfFalse and every body statement is net-zero.
void Compiler::compileLoop(Chunk& chunk, const ast::LoopStatement& statement) {
    const std::uint32_t conditionOffset = static_cast<std::uint32_t>(chunk.code.size());

    compileExpression(chunk, *statement.condition);
    chunk.writeOp(OpCode::JumpIfFalse);
    const std::size_t exitPatch = chunk.code.size();
    chunk.writeU32(0);  // patched to the first instruction after the loop

    loopStack_.push_back(LoopContext{});
    for (const auto& bodyStatement : statement.body) {
        compileStatement(chunk, *bodyStatement);
    }
    // continue → re-evaluate the condition (known from the start, but patched
    // through the same mechanism as 'for' for uniformity).
    for (const std::size_t offset : loopStack_.back().continuePatches) {
        chunk.patchU32(offset, conditionOffset);
    }

    chunk.writeOp(OpCode::Jump);
    chunk.writeU32(conditionOffset);
    // break AND the false condition land after the loop's back-jump.
    const std::uint32_t end = static_cast<std::uint32_t>(chunk.code.size());
    for (const std::size_t offset : loopStack_.back().breakPatches) {
        chunk.patchU32(offset, end);
    }
    loopStack_.pop_back();
    chunk.patchU32(exitPatch, end);
}

// for i = <start> to <end> [ body ]
//
//     <start>            evaluated ONCE
//     STORE i            (local inside functions/methods, global at top level)
//     <end>              evaluated ONCE — the range is fixed at entry
//     STORE #for_endN#   (compiler-internal hidden slot)
//     condStart: LOAD i
//                LOAD #for_endN#
//                Le                 (inclusive upper bound)
//                JumpIfFalse end
//                <body>
//     incr:      LOAD i
//                LoadConst 1
//                Add                (step is fixed at +1)
//                STORE i
//                Jump condStart
//     end:                           (breaks land here)
//
// 'continue' jumps to the INCREMENT (not to condStart) — skipping the rest of
// the body must still advance the counter, or the loop would never end.
// start > end makes the first condition check false → zero iterations.
void Compiler::compileFor(Chunk& chunk, const ast::ForStatement& statement) {
    // The counter is an ordinary variable: class/function names stay reserved,
    // exactly as for a normal assignment. Imported names too.
    if (isClassNameInScope(statement.variableName)) {
        fail(statement, "cannot use '" + statement.variableName +
                            "' as a for loop variable — it is a class name "
                            "(classes are not values)");
    }
    if (isUserFunctionName(statement.variableName)) {
        fail(statement, "cannot use '" + statement.variableName +
                            "' as a for loop variable — it is a function name");
    }
    if (isReservedUnitName(statement.variableName)) {
        fail(statement, "cannot use '" + statement.variableName +
                            "' as a for loop variable — it is an imported package "
                            "or symbol name");
    }
    ensureNotCoreReserved(statement.variableName, statement, "a for loop variable");

    // Compiler-internal hidden slot for the (fixed) end value.
    const std::string endSlot = "#for_end" + std::to_string(forTempCounter_++) + "#";

    // ---- init: counter = start; hidden = end (both evaluated exactly once) --
    compileExpression(chunk, *statement.start);
    storeVariableSlot(chunk, statement.variableName);
    compileExpression(chunk, *statement.end);
    storeVariableSlot(chunk, endSlot);

    // ---- condition: i <= end -------------------------------------------------
    const std::uint32_t conditionOffset = static_cast<std::uint32_t>(chunk.code.size());
    loadVariableSlot(chunk, statement.variableName);
    loadVariableSlot(chunk, endSlot);
    chunk.writeOp(OpCode::Le);
    chunk.writeOp(OpCode::JumpIfFalse);
    const std::size_t exitPatch = chunk.code.size();
    chunk.writeU32(0);

    // ---- body ----------------------------------------------------------------
    loopStack_.push_back(LoopContext{});
    for (const auto& bodyStatement : statement.body) {
        compileStatement(chunk, *bodyStatement);
    }
    // continue → the increment step (only known after the body, hence the
    // backpatching).
    const std::uint32_t incrementOffset = static_cast<std::uint32_t>(chunk.code.size());
    for (const std::size_t offset : loopStack_.back().continuePatches) {
        chunk.patchU32(offset, incrementOffset);
    }

    // ---- increment: i = i + 1 ------------------------------------------------
    loadVariableSlot(chunk, statement.variableName);
    chunk.writeOp(OpCode::LoadConst);
    chunk.writeU32(chunk.addNumberConstant(1));
    chunk.writeOp(OpCode::Add);
    storeVariableSlot(chunk, statement.variableName);
    chunk.writeOp(OpCode::Jump);
    chunk.writeU32(conditionOffset);

    // ---- break target (after the loop's back-jump) ---------------------------
    const std::uint32_t end = static_cast<std::uint32_t>(chunk.code.size());
    for (const std::size_t offset : loopStack_.back().breakPatches) {
        chunk.patchU32(offset, end);
    }
    loopStack_.pop_back();
    chunk.patchU32(exitPatch, end);
}

void Compiler::compileBreak(Chunk& chunk, const ast::BreakStatement& statement) {
    if (loopStack_.empty()) {
        fail(statement, "break outside of a loop (break is only valid inside a "
                        "loop or for body)");
    }
    chunk.writeOp(OpCode::Jump);
    loopStack_.back().breakPatches.push_back(chunk.code.size());
    chunk.writeU32(0);  // patched when the enclosing loop is done
}

void Compiler::compileContinue(Chunk& chunk, const ast::ContinueStatement& statement) {
    if (loopStack_.empty()) {
        fail(statement, "continue outside of a loop (continue is only valid inside "
                        "a loop or for body)");
    }
    chunk.writeOp(OpCode::Jump);
    loopStack_.back().continuePatches.push_back(chunk.code.size());
    chunk.writeU32(0);  // patched to the loop's continue target
}

void Compiler::compileFunctionDeclaration(Chunk& chunk,
                                          const ast::FunctionDeclaration& declaration) {
    bytecode::FunctionInfo info;
    // Package units register their functions under the hidden '#pkg.x#' prefix
    // (the main unit's prefix is empty, so names stay untouched).
    info.name = unitName(declaration.name);
    info.entryOffset = static_cast<std::uint32_t>(chunk.code.size());
    for (const ast::Parameter& parameter : declaration.parameters) {
        info.parameters.push_back(parameter.name);
    }
    chunk.functions.push_back(std::move(info));

    inFunction_ = true;
    // A function body is a fresh control-flow world: break/continue can never
    // cross a function boundary (spec section 6). The stack is provably empty
    // here (functions cannot nest inside loops), but clearing it makes the
    // boundary explicit and keeps the invariant local to this function.
    const std::size_t previousLoopDepth = loopStack_.size();
    loopStack_.clear();
    for (const auto& statement : declaration.body) {
        compileStatement(chunk, *statement);
    }
    loopStack_.resize(previousLoopDepth);
    chunk.writeOp(OpCode::ReturnNone);
    inFunction_ = false;
}

void Compiler::compileReturn(Chunk& chunk, const ast::ReturnStatement& statement) {
    if (!inFunction_) {
        fail(statement, "return outside of a function (return is only valid "
                        "inside a function body)");
    }
    if (inConstructor_ && statement.value != nullptr) {
        fail(statement, "a constructor cannot return a value (use a bare 'return' "
                        "to exit early)");
    }
    if (statement.value != nullptr) {
        compileExpression(chunk, *statement.value);
        chunk.writeOp(OpCode::ReturnValue);
    } else {
        chunk.writeOp(OpCode::ReturnNone);
    }
}

// ============================================================================
// Expression compilation
// ============================================================================

// Statically-known kind of a literal expression, mirroring the collection
// literal's compile-time homogeneity check: "number" / "float" / "string"
// / "boolean" / "null" / "collection", or "" when the kind is not known
// at compile time (variables, calls, arithmetic...). Parenthesized literals
// keep their kind — the parser returns the inner node for a '(' group.
static std::string staticKindOf(const ast::Expression& expression) {
    switch (expression.kind) {
        case ast::NodeKind::NumberLiteral:    return "number";
        case ast::NodeKind::FloatLiteral:     return "float";
        case ast::NodeKind::StringLiteral:    return "string";
        case ast::NodeKind::BoolLiteral:      return "boolean";
        case ast::NodeKind::NullLiteral:      return "null";
        case ast::NodeKind::CollectionLiteral: return "collection";
        default:                              return "";
    }
}

static bool isNumericKind(const std::string& kind) {
    return kind == "number" || kind == "float";
}

void Compiler::compileExpression(Chunk& chunk, const ast::Expression& expression) {
    switch (expression.kind) {
        case ast::NodeKind::StringLiteral: {
            const auto& literal = static_cast<const ast::StringLiteral&>(expression);
            chunk.writeOp(OpCode::LoadConst);
            chunk.writeU32(chunk.addStringConstant(literal.value));
            break;
        }
        case ast::NodeKind::NumberLiteral: {
            const auto& literal = static_cast<const ast::NumberLiteral&>(expression);
            chunk.writeOp(OpCode::LoadConst);
            chunk.writeU32(chunk.addNumberConstant(literal.value));
            break;
        }
        case ast::NodeKind::FloatLiteral: {
            // Floats are official in Runa 1.0 (DECISIONS #108): a decimal
            // literal becomes a Float constant, bit-exact through v6 bytecode.
            const auto& literal = static_cast<const ast::FloatLiteral&>(expression);
            chunk.writeOp(OpCode::LoadConst);
            chunk.writeU32(chunk.addFloatConstant(literal.value));
            break;
        }
        case ast::NodeKind::BoolLiteral: {
            const auto& literal = static_cast<const ast::BoolLiteral&>(expression);
            chunk.writeOp(OpCode::LoadConst);
            chunk.writeU32(chunk.addBooleanConstant(literal.value));
            break;
        }
        case ast::NodeKind::NullLiteral: {
            bytecode::Constant constant;
            constant.kind = bytecode::Constant::Kind::Null;
            chunk.constants.push_back(std::move(constant));
            chunk.writeOp(OpCode::LoadConst);
            chunk.writeU32(static_cast<std::uint32_t>(chunk.constants.size() - 1));
            break;
        }
        case ast::NodeKind::SelfExpr: {
            const auto& self = static_cast<const ast::SelfExpr&>(expression);
            if (ownerClass_ == nullptr || !inFunction_) {
                fail(self, "'self' is only available inside instance methods and "
                           "constructors");
            }
            if (inStaticMethod_) {
                fail(self, "'self' is not available in a static method (static methods "
                           "belong to the class, not to an instance)");
            }
            chunk.writeOp(OpCode::GetSelf);
            break;
        }
        case ast::NodeKind::Identifier: {
            const auto& identifier = static_cast<const ast::Identifier&>(expression);
            if (identifier.name == "none") {
                fail(expression, "'none' is not a source literal in Runa (none is "
                                 "produced by printr, a bare 'return', and functions "
                                 "that end without a value)");
            }
            if (identifier.name == "printr") {
                fail(expression, "'printr' is a core reserved feature and cannot be "
                                 "used as a value (call it: printr(...))");
            }
            if (isClassNameInScope(identifier.name)) {
                fail(expression, "'" + identifier.name +
                                     "' is a class, not a value (instantiate it with "
                                     "'new " + identifier.name + "(...)')");
            }
            if (isUserFunctionName(identifier.name)) {
                fail(expression, "function '" + identifier.name +
                                     "' cannot be used as a value (functions are "
                                     "not values in Runa yet)");
            }
            if (unit_ != nullptr) {
                if (unit_->imports.count(identifier.name) > 0) {
                    fail(expression, "'" + identifier.name +
                                         "' is a package — call its functions as '" +
                                         identifier.name + ".name(...)'");
                }
                if (unit_->cimports.count(identifier.name) > 0) {
                    fail(expression, "'" + identifier.name +
                                         "' is a native package — call its functions "
                                         "as '" + identifier.name + ".name(...)'");
                }
                const auto selective = unit_->selective.find(identifier.name);
                if (selective != unit_->selective.end()) {
                    if (selective->second.isClass) {
                        fail(expression, "'" + identifier.name +
                                             "' is an imported class, not a value "
                                             "(instantiate it with 'new " +
                                             identifier.name + "(...)')");
                    }
                    fail(expression, "imported function '" + identifier.name +
                                         "' cannot be used as a value (functions "
                                         "are not values in Runa yet)");
                }
            }
            // Globals (top level) carry the unit's hidden prefix; locals inside
            // functions keep raw names (frame prefix fallback handles globals).
            const std::uint32_t nameIndex = chunk.addName(
                inFunction_ ? identifier.name : unitName(identifier.name));
            if (inFunction_) {
                chunk.writeOp(OpCode::LoadVar);
            } else {
                chunk.writeOp(OpCode::LoadGlobal);
            }
            chunk.writeU32(nameIndex);
            break;
        }
        case ast::NodeKind::FunctionCallExpr:
            compileCallExpression(chunk,
                                  static_cast<const ast::FunctionCallExpr&>(expression));
            break;
        case ast::NodeKind::MethodCallExpr:
            compileMethodCall(chunk, static_cast<const ast::MethodCallExpr&>(expression));
            break;
        case ast::NodeKind::MemberAccessExpr:
            compileMemberAccess(chunk,
                                static_cast<const ast::MemberAccessExpr&>(expression));
            break;
        case ast::NodeKind::NewExpr:
            compileNewObject(chunk, static_cast<const ast::NewExpr&>(expression));
            break;
        case ast::NodeKind::SuperCallExpr:
            compileSuperCall(chunk, static_cast<const ast::SuperCallExpr&>(expression));
            break;
        case ast::NodeKind::IsExpr:
            compileIsExpr(chunk, static_cast<const ast::IsExpr&>(expression));
            break;
        case ast::NodeKind::BinaryExpr: {
            const auto& binary = static_cast<const ast::BinaryExpr&>(expression);

            // ---- compile-time operand check (DECISIONS #130) -------------
            // When BOTH operands are literals, their kinds are provable now:
            // reject the invalid combinations at compile time with the same
            // wording the run time uses, instead of letting the program
            // start and fail on its first line. Mixed/unknown operand kinds
            // stay run-time territory (full type inference is a larger,
            // separate decision).
            {
                const std::string lhsKind = staticKindOf(*binary.lhs);
                const std::string rhsKind = staticKindOf(*binary.rhs);
                const bool bothKnown = !lhsKind.empty() && !rhsKind.empty();
                if (bothKnown) {
                    switch (binary.op) {
                        case ast::BinaryOp::Add:
                            // '+' is arithmetic on numbers OR string
                            // concatenation when either side is a string
                            // (DECISIONS #128: either operand's string form
                            // is glued, exactly like printr renders it).
                            if (!((isNumericKind(lhsKind) && isNumericKind(rhsKind)) ||
                                  lhsKind == "string" || rhsKind == "string")) {
                                fail(expression, "unsupported operand types for '+': " +
                                                     lhsKind + " and " + rhsKind);
                            }
                            break;
                        case ast::BinaryOp::Sub:
                        case ast::BinaryOp::Mul:
                        case ast::BinaryOp::Div:
                        case ast::BinaryOp::Mod:
                            if (!isNumericKind(lhsKind) || !isNumericKind(rhsKind)) {
                                fail(expression, std::string("unsupported operand types for '") +
                                                     binaryOpSymbol(binary.op) + "': " +
                                                     lhsKind + " and " + rhsKind);
                            }
                            break;
                        case ast::BinaryOp::Gt:
                        case ast::BinaryOp::Lt:
                        case ast::BinaryOp::Ge:
                        case ast::BinaryOp::Le:
                            // Ordering is numeric, or string-vs-string in
                            // UTF-8 byte order (DECISIONS #129).
                            if (!((isNumericKind(lhsKind) && isNumericKind(rhsKind)) ||
                                  (lhsKind == "string" && rhsKind == "string"))) {
                                fail(expression, std::string("unsupported operand types for '") +
                                                     binaryOpSymbol(binary.op) + "': " +
                                                     lhsKind + " and " + rhsKind);
                            }
                            break;
                        case ast::BinaryOp::LogicalAnd:
                        case ast::BinaryOp::LogicalOr:
                            if (lhsKind != "boolean" || rhsKind != "boolean") {
                                fail(expression, std::string("logical '") +
                                                     binaryOpSymbol(binary.op) +
                                                     "' requires boolean operands, got " +
                                                     lhsKind + " and " + rhsKind);
                            }
                            break;
                        case ast::BinaryOp::Eq:
                        case ast::BinaryOp::Ne:
                        case ast::BinaryOp::IdentEq:
                            break;  // equality accepts any two values
                    }
                }
            }

            // ---- short-circuit compilation for 'and' / 'or' ---------------
            // (DECISIONS #127) The right operand must NOT be evaluated when
            // the left one already decides the result:
            //     and:  <lhs> JumpIfFalseKeep(end) <rhs> And  end:
            //     or:   <lhs> JumpIfTrueKeep(end)  <rhs> Or   end:
            // Both paths leave exactly one value on the stack.
            if (binary.op == ast::BinaryOp::LogicalAnd ||
                binary.op == ast::BinaryOp::LogicalOr) {
                const bool isAnd = binary.op == ast::BinaryOp::LogicalAnd;
                compileExpression(chunk, *binary.lhs);
                chunk.writeOp(isAnd ? OpCode::JumpIfFalseKeep : OpCode::JumpIfTrueKeep);
                const std::size_t shortOffset = chunk.code.size();
                chunk.writeU32(0);
                compileExpression(chunk, *binary.rhs);
                chunk.writeOp(isAnd ? OpCode::And : OpCode::Or);
                chunk.patchU32(shortOffset,
                               static_cast<std::uint32_t>(chunk.code.size()));
                break;
            }

            compileExpression(chunk, *binary.lhs);
            compileExpression(chunk, *binary.rhs);
            switch (binary.op) {
                case ast::BinaryOp::Add: chunk.writeOp(OpCode::Add); break;
                case ast::BinaryOp::Sub: chunk.writeOp(OpCode::Sub); break;
                case ast::BinaryOp::Mul: chunk.writeOp(OpCode::Mul); break;
                case ast::BinaryOp::Div: chunk.writeOp(OpCode::Div); break;
                case ast::BinaryOp::Mod: chunk.writeOp(OpCode::Mod); break;
                case ast::BinaryOp::Eq:  chunk.writeOp(OpCode::Eq);  break;
                case ast::BinaryOp::Ne:  chunk.writeOp(OpCode::Ne);  break;
                case ast::BinaryOp::Gt:  chunk.writeOp(OpCode::Gt);  break;
                case ast::BinaryOp::Lt:  chunk.writeOp(OpCode::Lt);  break;
                case ast::BinaryOp::Ge:  chunk.writeOp(OpCode::Ge);  break;
                case ast::BinaryOp::Le:  chunk.writeOp(OpCode::Le);  break;
                case ast::BinaryOp::IdentEq: chunk.writeOp(OpCode::EqIdent); break;
                case ast::BinaryOp::LogicalAnd:
                case ast::BinaryOp::LogicalOr:
                    // Handled above (short-circuit emission); unreachable.
                    break;
            }
            break;
        }
        case ast::NodeKind::UnaryExpr: {
            const auto& unary = static_cast<const ast::UnaryExpr&>(expression);
            // Compile-time operand check when the operand's kind is provable
            // (DECISIONS #130): unary '-' wants a number, 'not' wants a bool.
            const std::string operandKind = staticKindOf(*unary.operand);
            if (!operandKind.empty()) {
                if (unary.op == ast::UnaryOp::Negate && !isNumericKind(operandKind)) {
                    fail(expression, std::string("unsupported operand type for unary '-': ") +
                                         operandKind);
                }
                if (unary.op == ast::UnaryOp::Not && operandKind != "boolean") {
                    fail(expression, "logical 'not' requires a boolean operand, got " +
                                         operandKind);
                }
            }
            compileExpression(chunk, *unary.operand);
            chunk.writeOp(unary.op == ast::UnaryOp::Negate ? OpCode::Negate
                                                           : OpCode::Not);
            break;
        }
        case ast::NodeKind::CollectionLiteral:
            compileCollectionLiteral(
                chunk, static_cast<const ast::CollectionLiteral&>(expression));
            break;
        default:
            fail(expression, "unsupported expression in compiler (not implemented yet)");
    }
}

// Collection literal (the Collection System). Compile every element in source
// order, then terminate with BuildCollection <count>. The VM enforces the
// one-element-type rule at run time for dynamic elements; when EVERY element
// is a compile-time scalar literal, the same rule is enforced here at compile
// time so `{10, "Runa"}` fails before any bytecode exists.
void Compiler::compileCollectionLiteral(Chunk& chunk,
                                        const ast::CollectionLiteral& literal) {
    // ---- static homogeneity check (all-literal collections only) -----------
    // Returns "number" / "string" / "boolean" for the scalar literal nodes,
    // "collection" for a nested literal, and "" when the element's type is
    // not statically known (variables, arithmetic, calls...).
    static const auto staticKindOf =
        [](const ast::Expression& element) -> std::string {
        switch (element.kind) {
            case ast::NodeKind::NumberLiteral:  return "number";
            case ast::NodeKind::FloatLiteral:   return "float";
            case ast::NodeKind::StringLiteral:  return "string";
            case ast::NodeKind::BoolLiteral:    return "boolean";
            case ast::NodeKind::NullLiteral:    return "null";
            case ast::NodeKind::CollectionLiteral: return "collection";
            default:                            return "";
        }
    };
    std::string firstKind;
    for (const auto& element : literal.elements) {
        const std::string elementKind = staticKindOf(*element);
        if (elementKind == "collection") {
            fail(*element, "nested collections are not supported (collection "
                           "elements must be number, string, or boolean values)");
        }
        if (elementKind == "null") {
            fail(*element, "null cannot be a collection element (an empty "
                           "collection is written '{}' and is distinct from null)");
        }
        if (elementKind.empty()) {
            continue;  // dynamic element — the run-time check covers these
        }
        if (firstKind.empty()) {
            firstKind = elementKind;
        } else if (elementKind != firstKind) {
            fail(*element, "collection elements must share one type (found " +
                               firstKind + " and " + elementKind + ")");
        }
    }

    for (const auto& element : literal.elements) {
        compileExpression(chunk, *element);
    }
    chunk.writeOp(OpCode::BuildCollection);
    chunk.writeU32(static_cast<std::uint32_t>(literal.elements.size()));
}

void Compiler::compileCallExpression(Chunk& chunk, const ast::FunctionCallExpr& call) {
    const std::size_t argc = call.arguments.size();

    // ---- package-system resolution (active unit only) -----------------------
    if (unit_ != nullptr) {
        // Selective import binding: sqrt(25) calls the package function.
        const auto selective = unit_->selective.find(call.callee);
        if (selective != unit_->selective.end()) {
            if (selective->second.isClass) {
                fail(call, "'" + call.callee +
                               "' is an imported class — instantiate it with 'new " +
                               call.callee + "(...)'");
            }
            const auto view = packageViews_.find(selective->second.packageName);
            const auto fn = view->second.functions.find(selective->second.symbol);
            if (fn->second != argc) {
                fail(call, "'" + call.callee + "' expects " +
                               std::to_string(fn->second) + " argument(s), got " +
                               std::to_string(argc));
            }
            for (const auto& argument : call.arguments) {
                compileExpression(chunk, *argument);
            }
            chunk.writeOp(OpCode::Call);
            chunk.writeU32(chunk.addName(selective->second.prefix +
                                         selective->second.symbol));
            chunk.writeU32(static_cast<std::uint32_t>(argc));
            return;
        }
        // A package/native namespace used directly as the callee.
        if (unit_->imports.count(call.callee) > 0) {
            fail(call, "'" + call.callee +
                           "' is a package — call its functions as '" + call.callee +
                           ".name(...)'");
        }
        if (unit_->cimports.count(call.callee) > 0) {
            fail(call, "'" + call.callee +
                           "' is a native package — call its functions as '" +
                           call.callee + ".name(...)'");
        }
    }

    const auto userOverloads = declaredFunctions_.find(call.callee);
    if (userOverloads != declaredFunctions_.end()) {
        // Overload resolution: Name + Arity (spec section 16). The user
        // function wins over a same-name built-in; among overloads the one
        // with exactly this argument count is required.
        bool arityMatch = false;
        for (const auto* overload : userOverloads->second) {
            if (overload->parameters.size() == argc) {
                arityMatch = true;
                break;
            }
        }
        if (!arityMatch) {
            std::string arities;
            for (const auto* overload : userOverloads->second) {
                if (!arities.empty()) arities += "/";
                arities += std::to_string(overload->parameters.size());
            }
            fail(call, "no overload of '" + call.callee + "' takes " +
                           std::to_string(argc) + " argument(s) (expected " +
                           arities + ")");
        }
    } else {
        const stdlib::BuiltinSpec* builtin = stdlib::findBuiltin(call.callee);
        if (builtin == nullptr) {
            if (isClassNameInScope(call.callee)) {
                fail(call, "'" + call.callee +
                               "' is a class — instantiate it with 'new " + call.callee +
                               "(...)'");
            }
            fail(call, "call to unknown function '" + call.callee + "'");
        }
        if (argc < builtin->minArgs) {
            fail(call, "'" + builtin->name + "' expects at least " +
                            std::to_string(builtin->minArgs) + " argument(s), got " +
                            std::to_string(argc));
        }
        if (builtin->maxArgs != SIZE_MAX && argc > builtin->maxArgs) {
            fail(call, "'" + builtin->name + "' expects at most " +
                            std::to_string(builtin->maxArgs) + " argument(s), got " +
                            std::to_string(argc));
        }
    }

    for (const auto& argument : call.arguments) {
        compileExpression(chunk, *argument);
    }

    // A unit's own functions carry its hidden prefix; built-ins stay raw.
    const std::string calleeName = (userOverloads != declaredFunctions_.end())
                                       ? unitName(call.callee)
                                       : call.callee;
    const std::uint32_t nameIndex = chunk.addName(calleeName);
    chunk.writeOp(OpCode::Call);
    chunk.writeU32(nameIndex);
    chunk.writeU32(static_cast<std::uint32_t>(argc));
}

// Member READ (car.name / self.speed / Car.count / math.Vector.count). The
// receiver decides the path: class name → static field; self → checked
// instance field; anything else → dynamic access validated by the VM.
void Compiler::compileMemberAccess(Chunk& chunk, const ast::MemberAccessExpr& access) {
    const std::vector<int> scope = memberScopeChain();

    // ---- package-rooted static field read (math.Vector.count / Vector.count)
    if (access.object->kind == ast::NodeKind::MemberAccessExpr ||
        (unit_ != nullptr && access.object->kind == ast::NodeKind::Identifier)) {
        int classIdx = -1;
        std::string display;
        if (resolvePackageClassPath(*access.object, classIdx, display)) {
            const ResolvedMember found =
                findFieldInChain(classIdx, access.memberName, /*wantStatic=*/true);
            if (found.field == nullptr) {
                if (findFieldInChain(classIdx, access.memberName, false).field !=
                    nullptr) {
                    fail(access, "'" + access.memberName + "' is an instance field of "
                                 "class '" + display +
                                     "' (access it through an instance)");
                }
                fail(access, "class '" + display + "' has no static field '" +
                                 access.memberName + "'");
            }
            if (found.field->isPrivate && found.ownerIndex != ownerClassIndex()) {
                fail(access, "private static field '" + access.memberName +
                                 "' of class '" + display +
                                 "' is not accessible here");
            }
            chunk.writeOp(OpCode::GetStatic);
            chunk.writeU32(static_cast<std::uint32_t>(classIdx));
            chunk.writeU32(chunk.addName(access.memberName));
            return;
        }
    }

    if (access.object->kind == ast::NodeKind::Identifier) {
        const auto& object = static_cast<const ast::Identifier&>(*access.object);
        // Package-system names (active unit only): selectively imported class
        // statics read as Vector.count; package namespaces are not values.
        if (unit_ != nullptr) {
            const auto selective = unit_->selective.find(object.name);
            if (selective != unit_->selective.end() && selective->second.isClass) {
                const int classIdx = selective->second.classIndex;
                const ResolvedMember found =
                    findFieldInChain(classIdx, access.memberName, /*wantStatic=*/true);
                if (found.field == nullptr) {
                    if (findFieldInChain(classIdx, access.memberName, false).field !=
                        nullptr) {
                        fail(access, "'" + access.memberName +
                                     "' is an instance field of class '" + object.name +
                                     "' (access it through an instance)");
                    }
                    fail(access, "class '" + object.name + "' has no static field '" +
                                     access.memberName + "'");
                }
                if (found.field->isPrivate && found.ownerIndex != ownerClassIndex()) {
                    fail(access, "private static field '" + access.memberName +
                                     "' of class '" + object.name +
                                     "' is not accessible here");
                }
                chunk.writeOp(OpCode::GetStatic);
                chunk.writeU32(static_cast<std::uint32_t>(classIdx));
                chunk.writeU32(chunk.addName(access.memberName));
                return;
            }
            if (unit_->imports.count(object.name) > 0) {
                const PackageView& view = unit_->imports[object.name];
                if (view.classes.count(access.memberName) > 0) {
                    fail(access, "'" + object.name + "." + access.memberName +
                                     "' is a class, not a value (instantiate it with "
                                     "'new " + object.name + "." + access.memberName +
                                     "(...)')");
                }
                if (view.functions.count(access.memberName) > 0) {
                    fail(access, "function '" + object.name + "." + access.memberName +
                                     "' cannot be used as a value (functions are not "
                                     "values in Runa yet)");
                }
                fail(access, "package '" + object.name + "' has no member '" +
                                 access.memberName + "'");
            }
            if (unit_->cimports.count(object.name) > 0) {
                fail(access, "native function '" + object.name + "." +
                                 access.memberName +
                                 "' cannot be used as a value (call it: " + object.name +
                                 "." + access.memberName + "(...))");
            }
        }
        const int classIdx = resolveClass(object.name, scope);
        if (classIdx >= 0) {
            const ClassEntry& entry = classes_[static_cast<std::size_t>(classIdx)];
            const ResolvedMember found =
                findFieldInChain(classIdx, access.memberName, /*wantStatic=*/true);
            if (found.field == nullptr) {
                if (findFieldInChain(classIdx, access.memberName, false).field != nullptr) {
                    fail(access, "'" + access.memberName + "' is an instance field of "
                                 "class '" + entry.qualifiedName +
                                     "' (access it through an instance)");
                }
                fail(access, "class '" + entry.qualifiedName + "' has no static field '" +
                                 access.memberName + "'");
            }
            if (found.field->isPrivate && found.ownerIndex != ownerClassIndex()) {
                fail(access, "private static field '" + access.memberName + "' of class '" +
                                 classes_[static_cast<std::size_t>(found.ownerIndex)]
                                     .qualifiedName +
                                     "' is not accessible here");
            }
            chunk.writeOp(OpCode::GetStatic);
            chunk.writeU32(static_cast<std::uint32_t>(classIdx));
            chunk.writeU32(chunk.addName(access.memberName));
            return;
        }
    }

    if (access.object->kind == ast::NodeKind::SelfExpr) {
        if (ownerClass_ == nullptr || !inFunction_) {
            fail(access, "'self' is only available inside instance methods and "
                         "constructors");
        }
        if (inStaticMethod_) {
            fail(access, "'self' is not available in a static method");
        }
        const ResolvedMember found =
            findFieldInChain(ownerClassIndex(), access.memberName, /*wantStatic=*/false);
        if (found.field == nullptr) {
            if (findFieldInChain(ownerClassIndex(), access.memberName, true).field !=
                nullptr) {
                fail(access, "'" + access.memberName + "' is a static field (read it as '" +
                                 ownerClass_->qualifiedName + "." + access.memberName + "')");
            }
            fail(access, "class '" + ownerClass_->qualifiedName + "' has no field '" +
                             access.memberName + "'");
        }
        if (found.field->isPrivate && found.ownerIndex != ownerClassIndex()) {
            fail(access, "private field '" + access.memberName + "' of class '" +
                             classes_[static_cast<std::size_t>(found.ownerIndex)]
                                 .qualifiedName +
                                 "' is not accessible from '" + ownerClass_->qualifiedName +
                                 "'");
        }
        chunk.writeOp(OpCode::GetSelf);
        chunk.writeOp(OpCode::GetField);
        chunk.writeU32(chunk.addName(access.memberName));
        return;
    }

    // Dynamic receiver — the VM validates object-ness, existence and privacy.
    compileExpression(chunk, *access.object);
    chunk.writeOp(OpCode::GetField);
    chunk.writeU32(chunk.addName(access.memberName));
}

// Method call (car.go() / self.go() / Car.make(); super.go() is separate).
// Package paths root here too: math.sqrt(...), graphics.create_window(...),
// math.Vector.make(...) and Vector.make(...) (selective class).
void Compiler::compileMethodCall(Chunk& chunk, const ast::MethodCallExpr& call) {
    const std::size_t argc = call.arguments.size();
    const std::vector<int> scope = memberScopeChain();

    // ---- package/native namespace member call -------------------------------
    if (unit_ != nullptr && call.object->kind == ast::NodeKind::Identifier) {
        const auto& object = static_cast<const ast::Identifier&>(*call.object);

        const auto imported = unit_->imports.find(object.name);
        if (imported != unit_->imports.end()) {
            const PackageView& view = imported->second;
            const auto fn = view.functions.find(call.methodName);
            if (fn == view.functions.end()) {
                if (view.classes.count(call.methodName) > 0) {
                    fail(call, "'" + object.name + "." + call.methodName +
                                   "' is a class — instantiate it with 'new " +
                                   object.name + "." + call.methodName + "(...)'");
                }
                fail(call, "package '" + object.name + "' has no function '" +
                               call.methodName + "'");
            }
            if (fn->second != argc) {
                fail(call, "'" + object.name + "." + call.methodName + "' expects " +
                               std::to_string(fn->second) + " argument(s), got " +
                               std::to_string(argc));
            }
            for (const auto& argument : call.arguments) {
                compileExpression(chunk, *argument);
            }
            chunk.writeOp(OpCode::Call);
            chunk.writeU32(chunk.addName(view.prefix + call.methodName));
            chunk.writeU32(static_cast<std::uint32_t>(argc));
            return;
        }

        const auto cimported = unit_->cimports.find(object.name);
        if (cimported != unit_->cimports.end()) {
            const auto fn = cimported->second.find(call.methodName);
            if (fn == cimported->second.end()) {
                fail(call, "native package '" + object.name + "' has no function '" +
                               call.methodName + "'");
            }
            compileNativeCall(chunk, fn->second, call.arguments, call,
                              object.name + "." + call.methodName);
            return;
        }

        // Selectively imported class: Vector.make(...)
        const auto selective = unit_->selective.find(object.name);
        if (selective != unit_->selective.end() && selective->second.isClass) {
            const int classIdx = selective->second.classIndex;
            const ClassEntry& entry = classes_[static_cast<std::size_t>(classIdx)];
            const ResolvedMember found = findMethodInChain(
                classIdx, call.methodName, argc, /*wantStatic=*/true,
                /*includePrivate=*/true);
            if (found.method == nullptr) {
                fail(call, "class '" + entry.qualifiedName + "' has no static method '" +
                               call.methodName + "' with " + std::to_string(argc) +
                               " argument(s)");
            }
            if (found.method->isPrivate && found.ownerIndex != ownerClassIndex()) {
                fail(call, "private static method '" + call.methodName +
                               "' of class '" + entry.qualifiedName +
                               "' is not accessible here");
            }
            for (const auto& argument : call.arguments) {
                compileExpression(chunk, *argument);
            }
            chunk.writeOp(OpCode::InvokeStatic);
            chunk.writeU32(static_cast<std::uint32_t>(classIdx));
            chunk.writeU32(chunk.addName(call.methodName));
            chunk.writeU32(static_cast<std::uint32_t>(argc));
            return;
        }
    }

    // ---- package-rooted class path: math.Vector.make(...) -------------------
    if (call.object->kind == ast::NodeKind::MemberAccessExpr) {
        int classIdx = -1;
        std::string display;
        if (resolvePackageClassPath(*call.object, classIdx, display)) {
            const ResolvedMember found = findMethodInChain(
                classIdx, call.methodName, argc, /*wantStatic=*/true,
                /*includePrivate=*/true);
            if (found.method == nullptr) {
                fail(call, "class '" + display + "' has no static method '" +
                               call.methodName + "' with " + std::to_string(argc) +
                               " argument(s)");
            }
            if (found.method->isPrivate && found.ownerIndex != ownerClassIndex()) {
                fail(call, "private static method '" + call.methodName +
                               "' of class '" + display + "' is not accessible here");
            }
            for (const auto& argument : call.arguments) {
                compileExpression(chunk, *argument);
            }
            chunk.writeOp(OpCode::InvokeStatic);
            chunk.writeU32(static_cast<std::uint32_t>(classIdx));
            chunk.writeU32(chunk.addName(call.methodName));
            chunk.writeU32(static_cast<std::uint32_t>(argc));
            return;
        }
    }

    // ---- ClassName.staticMethod(args) --------------------------------------
    if (call.object->kind == ast::NodeKind::Identifier) {
        const auto& object = static_cast<const ast::Identifier&>(*call.object);
        const int classIdx = resolveClass(object.name, scope);
        if (classIdx >= 0) {
            const ClassEntry& entry = classes_[static_cast<std::size_t>(classIdx)];
            const ResolvedMember found = findMethodInChain(
                classIdx, call.methodName, argc, /*wantStatic=*/true, /*includePrivate=*/true);
            if (found.method == nullptr) {
                if (findMethodInChain(classIdx, call.methodName, argc, false, true)
                        .method != nullptr) {
                    fail(call, "'" + call.methodName + "' is an instance method of class '" +
                                   entry.qualifiedName + "' (call it through an instance)");
                }
                fail(call, "class '" + entry.qualifiedName + "' has no static method '" +
                               call.methodName + "' with " + std::to_string(argc) +
                               " argument(s)");
            }
            if (found.method->isPrivate && found.ownerIndex != ownerClassIndex()) {
                fail(call, "private static method '" + call.methodName + "' of class '" +
                                classes_[static_cast<std::size_t>(found.ownerIndex)]
                                    .qualifiedName +
                                "' is not accessible here");
            }
            for (const auto& argument : call.arguments) {
                compileExpression(chunk, *argument);
            }
            chunk.writeOp(OpCode::InvokeStatic);
            chunk.writeU32(static_cast<std::uint32_t>(classIdx));
            chunk.writeU32(chunk.addName(call.methodName));
            chunk.writeU32(static_cast<std::uint32_t>(argc));
            return;
        }
    }

    // ---- self.method(args): compile-time existence + visibility check -------
    if (call.object->kind == ast::NodeKind::SelfExpr) {
        if (ownerClass_ == nullptr || !inFunction_) {
            fail(call, "'self' is only available inside instance methods and "
                       "constructors");
        }
        if (inStaticMethod_) {
            fail(call, "'self' is not available in a static method");
        }
        const ResolvedMember found =
            findMethodInChain(ownerClassIndex(), call.methodName, argc,
                              /*wantStatic=*/false, /*includePrivate=*/true);
        if (found.method == nullptr) {
            if (findMethodInChain(ownerClassIndex(), call.methodName, argc, true, true)
                    .method != nullptr) {
                fail(call, "'" + call.methodName + "' is a static method (call it as '" +
                               ownerClass_->qualifiedName + "." + call.methodName + "(...)')");
            }
            fail(call, "class '" + ownerClass_->qualifiedName + "' has no method '" +
                           call.methodName + "' with " + std::to_string(argc) +
                           " argument(s)");
        }
        if (found.method->isPrivate && found.ownerIndex != ownerClassIndex()) {
            fail(call, "private method '" + call.methodName + "' of class '" +
                            classes_[static_cast<std::size_t>(found.ownerIndex)]
                                .qualifiedName +
                            "' is not accessible from '" + ownerClass_->qualifiedName + "'");
        }
        chunk.writeOp(OpCode::GetSelf);
        for (const auto& argument : call.arguments) {
            compileExpression(chunk, *argument);
        }
        chunk.writeOp(OpCode::InvokeVirtual);
        chunk.writeU32(chunk.addName(call.methodName));
        chunk.writeU32(static_cast<std::uint32_t>(argc));
        return;
    }

    // ---- dynamic receiver: runtime dispatch ----------------------------------
    compileExpression(chunk, *call.object);
    for (const auto& argument : call.arguments) {
        compileExpression(chunk, *argument);
    }
    chunk.writeOp(OpCode::InvokeVirtual);
    chunk.writeU32(chunk.addName(call.methodName));
    chunk.writeU32(static_cast<std::uint32_t>(argc));
}

void Compiler::compileNewObject(Chunk& chunk, const ast::NewExpr& creation) {
    const std::size_t argc = creation.arguments.size();
    int classIdx = -1;
    const std::size_t dotPosition = creation.className.find('.');

    // Package-system resolution (active unit only).
    if (unit_ != nullptr && dotPosition != std::string::npos) {
        const std::string root = creation.className.substr(0, dotPosition);
        const std::string rest = creation.className.substr(dotPosition + 1);
        if (rest.find('.') != std::string::npos) {
            fail(creation, "nested classes of an imported class are not accessible ('" +
                               creation.className + "')");
        }
        const auto imported = unit_->imports.find(root);
        if (imported != unit_->imports.end()) {
            const auto cls = imported->second.classes.find(rest);
            if (cls == imported->second.classes.end()) {
                fail(creation, "package '" + root + "' has no class '" + rest + "'");
            }
            classIdx = cls->second;
        } else if (unit_->cimports.count(root) > 0) {
            fail(creation, "'" + root +
                               "' is a native package — native packages have no classes");
        } else {
            fail(creation, "unknown class '" + creation.className + "'");
        }
    } else if (unit_ != nullptr) {
        const auto selective = unit_->selective.find(creation.className);
        if (selective != unit_->selective.end()) {
            if (!selective->second.isClass) {
                fail(creation, "'" + creation.className +
                                   "' is an imported function, not a class");
            }
            classIdx = selective->second.classIndex;
        } else if (unit_->imports.count(creation.className) > 0 ||
                   unit_->cimports.count(creation.className) > 0) {
            fail(creation, "'" + creation.className +
                               "' is a package name — instantiate its classes as 'new " +
                               creation.className + ".ClassName(...)'");
        }
    }

    if (classIdx < 0) {
        classIdx = resolveClass(creation.className, memberScopeChain());
    }
    // Qualified nested-class construction from outside (spec sections 51/52):
    // new Car::Engine() walks the nesting chain from the global scope.
    if (classIdx < 0 &&
        creation.className.find("::") != std::string::npos) {
        classIdx = resolveQualifiedNestedClass(creation.className);
    }
    if (classIdx < 0) {
        fail(creation, "unknown class '" + creation.className + "'");
    }
    const ClassEntry& entry = classes_[static_cast<std::size_t>(classIdx)];
    if (entry.declaration->isInterface) {
        fail(creation, "cannot instantiate interface '" + entry.qualifiedName + "'");
    }
    if (entry.declaration->isAbstract) {
        fail(creation, "cannot instantiate abstract class '" + entry.qualifiedName + "'");
    }

    // Constructors are NOT inherited: only the class's own constructors count.
    // Overload resolution is by argument count (spec sections 29/30).
    const ClassMethod* constructor = nullptr;
    for (const ClassMethod& method : entry.methods) {
        if (method.isConstructor && method.parameters->size() == argc) {
            constructor = &method;
            break;
        }
    }
    if (constructor == nullptr) {
        // Report the available arities for a deterministic message. A class
        // with no constructor accepts only new X() (implicit constructor,
        // spec 31); a class WITH constructors rejects any unmatched arity —
        // at compile time, because the class here is statically known.
        std::string arities;
        for (const ClassMethod& method : entry.methods) {
            if (!method.isConstructor) continue;
            if (!arities.empty()) arities += "/";
            arities += std::to_string(method.parameters->size());
        }
        if (arities.empty()) {
            if (argc > 0) {
                fail(creation, "class '" + entry.qualifiedName +
                                   "' has no constructor (new " + entry.name +
                                   "() takes no arguments, got " + std::to_string(argc) + ")");
            }
        } else {
            fail(creation, "no constructor of '" + entry.qualifiedName + "' takes " +
                               std::to_string(argc) + " argument(s) (expected " +
                               arities + ")");
        }
    }

    for (const auto& argument : creation.arguments) {
        compileExpression(chunk, *argument);
    }
    chunk.writeOp(OpCode::NewObject);
    chunk.writeU32(static_cast<std::uint32_t>(classIdx));
    chunk.writeU32(static_cast<std::uint32_t>(argc));
}

void Compiler::compileSuperCall(Chunk& chunk, const ast::SuperCallExpr& call) {
    const std::size_t argc = call.arguments.size();
    if (ownerClass_ == nullptr || inStaticMethod_ || !inFunction_) {
        fail(call, "'super' is only valid inside instance methods and constructors");
    }
    if (ownerClass_->parentIndex < 0) {
        fail(call, "class '" + ownerClass_->qualifiedName +
                       "' has no parent class — 'super' cannot be used here");
    }
    const ResolvedMember found =
        findMethodInChain(ownerClass_->parentIndex, call.methodName, argc,
                          /*wantStatic=*/false, /*includePrivate=*/false);
    if (found.method == nullptr) {
        fail(call, "class '" +
                       classes_[static_cast<std::size_t>(ownerClass_->parentIndex)]
                           .qualifiedName +
                       "' has no visible method '" + call.methodName + "' with " +
                       std::to_string(argc) + " argument(s) to call via 'super'");
    }
    chunk.writeOp(OpCode::GetSelf);
    for (const auto& argument : call.arguments) {
        compileExpression(chunk, *argument);
    }
    chunk.writeOp(OpCode::InvokeSuper);
    chunk.writeU32(chunk.addName(call.methodName));
    chunk.writeU32(static_cast<std::uint32_t>(argc));
}

void Compiler::compileIsExpr(Chunk& chunk, const ast::IsExpr& check) {
    int classIdx = -1;
    const std::size_t dotPosition = check.className.find('.');

    // Package-system resolution (active unit only).
    if (unit_ != nullptr && dotPosition != std::string::npos) {
        const std::string root = check.className.substr(0, dotPosition);
        const std::string rest = check.className.substr(dotPosition + 1);
        if (rest.find('.') != std::string::npos) {
            fail(check, "nested classes of an imported class are not accessible ('" +
                            check.className + "')");
        }
        const auto imported = unit_->imports.find(root);
        if (imported != unit_->imports.end()) {
            const auto cls = imported->second.classes.find(rest);
            if (cls == imported->second.classes.end()) {
                fail(check, "package '" + root + "' has no class '" + rest + "'");
            }
            classIdx = cls->second;
        } else if (unit_->cimports.count(root) > 0) {
            fail(check, "'" + root + "' is a native package — it has no classes");
        } else {
            fail(check, "'" + check.className + "' is not a class");
        }
    } else if (unit_ != nullptr) {
        const auto selective = unit_->selective.find(check.className);
        if (selective != unit_->selective.end() && selective->second.isClass) {
            classIdx = selective->second.classIndex;
        } else if (unit_->imports.count(check.className) > 0 ||
                   unit_->cimports.count(check.className) > 0) {
            fail(check, "'" + check.className + "' is a package, not a class");
        }
    }

    if (classIdx < 0) {
        classIdx = resolveClass(check.className, memberScopeChain());
    }
    if (classIdx < 0) {
        fail(check, "'is' expects a class or interface name — '" + check.className +
                        "' is not a class");
    }
    compileExpression(chunk, *check.value);
    chunk.writeOp(OpCode::IsType);
    chunk.writeU32(static_cast<std::uint32_t>(classIdx));
}

// ============================================================================
// PASS 4: class machinery
// ============================================================================

void Compiler::emitClassMachine(Chunk& chunk) {
    std::vector<bytecode::ClassInfo> infos(classes_.size());
    for (std::size_t i = 0; i < classes_.size(); ++i) {
        const ClassEntry& entry = classes_[i];
        bytecode::ClassInfo& info = infos[i];
        info.name = entry.qualifiedName;
        info.parentIndex = entry.parentIndex >= 0
                               ? static_cast<std::uint32_t>(entry.parentIndex)
                               : bytecode::kNoParent;
        info.isAbstract = entry.declaration->isAbstract;
        info.isInterface = entry.declaration->isInterface;
        // RESOLVED interface names (qualified) so the VM can rebuild the
        // transitive interface sets without name-lookup ambiguity.
        for (int interfaceIdx : entry.interfaceIndices) {
            info.interfaceNames.push_back(
                classes_[static_cast<std::size_t>(interfaceIdx)].qualifiedName);
        }

        activateUnit(entry.unitIndex);
        emitClassMethods(chunk, info, entry);
        emitFieldInitializerBlock(chunk, info, entry);
        emitStaticInitializerBlock(chunk, info, entry);
    }
    chunk.classes = std::move(infos);
}

void Compiler::emitClassMethods(Chunk& chunk, bytecode::ClassInfo& info,
                                const ClassEntry& entry) {
    for (const ClassMethod& method : entry.methods) {
        bytecode::ClassMethodInfo methodInfo;
        methodInfo.name = method.name;
        methodInfo.isStatic = method.isStatic;
        methodInfo.isPrivate = method.isPrivate;
        methodInfo.isAbstract = method.isAbstract;
        methodInfo.isConstructor = method.isConstructor;
        for (const ast::Parameter& parameter : *method.parameters) {
            ensureNotCoreReserved(parameter.name, *entry.declaration,
                                  "a parameter name");
            methodInfo.parameters.push_back(parameter.name);
        }

        if (method.isAbstract) {
            methodInfo.entryOffset = bytecode::kNoOffset;
        } else {
            // Context: a method body behaves like a function body (locals,
            // returns) plus class context (self/super/visibility).
            const bool previousInFunction = inFunction_;
            const ClassEntry* previousOwner = ownerClass_;
            const bool previousStatic = inStaticMethod_;
            const bool previousConstructor = inConstructor_;

            inFunction_ = true;
            ownerClass_ = &entry;
            inStaticMethod_ = method.isStatic;
            inConstructor_ = method.isConstructor;
            // Same boundary rule as function bodies: break/continue stay
            // inside the method they are written in.
            const std::size_t previousLoopDepth = loopStack_.size();
            loopStack_.clear();
            methodInfo.entryOffset = static_cast<std::uint32_t>(chunk.code.size());
            for (const auto& statement : *method.body) {
                compileStatement(chunk, *statement);
            }
            chunk.writeOp(OpCode::ReturnNone);
            loopStack_.resize(previousLoopDepth);

            inFunction_ = previousInFunction;
            ownerClass_ = previousOwner;
            inStaticMethod_ = previousStatic;
            inConstructor_ = previousConstructor;
        }
        info.methods.push_back(std::move(methodInfo));
    }
}

// One block per class: evaluate every instance field default (in declaration
// order) and InitField it into the instance being constructed. The VM runs
// these blocks base-class first when executing 'new'. The block starts at the
// FIRST field's initializer offset and ends with ReturnNone (the VM's plan
// machinery advances to the next step on that return).
void Compiler::emitFieldInitializerBlock(Chunk& chunk, bytecode::ClassInfo& info,
                                         const ClassEntry& entry) {
    const ClassEntry* previousOwner = ownerClass_;
    const bool previousInFunction = inFunction_;
    ownerClass_ = &entry;
    inFunction_ = false;  // no locals/return: field initializers are expressions

    bool emittedAny = false;
    for (const ClassField& field : entry.fields) {
        if (field.isStatic) continue;
        bytecode::ClassFieldInfo fieldInfo;
        fieldInfo.name = field.name;
        fieldInfo.isStatic = false;
        fieldInfo.isPrivate = field.isPrivate;
        fieldInfo.initializerOffset = static_cast<std::uint32_t>(chunk.code.size());
        compileExpression(chunk, *field.initializer);
        chunk.writeOp(OpCode::InitField);
        chunk.writeU32(chunk.addName(field.name));
        info.fields.push_back(std::move(fieldInfo));
        emittedAny = true;
    }
    if (emittedAny) {
        chunk.writeOp(OpCode::ReturnNone);
    }

    ownerClass_ = previousOwner;
    inFunction_ = previousInFunction;
}

// One block per class: evaluate every static field default and StoreStatic it
// into the class's storage. The VM runs these once at program start, in class
// registration order.
void Compiler::emitStaticInitializerBlock(Chunk& chunk, bytecode::ClassInfo& info,
                                          const ClassEntry& entry) {
    const ClassEntry* previousOwner = ownerClass_;
    const bool previousInFunction = inFunction_;
    ownerClass_ = &entry;
    const int classIdx = ownerClassIndex();
    inFunction_ = false;

    bool emittedAny = false;
    for (const ClassField& field : entry.fields) {
        if (!field.isStatic) continue;
        bytecode::ClassFieldInfo fieldInfo;
        fieldInfo.name = field.name;
        fieldInfo.isStatic = true;
        fieldInfo.isPrivate = field.isPrivate;
        fieldInfo.initializerOffset = static_cast<std::uint32_t>(chunk.code.size());
        compileExpression(chunk, *field.initializer);
        chunk.writeOp(OpCode::StoreStatic);
        chunk.writeU32(static_cast<std::uint32_t>(classIdx));
        chunk.writeU32(chunk.addName(field.name));
        info.fields.push_back(std::move(fieldInfo));
        emittedAny = true;
    }
    if (emittedAny) {
        chunk.writeOp(OpCode::ReturnNone);
    }

    ownerClass_ = previousOwner;
    inFunction_ = previousInFunction;
}

}  // namespace runa::compiler
