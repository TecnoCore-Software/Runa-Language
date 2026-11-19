#include "vm/vm.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <utility>

#include "import/platform.hpp"
#include "runtime/heap.hpp"
#include "stdlib/stdlib.hpp"

namespace runa::vm {

using bytecode::OpCode;

namespace {

// Source symbol of an arithmetic opcode, for error messages.
const char* arithmeticSymbol(OpCode op) {
    switch (op) {
        case OpCode::Add: return "+";
        case OpCode::Sub: return "-";
        case OpCode::Mul: return "*";
        case OpCode::Div: return "/";
        case OpCode::Mod: return "%";
        default:          return "?";
    }
}

// Source symbol of a comparison opcode, for error messages.
const char* comparisonSymbol(OpCode op) {
    switch (op) {
        case OpCode::Eq: return "==";
        case OpCode::Ne: return "!=";
        case OpCode::Gt: return ">";
        case OpCode::Lt: return "<";
        case OpCode::Ge: return ">=";
        case OpCode::Le: return "<=";
        default:         return "?";
    }
}

// Signed 64-bit overflow predicates. Written without any operation that
// could itself overflow, so the VM never triggers C++ undefined behavior.
bool addOverflows(std::int64_t a, std::int64_t b) {
    return (b > 0) ? (a > std::numeric_limits<std::int64_t>::max() - b)
                   : (a < std::numeric_limits<std::int64_t>::min() - b);
}

bool subOverflows(std::int64_t a, std::int64_t b) {
    return (b < 0) ? (a > std::numeric_limits<std::int64_t>::max() + b)
                   : (a < std::numeric_limits<std::int64_t>::min() + b);
}

bool mulOverflows(std::int64_t a, std::int64_t b) {
    if (a == 0 || b == 0) return false;
    if (a == -1) return b == std::numeric_limits<std::int64_t>::min();
    if (b == -1) return a == std::numeric_limits<std::int64_t>::min();
    const std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    const std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    if (a > 0 && b > 0) return a > kMax / b;
    if (a < 0 && b < 0) return a < kMax / b;
    return (a > 0) ? (a > kMin / b) : (b > kMin / a);
}

// The simple (unqualified) part of a class name: "Car::Engine" → "Engine".
std::string simpleClassName(const std::string& qualified) {
    const std::size_t pos = qualified.rfind("::");
    return (pos == std::string::npos) ? qualified : qualified.substr(pos + 2);
}

// Renders compiler-internal hidden names for humans:
//   '#pkg.math#sqrt'  → 'math.sqrt'      '#init#math#'  → 'math.<init>'
std::string friendlyName(const std::string& name) {
    if (name.rfind("#init#", 0) == 0 && name.size() > 6 && name.back() == '#') {
        return name.substr(5, name.size() - 6) + ".<init>";
    }
    if (name.rfind("#pkg.", 0) == 0) {
        const std::size_t end = name.find('#', 5);
        if (end != std::string::npos && end + 1 < name.size()) {
            return name.substr(5, end - 5) + "." + name.substr(end + 1);
        }
    }
    return name;
}

// The '#pkg.<name>#' unit prefix carried by a compiled name (empty for the
// main unit's names).
std::string unitPrefixOf(const std::string& name) {
    if (name.rfind("#pkg.", 0) == 0) {
        const std::size_t end = name.find('#', 5);
        if (end != std::string::npos) return name.substr(0, end + 1);
    }
    return "";
}

}  // namespace

VirtualMachine::VirtualMachine(Options options) : options_(std::move(options)) {}

std::ostream& VirtualMachine::outputStream() const {
    return options_.output ? *options_.output : std::cout;
}

std::uint32_t VirtualMachine::readU32(const bytecode::Chunk& chunk) {
    if (ip_ + 4 > chunk.code.size()) {
        runtimeFail("truncated instruction operand");
    }
    std::uint32_t value =
        static_cast<std::uint32_t>(chunk.code[ip_]) |
        (static_cast<std::uint32_t>(chunk.code[ip_ + 1]) << 8) |
        (static_cast<std::uint32_t>(chunk.code[ip_ + 2]) << 16) |
        (static_cast<std::uint32_t>(chunk.code[ip_ + 3]) << 24);
    ip_ += 4;
    return value;
}

void VirtualMachine::runtimeFail(const std::string& message) const {
    errors::RunaError error;
    error.kind = errors::ErrorKind::RuntimeError;
    // No source mapping yet: debug information in bytecode is a later phase
    // (baseline spec, section 12).
    error.location = errors::SourceLocation{};
    error.message = message;
    throw errors::RunaErrorException(error);
}

// ============================================================================
// Class registry (built from the chunk's class table at startup)
// ============================================================================

std::vector<std::uint32_t> VirtualMachine::classChain(std::uint32_t index) const {
    std::vector<std::uint32_t> chain;
    std::uint32_t current = index;
    std::size_t guard = 0;
    while (current != bytecode::kNoParent) {
        if (current >= classes_.size()) {
            runtimeFail("malformed bytecode: class index out of range");
        }
        if (++guard > classes_.size()) {
            runtimeFail("malformed bytecode: inheritance cycle involving class '" +
                        classes_[current].qualifiedName + "'");
        }
        chain.push_back(current);
        current = classes_[current].parent;
    }
    std::reverse(chain.begin(), chain.end());  // base class first
    return chain;
}

void VirtualMachine::buildClassRegistry(const bytecode::Chunk& chunk) {
    classes_.assign(chunk.classes.size(), ClassRuntime{});

    // Pass 1: identities and direct flags.
    std::map<std::string, std::uint32_t> byQualifiedName;
    for (std::size_t i = 0; i < chunk.classes.size(); ++i) {
        const bytecode::ClassInfo& info = chunk.classes[i];
        ClassRuntime& entry = classes_[i];
        entry.qualifiedName = info.name;
        entry.name = simpleClassName(info.name);
        entry.parent = info.parentIndex;
        entry.isAbstract = info.isAbstract;
        entry.isInterface = info.isInterface;
        byQualifiedName[info.name] = static_cast<std::uint32_t>(i);
    }

    // Pass 2: merged tables along each chain (base first, nearest wins).
    for (std::size_t i = 0; i < chunk.classes.size(); ++i) {
        ClassRuntime& entry = classes_[i];
        const std::vector<std::uint32_t> chain = classChain(static_cast<std::uint32_t>(i));

        for (std::uint32_t ancestorIndex : chain) {
            const bytecode::ClassInfo& ancestor = chunk.classes[ancestorIndex];
            for (const bytecode::ClassMethodInfo& method : ancestor.methods) {
                if (method.isConstructor) {
                    if (ancestorIndex == i) {
                        entry.constructorsByArity[method.parameters.size()] =
                            &method;
                    }
                    continue;
                }
                // Abstract declarations are not callable — a concrete class
                // must provide an implementation (compile-time enforced), so
                // they never need a merged entry.
                if (method.isAbstract || method.entryOffset == bytecode::kNoOffset) {
                    continue;
                }
                ClassRuntime::MethodRT runtime2;
                runtime2.owner = ancestorIndex;
                runtime2.entry = method.entryOffset;
                runtime2.isStatic = method.isStatic;
                runtime2.isPrivate = method.isPrivate;
                runtime2.parameters = method.parameters;
                const auto signature =
                    std::make_pair(method.name, method.parameters.size());
                if (method.isStatic) {
                    entry.staticMethods[signature] = runtime2;
                } else {
                    entry.instanceMethods[signature] = runtime2;
                }
            }
            for (const bytecode::ClassFieldInfo& field : ancestor.fields) {
                ClassRuntime::FieldRT owner;
                owner.owner = ancestorIndex;
                owner.isStatic = field.isStatic;
                owner.isPrivate = field.isPrivate;
                entry.fieldOwners[field.name] = owner;
            }
            // Transitive interface set (interfaces cannot inherit, but a
            // subclass inherits every interface of its ancestors).
            for (const std::string& interfaceName : ancestor.interfaceNames) {
                const auto found = byQualifiedName.find(interfaceName);
                if (found == byQualifiedName.end()) {
                    runtimeFail("malformed bytecode: unknown interface '" +
                                interfaceName + "' on class '" +
                                classes_[ancestorIndex].qualifiedName + "'");
                }
                if (!chunk.classes[found->second].isInterface) {
                    runtimeFail("malformed bytecode: '" + interfaceName +
                                "' is not an interface");
                }
                if (std::find(entry.interfaces.begin(), entry.interfaces.end(),
                              found->second) == entry.interfaces.end()) {
                    entry.interfaces.push_back(found->second);
                }
            }
        }

        // Block entry offsets come from the class's OWN fields only (they are
        // the entry points of the initializer blocks emitted for THIS class;
        // inherited fields have their own blocks in their own classes).
        ClassRuntime& self2 = classes_[i];
        for (const bytecode::ClassFieldInfo& field : chunk.classes[i].fields) {
            if (field.isStatic) {
                if (self2.firstStaticFieldInit == bytecode::kNoOffset) {
                    self2.firstStaticFieldInit = field.initializerOffset;
                }
            } else if (self2.firstInstanceFieldInit == bytecode::kNoOffset) {
                self2.firstInstanceFieldInit = field.initializerOffset;
            }
        }
    }
}

void VirtualMachine::runStaticInitializers(const bytecode::Chunk& chunk) {
    (void)chunk;
    FramePlan plan;
    plan.kind = FramePlan::Kind::Startup;
    // v0.9: the inits table carries the per-unit startup order (dependencies
    // first, the main unit last): each unit's class static initializers run,
    // then its package-init body. A file without an inits table (legacy
    // defense) keeps the flat class-registration-order behaviour.
    if (!chunk.inits.empty()) {
        for (const bytecode::InitInfo& unit : chunk.inits) {
            for (std::uint32_t c = 0; c < unit.classCount; ++c) {
                const std::uint32_t classIndex = unit.firstClassIndex + c;
                if (classIndex >= classes_.size()) {
                    runtimeFail("malformed bytecode: unit class index out of range");
                }
                if (classes_[classIndex].firstStaticFieldInit != bytecode::kNoOffset) {
                    FramePlanStep step;
                    step.kind = FramePlanStep::Kind::StaticInit;
                    step.classIndex = classIndex;
                    step.entryOffset = classes_[classIndex].firstStaticFieldInit;
                    plan.steps.push_back(step);
                }
            }
            if (unit.initEntryOffset != bytecode::kNoOffset) {
                if (unit.initEntryOffset >= chunk.code.size()) {
                    runtimeFail("malformed bytecode: init entry outside the code");
                }
                FramePlanStep step;
                step.kind = FramePlanStep::Kind::PackageInit;
                step.classIndex = 0;
                step.entryOffset = unit.initEntryOffset;
                step.unitPrefix = "#pkg." + unit.packageName + "#";
                step.packageName = unit.packageName;
                plan.steps.push_back(step);
            }
        }
    } else {
        for (std::size_t i = 0; i < classes_.size(); ++i) {
            if (classes_[i].firstStaticFieldInit != bytecode::kNoOffset) {
                FramePlanStep step;
                step.kind = FramePlanStep::Kind::StaticInit;
                step.classIndex = static_cast<std::uint32_t>(i);
                step.entryOffset = classes_[i].firstStaticFieldInit;
                plan.steps.push_back(step);
            }
        }
    }
    if (plan.steps.empty()) {
        beginMain();
        return;
    }
    plans_.push_back(std::move(plan));
    advancePlan(plans_.size() - 1);
}

// ============================================================================
// Plan machinery (object construction + program startup)
// ============================================================================

void VirtualMachine::beginMain() {
    mainStarted_ = true;
    CallFrame mainFrame;
    mainFrame.functionName = "<main>";
    frames_.push_back(std::move(mainFrame));
    ip_ = 0;
}

void VirtualMachine::advancePlan(std::size_t planIndex) {
    if (planIndex >= plans_.size()) {
        runtimeFail("malformed bytecode: plan index out of range");
    }
    FramePlan& plan = plans_[planIndex];

    if (plan.nextStep < plan.steps.size()) {
        const FramePlanStep& step = plan.steps[plan.nextStep++];
        const ClassRuntime& cls = classes_[step.classIndex];

        if (frames_.size() >= kMaxCallDepth) {
            runtimeFail("call stack overflow while constructing '" +
                        cls.qualifiedName + "' (maximum call depth of " +
                        std::to_string(kMaxCallDepth) + " exceeded)");
        }

        CallFrame frame;
        frame.stackBase = stack_.size();
        frame.planMember = true;
        frame.planIndex = planIndex;
        frame.ownerClass = static_cast<int>(step.classIndex);

        switch (step.kind) {
            case FramePlanStep::Kind::FieldInit:
                frame.functionName = cls.qualifiedName + ".<fields>";
                frame.self = plan.instance;
                frame.unitPrefix = unitPrefixOf(cls.qualifiedName);
                break;
            case FramePlanStep::Kind::StaticInit:
                frame.functionName = cls.qualifiedName + ".<static>";
                frame.unitPrefix = unitPrefixOf(cls.qualifiedName);
                break;
            case FramePlanStep::Kind::PackageInit:
                frame.functionName = step.packageName + ".<init>";
                frame.unitPrefix = step.unitPrefix;
                break;
            case FramePlanStep::Kind::Constructor: {
                frame.functionName = cls.qualifiedName + ".<constructor>";
                frame.self = plan.instance;
                frame.unitPrefix = unitPrefixOf(cls.qualifiedName);
                const auto chosen =
                    cls.constructorsByArity.find(plan.constructorArgs.size());
                if (chosen == cls.constructorsByArity.end()) {
                    runtimeFail("malformed bytecode: class '" + cls.qualifiedName +
                                    "' has no constructor taking " +
                                    std::to_string(plan.constructorArgs.size()) +
                                    " argument(s)");
                }
                const bytecode::ClassMethodInfo* ctor = chosen->second;
                for (std::size_t i = 0; i < ctor->parameters.size(); ++i) {
                    frame.locals.emplace(ctor->parameters[i],
                                         std::move(plan.constructorArgs[i]));
                }
                break;
            }
        }

        frames_.push_back(std::move(frame));
        ip_ = step.entryOffset;
        return;
    }

    // The plan finished.
    const FramePlan::Kind kind = plan.kind;
    const std::size_t resumeIp = plan.resumeIp;
    std::shared_ptr<runtime::Instance> instance = plan.instance;
    if (planIndex != plans_.size() - 1) {
        runtimeFail("malformed bytecode: construction plans out of order");
    }
    plans_.pop_back();
    if (kind == FramePlan::Kind::Construction) {
        stack_.push_back(runtime::Value::makeObject(std::move(instance)));
        ip_ = resumeIp;  // the creator continues right after its NewObject
    } else if (kind == FramePlan::Kind::Startup) {
        beginMain();
    }
}

void VirtualMachine::executeNewObject(const bytecode::Chunk& chunk) {
    (void)chunk;
    const std::uint32_t classIndex = readU32(chunk);
    const std::uint32_t argc = readU32(chunk);
    if (classIndex >= classes_.size()) {
        runtimeFail("malformed bytecode: class index out of range");
    }
    const ClassRuntime& cls = classes_[classIndex];
    if (cls.isInterface) {
        runtimeFail("malformed bytecode: cannot instantiate interface '" +
                    cls.qualifiedName + "'");
    }
    if (cls.isAbstract) {
        runtimeFail("malformed bytecode: cannot instantiate abstract class '" +
                    cls.qualifiedName + "'");
    }

    // Constructor overload resolution by argument count (spec 30). A class
    // with NO constructor has an implicit zero-argument constructor (spec 31).
    // The compiler already validated statically; this is the runtime defense.
    if (!cls.constructorsByArity.empty() &&
        cls.constructorsByArity.find(argc) == cls.constructorsByArity.end()) {
        runtimeFail("no constructor of '" + cls.qualifiedName + "' takes " +
                    std::to_string(argc) + " argument(s)");
    }
    if (stack_.size() < argc) {
        runtimeFail("stack underflow in construction of '" + cls.qualifiedName + "'");
    }

    std::vector<runtime::Value> args(stack_.end() - argc, stack_.end());
    stack_.resize(stack_.size() - argc);

    auto instance = std::make_shared<runtime::Instance>();
    instance->classIndex = classIndex;
    instance->className = cls.name;
    instance->objectId = nextObjectId_++;
    runtime::heap::track(instance);  // cycle-collector registry (DECISIONS #118)

    // The creator resumes right after this instruction once the whole
    // construction plan (field initializers + constructor) has finished.
    const std::size_t resumeIp = ip_;

    FramePlan plan;
    plan.kind = FramePlan::Kind::Construction;
    plan.instance = instance;
    plan.constructorArgs = std::move(args);
    plan.resumeIp = resumeIp;
    // Field initializers run base class first, then the constructor
    // (constructors are not inherited in Runa).
    for (std::uint32_t chainIndex : classChain(classIndex)) {
        if (classes_[chainIndex].firstInstanceFieldInit != bytecode::kNoOffset) {
            FramePlanStep step;
            step.kind = FramePlanStep::Kind::FieldInit;
            step.classIndex = chainIndex;
            step.entryOffset = classes_[chainIndex].firstInstanceFieldInit;
            plan.steps.push_back(step);
        }
    }
    const auto chosenCtor = cls.constructorsByArity.find(argc);
    if (chosenCtor != cls.constructorsByArity.end()) {
        FramePlanStep step;
        step.kind = FramePlanStep::Kind::Constructor;
        step.classIndex = classIndex;
        step.entryOffset = chosenCtor->second->entryOffset;
        plan.steps.push_back(step);
    }

    plans_.push_back(std::move(plan));
    advancePlan(plans_.size() - 1);
    maybeCollectGarbage();  // runtime-controlled cycle collection (spec 58-60)
}

// ============================================================================
// Cycle collection (Runa 1.0 lifetime, DECISIONS #118)
// ============================================================================

// The collection trigger is the RUNTIME's decision, not the program's:
// after enough allocations have happened, the VM hands every root it owns
// to the heap and lets reachability reclaim unreachable cycles.
void VirtualMachine::maybeCollectGarbage() {
    if (++allocationsSinceCollect_ < kGcAllocationThreshold) return;
    allocationsSinceCollect_ = 0;
    collectGarbage();
}

void VirtualMachine::collectGarbage() {
    runtime::heap::collect([this](const runtime::heap::ValueTracer& trace) {
        // Root 1: global variables.
        for (const auto& entry : globals_.sortedEntries()) {
            trace(entry.second);
        }
        // Root 2: the operand stack (intermediate results included).
        for (const runtime::Value& value : stack_) {
            trace(value);
        }
        // Root 3: every call frame's locals + self.
        for (const CallFrame& frame : frames_) {
            for (const auto& entry : frame.locals) {
                trace(entry.second);
            }
            if (frame.self) {
                trace(runtime::Value::makeObject(frame.self));
            }
        }
        // Root 4: class static storage (shared by inherited access).
        for (const ClassRuntime& cls : classes_) {
            for (const auto& entry : cls.staticStorage) {
                trace(entry.second);
            }
        }
        // Root 5: active construction plans (instance under construction +
        // its queued constructor arguments).
        for (const FramePlan& plan : plans_) {
            if (plan.instance) {
                trace(runtime::Value::makeObject(plan.instance));
            }
            for (const runtime::Value& value : plan.constructorArgs) {
                trace(value);
            }
        }
    });
}

// ============================================================================
// Field / method / static access
// ============================================================================

void VirtualMachine::executeGetField(const bytecode::Chunk& chunk,
                                     const std::string& name) {
    (void)chunk;
    if (stack_.empty()) {
        runtimeFail("stack underflow in field access");
    }
    const runtime::Value receiver = stack_.back();
    stack_.pop_back();
    if (receiver.kind == runtime::Value::Kind::Null) {
        runtimeFail("null dereference: cannot access field '" + name + "' on null");
    }
    if (receiver.kind != runtime::Value::Kind::Object || !receiver.object) {
        runtimeFail("cannot access field '" + name + "' on a value of type '" +
                    receiver.typeName() + "'");
    }
    const runtime::Instance& instance = *receiver.object;
    const ClassRuntime& cls = classes_[instance.classIndex];

    const auto owner = cls.fieldOwners.find(name);
    if (owner == cls.fieldOwners.end()) {
        runtimeFail("class '" + cls.qualifiedName + "' has no field '" + name + "'");
    }
    if (owner->second.isStatic) {
        runtimeFail("'" + name + "' is a static field of class '" + cls.qualifiedName +
                    "' (access it as '" + cls.qualifiedName + "." + name + "')");
    }
    if (owner->second.isPrivate && static_cast<int>(owner->second.owner) != frames_.back().ownerClass) {
        runtimeFail("private field '" + name + "' of class '" +
                    classes_[owner->second.owner].qualifiedName +
                    "' is not accessible here");
    }
    const auto value = instance.fields.find(name);
    if (value == instance.fields.end()) {
        runtimeFail("malformed bytecode: field '" + name + "' of class '" +
                    cls.qualifiedName + "' was never initialized");
    }
    stack_.push_back(value->second);
}

void VirtualMachine::executeSetField(const bytecode::Chunk& chunk,
                                     const std::string& name) {
    (void)chunk;
    if (stack_.size() < 2) {
        runtimeFail("stack underflow in field assignment");
    }
    // Stack layout: [value, object] — the receiver was pushed last.
    const runtime::Value receiver = stack_.back();
    stack_.pop_back();
    const runtime::Value value = stack_.back();
    stack_.pop_back();
    if (receiver.kind == runtime::Value::Kind::Null) {
        runtimeFail("null dereference: cannot set field '" + name + "' on null");
    }
    if (receiver.kind != runtime::Value::Kind::Object || !receiver.object) {
        runtimeFail("cannot set field '" + name + "' on a value of type '" +
                    receiver.typeName() + "'");
    }
    runtime::Instance& instance = *receiver.object;
    const ClassRuntime& cls = classes_[instance.classIndex];

    const auto owner = cls.fieldOwners.find(name);
    if (owner == cls.fieldOwners.end()) {
        runtimeFail("class '" + cls.qualifiedName + "' has no field '" + name + "'");
    }
    if (owner->second.isStatic) {
        runtimeFail("'" + name + "' is a static field of class '" + cls.qualifiedName +
                    "' (assign it as '" + cls.qualifiedName + "." + name + "')");
    }
    if (owner->second.isPrivate && static_cast<int>(owner->second.owner) != frames_.back().ownerClass) {
        runtimeFail("private field '" + name + "' of class '" +
                    classes_[owner->second.owner].qualifiedName +
                    "' is not accessible here");
    }
    instance.fields[name] = value;
}

void VirtualMachine::executeInitField(const bytecode::Chunk& chunk,
                                      const std::string& name) {
    (void)chunk;
    if (stack_.empty()) {
        runtimeFail("stack underflow in field initialization");
    }
    runtime::Value value = stack_.back();
    stack_.pop_back();
    CallFrame& frame = frames_.back();
    if (!frame.self) {
        runtimeFail("malformed bytecode: field initialization outside of a "
                    "construction plan");
    }
    frame.self->fields[name] = std::move(value);
}

void VirtualMachine::pushMethodFrame(const ClassRuntime::MethodRT& method,
                                     const std::string& displayName,
                                     std::shared_ptr<runtime::Instance> self,
                                     int ownerClass,
                                     std::vector<runtime::Value> args) {
    if (args.size() != method.parameters.size()) {
        runtimeFail("malformed bytecode: '" + displayName + "' expects " +
                    std::to_string(method.parameters.size()) + " argument(s), got " +
                    std::to_string(args.size()));
    }
    if (frames_.size() >= kMaxCallDepth) {
        runtimeFail("call stack overflow while calling '" + displayName +
                    "' (maximum call depth of " + std::to_string(kMaxCallDepth) +
                    " exceeded)");
    }

    CallFrame frame;
    frame.returnIp = ip_;  // resume the caller right after the operands
    frame.stackBase = stack_.size();
    frame.functionName = displayName;
    frame.self = std::move(self);
    frame.ownerClass = ownerClass;
    // Package identification for the global-fallback rule (library methods).
    frame.unitPrefix =
        ownerClass >= 0
            ? unitPrefixOf(classes_[static_cast<std::size_t>(ownerClass)].qualifiedName)
            : std::string();
    for (std::size_t i = 0; i < args.size(); ++i) {
        frame.locals.emplace(method.parameters[i], std::move(args[i]));
    }
    frames_.push_back(std::move(frame));
    ip_ = method.entry;
}

void VirtualMachine::executeInvokeVirtual(const bytecode::Chunk& chunk,
                                          const std::string& name,
                                          std::uint32_t argc) {
    (void)chunk;
    if (stack_.size() < argc + 1) {
        runtimeFail("stack underflow in method call");
    }
    std::vector<runtime::Value> args(stack_.end() - argc, stack_.end());
    stack_.resize(stack_.size() - argc);
    const runtime::Value receiver = stack_.back();
    stack_.pop_back();

    if (receiver.kind == runtime::Value::Kind::Null) {
        runtimeFail("null dereference: cannot call method '" + name + "' on null");
    }
    if (receiver.kind != runtime::Value::Kind::Object || !receiver.object) {
        runtimeFail("cannot call method '" + name + "' on a value of type '" +
                    receiver.typeName() + "'");
    }
    const ClassRuntime& cls = classes_[receiver.object->classIndex];
    const auto found = cls.instanceMethods.find({name, argc});
    if (found == cls.instanceMethods.end()) {
        runtimeFail("class '" + cls.qualifiedName + "' has no method '" + name +
                    "' with " + std::to_string(argc) + " argument(s)");
    }
    const ClassRuntime::MethodRT& method = found->second;
    if (method.isPrivate && static_cast<int>(method.owner) != frames_.back().ownerClass) {
        runtimeFail("private method '" + name + "' of class '" +
                    classes_[method.owner].qualifiedName + "' is not accessible here");
    }
    pushMethodFrame(method, classes_[method.owner].qualifiedName + "." + name,
                    receiver.object, static_cast<int>(method.owner), std::move(args));
}

void VirtualMachine::executeInvokeStatic(const bytecode::Chunk& chunk,
                                         std::uint32_t classIndex,
                                         const std::string& name, std::uint32_t argc) {
    (void)chunk;
    if (classIndex >= classes_.size()) {
        runtimeFail("malformed bytecode: class index out of range");
    }
    if (stack_.size() < argc) {
        runtimeFail("stack underflow in static method call");
    }
    std::vector<runtime::Value> args(stack_.end() - argc, stack_.end());
    stack_.resize(stack_.size() - argc);

    const ClassRuntime& cls = classes_[classIndex];
    const auto found = cls.staticMethods.find({name, argc});
    if (found == cls.staticMethods.end()) {
        runtimeFail("class '" + cls.qualifiedName + "' has no static method '" + name +
                    "' with " + std::to_string(argc) + " argument(s)");
    }
    const ClassRuntime::MethodRT& method = found->second;
    if (method.isPrivate && static_cast<int>(method.owner) != frames_.back().ownerClass) {
        runtimeFail("private static method '" + name + "' of class '" +
                    classes_[method.owner].qualifiedName + "' is not accessible here");
    }
    pushMethodFrame(method, classes_[method.owner].qualifiedName + "." + name,
                    nullptr, static_cast<int>(method.owner), std::move(args));
}

void VirtualMachine::executeInvokeSuper(const bytecode::Chunk& chunk,
                                        const std::string& name, std::uint32_t argc) {
    (void)chunk;
    // Stack layout: [self, args...] — the compiler emits GetSelf first.
    if (stack_.size() < argc + 1) {
        runtimeFail("stack underflow in super call");
    }
    std::vector<runtime::Value> args(stack_.end() - argc, stack_.end());
    stack_.resize(stack_.size() - argc);
    const runtime::Value pushedSelf = stack_.back();
    stack_.pop_back();

    const CallFrame& current = frames_.back();
    if (!current.self || current.ownerClass < 0) {
        runtimeFail("malformed bytecode: 'super' outside of an instance method");
    }
    // Defense in depth: the pushed self must be the current frame's self.
    if (pushedSelf.kind != runtime::Value::Kind::Object ||
        pushedSelf.object.get() != current.self.get()) {
        runtimeFail("malformed bytecode: super call receiver mismatch");
    }
    const ClassRuntime& currentClass = classes_[current.ownerClass];
    if (currentClass.parent == bytecode::kNoParent) {
        runtimeFail("malformed bytecode: 'super' in a class without a parent");
    }
    const ClassRuntime& parentClass = classes_[currentClass.parent];
    const auto found = parentClass.instanceMethods.find({name, argc});
    if (found == parentClass.instanceMethods.end()) {
        runtimeFail("class '" + parentClass.qualifiedName + "' has no method '" + name +
                    "' with " + std::to_string(argc) + " argument(s) to call via 'super'");
    }
    const ClassRuntime::MethodRT& method = found->second;
    if (method.isPrivate) {
        runtimeFail("private method '" + name + "' of class '" +
                    classes_[method.owner].qualifiedName +
                    "' is not accessible via 'super'");
    }
    // The SAME self continues into the parent implementation; the owner class
    // becomes the declaring class so a further super.* resolves correctly.
    pushMethodFrame(method, classes_[method.owner].qualifiedName + "." + name,
                    current.self, static_cast<int>(method.owner), std::move(args));
}

void VirtualMachine::executeIsType(std::uint32_t classIndex) {
    if (classIndex >= classes_.size()) {
        runtimeFail("malformed bytecode: class index out of range");
    }
    if (stack_.empty()) {
        runtimeFail("stack underflow in type check");
    }
    const runtime::Value value = stack_.back();
    stack_.pop_back();

    bool result = false;
    if (value.kind == runtime::Value::Kind::Object && value.object) {
        // A class matches the runtime class itself or any ancestor; an
        // interface matches when it appears in the transitive interface set.
        for (std::uint32_t chainIndex : classChain(value.object->classIndex)) {
            if (chainIndex == classIndex) {
                result = true;
                break;
            }
            for (std::uint32_t interfaceIndex : classes_[chainIndex].interfaces) {
                if (interfaceIndex == classIndex) {
                    result = true;
                    break;
                }
            }
            if (result) break;
        }
    }
    stack_.push_back(runtime::Value::makeBoolean(result));
}

// ============================================================================
// Equality
// ============================================================================

// Structural equality across the value model: different kinds are simply
// unequal (5 == "5" is false, not an error — no implicit conversions).
// `none` equals only none; `null` equals only null. Objects compare
// structurally (same runtime class + equal fields) with a depth limit.
bool VirtualMachine::valuesEqualDeep(const runtime::Value& a,
                                     const runtime::Value& b, int depth) {
    // Equality is STRICT in Runa 1.0 (spec section 11 + DECISIONS #109):
    // Integer and Float are two distinct types and NEVER compare equal
    // across kinds — different kinds are simply unequal, like everywhere
    // else in the value model.
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case runtime::Value::Kind::None:    return true;
        case runtime::Value::Kind::String:  return a.str == b.str;
        case runtime::Value::Kind::Number:  return a.number == b.number;
        case runtime::Value::Kind::Boolean: return a.boolean == b.boolean;
        case runtime::Value::Kind::Null:    return true;
        case runtime::Value::Kind::Float:   return a.floating == b.floating;
        case runtime::Value::Kind::Pointer: return a.pointer == b.pointer;
        case runtime::Value::Kind::Object: {
            // Depth limit (spec sections 13/62 + DECISIONS #117): at most 64
            // field levels. Exceeding it is a clean RuntimeError — never a
            // crash, never a silent false.
            if (depth > kMaxEqualityDepth) {
                runtimeFail("object comparison exceeded the maximum depth of 64");
            }
            if (a.object.get() == b.object.get()) return true;  // same object
            if (!a.object || !b.object) return false;
            if (a.object->classIndex != b.object->classIndex) return false;
            // Cycle safety (spec section 13): a pair of objects that is
            // already being compared higher up the stack is assumed equal —
            // otherwise A->B->A structures would recurse forever.
            for (const auto& pair : equalityPairs_) {
                if (pair.first == a.object.get() && pair.second == b.object.get()) {
                    return true;
                }
            }
            equalityPairs_.push_back({a.object.get(), b.object.get()});
            bool result = true;
            for (const auto& field : a.object->fields) {
                const auto other = b.object->fields.find(field.first);
                if (other == b.object->fields.end()) {
                    result = false;
                    break;
                }
                if (!valuesEqualDeep(field.second, other->second, depth + 1)) {
                    result = false;
                    break;
                }
            }
            equalityPairs_.pop_back();
            return result;
        }
        case runtime::Value::Kind::Collection: {
            // Set equality: order-independent, duplicate-free on both sides.
            if (a.collection.get() == b.collection.get()) return true;  // aliased
            if (!a.collection || !b.collection) return false;
            return a.collection->equals(*b.collection);
        }
    }
    return false;
}

// ============================================================================
// Calls, arithmetic, comparisons, returns (pre-class opcodes)
// ============================================================================

void VirtualMachine::executeCall(const bytecode::Chunk& chunk) {
    const std::uint32_t nameIndex = readU32(chunk);
    const std::uint32_t argc = readU32(chunk);
    if (nameIndex >= chunk.names.size()) {
        runtimeFail("name index out of range");
    }
    const std::string callee = chunk.names[nameIndex];

    // ---- user functions (overload resolution: Name + Arity) ---------------
    const auto userOverloads = functions_.find(callee);
    if (userOverloads != functions_.end()) {
        const bytecode::FunctionInfo* matched = nullptr;
        for (const bytecode::FunctionInfo* overload : userOverloads->second) {
            if (overload->parameters.size() == argc) {
                matched = overload;
                break;
            }
        }
        if (matched == nullptr) {
            runtimeFail("no overload of '" + friendlyName(callee) + "' takes " +
                        std::to_string(argc) + " argument(s)");
        }
        const bytecode::FunctionInfo& fn = *matched;
        if (stack_.size() < argc) {
            runtimeFail("stack underflow in call to '" + friendlyName(callee) + "'");
        }
        if (frames_.size() >= kMaxCallDepth) {
            runtimeFail("call stack overflow while calling '" + friendlyName(callee) +
                        "' (maximum call depth of " +
                        std::to_string(kMaxCallDepth) + " exceeded)");
        }

        // The stack holds arguments in source order (arg0 deepest). Copy them
        // out in that order, then pop — they become THIS invocation's locals.
        std::vector<runtime::Value> args(stack_.end() - argc, stack_.end());
        stack_.resize(stack_.size() - argc);

        if (fn.entryOffset >= chunk.code.size()) {
            runtimeFail("malformed bytecode: invalid entry offset for function '" +
                        callee + "'");
        }

        // Push an independent frame for this invocation: parameters bind to
        // the evaluated argument values, locals start empty. Two calls to the
        // same function never share state.
    CallFrame frame;
    frame.returnIp = ip_;  // resume the caller right after the operands
    frame.stackBase = stack_.size();
    frame.functionName = fn.name;
    frame.unitPrefix = unitPrefixOf(fn.name);  // package-frame identification
        for (std::size_t i = 0; i < args.size(); ++i) {
            frame.locals.emplace(fn.parameters[i], std::move(args[i]));
        }
        frames_.push_back(std::move(frame));
        ip_ = fn.entryOffset;  // transfer control to the function code
        return;
    }

    // ---- built-ins ----------------------------------------------------------
    const stdlib::BuiltinSpec* spec = stdlib::findBuiltin(callee);
    if (spec == nullptr) {
        runtimeFail("unknown function '" + callee + "'");
    }
    if (argc < spec->minArgs || (spec->maxArgs != SIZE_MAX && argc > spec->maxArgs)) {
        runtimeFail("'" + callee + "' called with " + std::to_string(argc) +
                    " argument(s) (expected " +
                    (spec->minArgs == spec->maxArgs
                         ? std::to_string(spec->minArgs)
                         : std::to_string(spec->minArgs) + ".." +
                               (spec->maxArgs == SIZE_MAX ? "*" : std::to_string(spec->maxArgs))) +
                    ")");
    }
    if (stack_.size() < argc) {
        runtimeFail("stack underflow in call to '" + callee + "'");
    }

    std::vector<runtime::Value> args(stack_.end() - argc, stack_.end());
    stack_.resize(stack_.size() - argc);

    stdlib::BuiltinFn handler = stdlib::findBuiltinFn(callee);
    if (handler == nullptr) {
        runtimeFail("unknown function '" + callee + "'");
    }
    // Calls are expressions: every call leaves exactly one value on the stack.
    // Built-ins that produce nothing useful return the "none" value. A
    // built-in reports a failed operation (wrong argument kinds, an empty
    // collection where elements are required, ...) by throwing BuiltinError;
    // the VM converts it into the same clean runtime failure path it uses
    // internally — the process never crashes.
    runtime::Value result;
    try {
        result = handler(outputStream(), args);
    } catch (const stdlib::BuiltinError& problem) {
        runtimeFail(problem.what());
    }
    stack_.push_back(std::move(result));
}

void VirtualMachine::executeBuildCollection(const bytecode::Chunk& chunk) {
    const std::uint32_t count = readU32(chunk);
    if (stack_.size() < count) {
        runtimeFail("malformed bytecode: stack underflow in collection literal");
    }
    // Stack holds the elements in source order (last element on top).
    std::vector<runtime::Value> elements(stack_.end() - count, stack_.end());
    stack_.resize(stack_.size() - count);

    // ---- run-time validation: element kinds + the one-type rule -----------
    runtime::CollectionData::ElementType elementType =
        runtime::CollectionData::ElementType::Number;
    bool typeKnown = false;
    for (const runtime::Value& element : elements) {
        if (!runtime::CollectionData::elementAllowed(element)) {
            runtimeFail("collection elements must be number, float, string, or boolean "
                        "values, got " + element.typeName());
        }
        const auto elementKind = runtime::CollectionData::elementTypeOf(element);
        if (!typeKnown) {
            elementType = elementKind;
            typeKnown = true;
        } else if (elementKind != elementType) {
            runtimeFail("collection elements must share one type (found " +
                        std::string(runtime::CollectionData::elementTypeName(elementType)) +
                        " and " + element.typeName() + ")");
        }
    }
    // Canonical order + duplicate collapse in O(k log k): sort, then unique.
    std::sort(elements.begin(), elements.end(),
              [](const runtime::Value& a, const runtime::Value& b) {
                  return runtime::CollectionData::compareCanonical(a, b) < 0;
              });
    elements.erase(
        std::unique(elements.begin(), elements.end(),
                    [](const runtime::Value& a, const runtime::Value& b) {
                        return runtime::CollectionData::compareCanonical(a, b) == 0;
                    }),
        elements.end());
    auto data = std::make_shared<runtime::CollectionData>();
    data->elementType = elementType;
    data->elements = std::move(elements);
    stack_.push_back(runtime::Value::makeCollection(std::move(data)));
}

void VirtualMachine::executeArithmetic(OpCode op) {
    if (stack_.size() < 2) {
        runtimeFail("stack underflow in arithmetic operation");
    }
    const runtime::Value b = stack_.back();
    stack_.pop_back();
    const runtime::Value a = stack_.back();
    stack_.pop_back();

    // ---- String concatenation (DECISIONS #128) -----------------------------
    // '+' with a string on EITHER side glues strings: both strings join
    // directly; any other operand contributes its printr display form
    // (numbers decimal, floats like 1.5, booleans true/false, null as null,
    // objects as Car#3, collections as {1, 2}). This matches Java/C# /
    // JavaScript "" + x semantics and gives one single rule: if a string is
    // involved, '+' means "text", and the text form is EXACTLY what printr
    // would print. Non-string non-numeric operands keep the arithmetic
    // error ("+ only" — 'a' - 'b' stays invalid).
    if (op == OpCode::Add &&
        (a.kind == runtime::Value::Kind::String ||
         b.kind == runtime::Value::Kind::String)) {
        const std::string left = (a.kind == runtime::Value::Kind::String)
                                     ? a.str
                                     : a.displayString();
        const std::string right = (b.kind == runtime::Value::Kind::String)
                                      ? b.str
                                      : b.displayString();
        stack_.push_back(runtime::Value::makeString(left + right));
        return;
    }

    // ---- Float path (Runa 1.0, spec section 5 + DECISIONS #108) -----------
    // ANY float operand promotes the whole operation to double arithmetic
    // (Integer + Float -> Float). Results must stay finite: Runa never lets
    // arithmetic auto-produce Infinity or NaN — that is a clean RuntimeError.
    const bool aNumeric = a.kind == runtime::Value::Kind::Number ||
                          a.kind == runtime::Value::Kind::Float;
    const bool bNumeric = b.kind == runtime::Value::Kind::Number ||
                          b.kind == runtime::Value::Kind::Float;
    if (aNumeric && bNumeric &&
        (a.kind == runtime::Value::Kind::Float ||
         b.kind == runtime::Value::Kind::Float)) {
        const double lhs = (a.kind == runtime::Value::Kind::Float)
                               ? a.floating
                               : static_cast<double>(a.number);
        const double rhs = (b.kind == runtime::Value::Kind::Float)
                               ? b.floating
                               : static_cast<double>(b.number);
        double result = 0.0;
        switch (op) {
            case OpCode::Add: result = lhs + rhs; break;
            case OpCode::Sub: result = lhs - rhs; break;
            case OpCode::Mul: result = lhs * rhs; break;
            case OpCode::Div:
                if (rhs == 0.0) {
                    runtimeFail("float division by zero");
                }
                result = lhs / rhs;
                break;
            case OpCode::Mod:
                if (rhs == 0.0) {
                    runtimeFail("float modulo by zero");
                }
                result = std::fmod(lhs, rhs);
                break;
            default:
                runtimeFail("unknown arithmetic opcode");
        }
        if (!std::isfinite(result)) {
            runtimeFail(std::string("float overflow in '") + arithmeticSymbol(op) + "'");
        }
        stack_.push_back(runtime::Value::makeFloat(result));
        return;
    }

    if (a.kind != runtime::Value::Kind::Number ||
        b.kind != runtime::Value::Kind::Number) {
        runtimeFail(std::string("unsupported operand types for '") +
                    arithmeticSymbol(op) + "': " + a.typeName() + " and " +
                    b.typeName());
    }

    const std::int64_t lhs = a.number;
    const std::int64_t rhs = b.number;
    std::int64_t result = 0;
    switch (op) {
        case OpCode::Add:
            if (addOverflows(lhs, rhs)) {
                runtimeFail("integer overflow in '+'");
            }
            result = lhs + rhs;
            break;
        case OpCode::Sub:
            if (subOverflows(lhs, rhs)) {
                runtimeFail("integer overflow in '-'");
            }
            result = lhs - rhs;
            break;
        case OpCode::Mul:
            if (mulOverflows(lhs, rhs)) {
                runtimeFail("integer overflow in '*'");
            }
            result = lhs * rhs;
            break;
        case OpCode::Div:
            if (rhs == 0) {
                runtimeFail("division by zero");
            }
            // INT64_MIN / -1 would overflow (+2^63 does not fit).
            if (lhs == std::numeric_limits<std::int64_t>::min() && rhs == -1) {
                runtimeFail("integer overflow in '/'");
            }
            result = lhs / rhs;
            break;
        case OpCode::Mod:
            if (rhs == 0) {
                runtimeFail("modulo by zero");
            }
            // INT64_MIN % -1 is mathematically 0; C++ would trap on the
            // associated division, so it is defined here explicitly.
            result = (lhs == std::numeric_limits<std::int64_t>::min() && rhs == -1)
                         ? 0
                         : lhs % rhs;
            break;
        default:
            runtimeFail("unknown arithmetic opcode");
    }
    stack_.push_back(runtime::Value::makeNumber(result));
}

void VirtualMachine::executeComparison(OpCode op) {
    if (stack_.size() < 2) {
        runtimeFail("stack underflow in comparison");
    }
    const runtime::Value b = stack_.back();
    stack_.pop_back();
    const runtime::Value a = stack_.back();
    stack_.pop_back();

    // Equality works on any two values; ordering is numeric, or string vs
    // string in UTF-8 byte order (DECISIONS #129: UTF-8 byte comparison is
    // code-point comparison, so string ordering is total, deterministic and
    // matches Python/Java/Go byte-wise ordering — Persian identifiers sort
    // naturally too).
    if (op == OpCode::Eq || op == OpCode::Ne) {
        equalityPairs_.clear();  // fresh context per top-level comparison
        const bool equal = valuesEqualDeep(a, b, 0);
        stack_.push_back(runtime::Value::makeBoolean(op == OpCode::Eq ? equal : !equal));
        return;
    }

    if (a.kind == runtime::Value::Kind::String &&
        b.kind == runtime::Value::Kind::String) {
        // Unsigned byte comparison: memcmp never sees signed chars, so the
        // order is by UTF-8 bytes, which is by code point.
        const std::size_t common = std::min(a.str.size(), b.str.size());
        int order = 0;
        if (common > 0) {
            order = std::memcmp(a.str.data(), b.str.data(), common);
        }
        if (order == 0) {
            order = (a.str.size() < b.str.size()) ? -1
                    : (a.str.size() > b.str.size()) ? 1 : 0;
        }
        bool result = false;
        switch (op) {
            case OpCode::Gt: result = order > 0; break;
            case OpCode::Lt: result = order < 0; break;
            case OpCode::Ge: result = order >= 0; break;
            case OpCode::Le: result = order <= 0; break;
            default:
                runtimeFail("unknown comparison opcode");
        }
        stack_.push_back(runtime::Value::makeBoolean(result));
        return;
    }

    // Ordering works on numbers and floats (mixed allowed; compared as
    // doubles). Every other type combination is a clean error.
    const bool aNumeric = a.kind == runtime::Value::Kind::Number ||
                          a.kind == runtime::Value::Kind::Float;
    const bool bNumeric = b.kind == runtime::Value::Kind::Number ||
                          b.kind == runtime::Value::Kind::Float;
    if (!aNumeric || !bNumeric) {
        runtimeFail(std::string("unsupported operand types for '") +
                    comparisonSymbol(op) + "': " + a.typeName() + " and " +
                    b.typeName());
    }
    const double lhs = (a.kind == runtime::Value::Kind::Number)
                           ? static_cast<double>(a.number)
                           : a.floating;
    const double rhs = (b.kind == runtime::Value::Kind::Number)
                           ? static_cast<double>(b.number)
                           : b.floating;

    bool result = false;
    switch (op) {
        case OpCode::Gt: result = lhs > rhs; break;
        case OpCode::Lt: result = lhs < rhs; break;
        case OpCode::Ge: result = lhs >= rhs; break;
        case OpCode::Le: result = lhs <= rhs; break;
        default:
            runtimeFail("unknown comparison opcode");
    }
    stack_.push_back(runtime::Value::makeBoolean(result));
}

// Unary operators (DECISIONS #126): '-' on numbers/floats, 'not' on booleans.
// Negating the most negative int64 has no positive counterpart — a clean
// RuntimeError instead of silent wrap-around, matching the language's
// overflow policy for the binary arithmetic.
void VirtualMachine::executeUnary(OpCode op) {
    if (stack_.empty()) {
        runtimeFail("stack underflow in unary operation");
    }
    const runtime::Value value = stack_.back();
    stack_.pop_back();

    if (op == OpCode::Negate) {
        if (value.kind == runtime::Value::Kind::Number) {
            if (value.number == std::numeric_limits<std::int64_t>::min()) {
                runtimeFail("integer overflow in unary '-'");
            }
            stack_.push_back(runtime::Value::makeNumber(-value.number));
            return;
        }
        if (value.kind == runtime::Value::Kind::Float) {
            if (!std::isfinite(value.floating)) {
                runtimeFail("float overflow in unary '-'");
            }
            stack_.push_back(runtime::Value::makeFloat(-value.floating));
            return;
        }
        runtimeFail(std::string("unsupported operand type for unary '-': ") +
                    value.typeName());
    }

    if (op == OpCode::Not) {
        if (value.kind != runtime::Value::Kind::Boolean) {
            runtimeFail(std::string("logical 'not' requires a boolean operand, got ") +
                        value.typeName());
        }
        stack_.push_back(runtime::Value::makeBoolean(!value.boolean));
        return;
    }

    runtimeFail("unknown unary opcode");
}

// Logical conjunction/disjunction tail of 'and' / 'or' (DECISIONS #127).
// The short-circuit jump already handled the deciding side; this opcode
// runs only when BOTH operands must be examined (left side false for 'and',
// left side true for 'or' is impossible here). Both must be booleans —
// Runa has no truthiness.
void VirtualMachine::executeLogical(OpCode op) {
    if (stack_.size() < 2) {
        runtimeFail("stack underflow in logical operation");
    }
    const runtime::Value b = stack_.back();
    stack_.pop_back();
    const runtime::Value a = stack_.back();
    stack_.pop_back();

    if (a.kind != runtime::Value::Kind::Boolean ||
        b.kind != runtime::Value::Kind::Boolean) {
        runtimeFail(std::string("logical '") +
                    (op == OpCode::And ? "and" : "or") +
                    "' requires boolean operands, got " + a.typeName() +
                    " and " + b.typeName());
    }
    stack_.push_back(runtime::Value::makeBoolean(op == OpCode::And
                                                     ? (a.boolean && b.boolean)
                                                     : (a.boolean || b.boolean)));
}

// Common tail of ReturnValue and ReturnNone: hand `value` to the caller,
// unwind the current frame and resume it. The operand stack must be exactly
// at the frame's floor (only the return value on top) — anything else means
// malformed bytecode, which is reported instead of corrupting the caller.
// Frames that belong to a construction/startup plan advance the plan instead
// (field initializers and constructors do not push return values).
void VirtualMachine::finishReturn(runtime::Value value) {
    if (frames_.empty()) {
        runtimeFail("return outside of a function");
    }
    CallFrame& frame = frames_.back();
    if (frame.planMember) {
        if (stack_.size() != frame.stackBase) {
            runtimeFail("stack imbalance on return from '" + frame.functionName +
                        "' (malformed bytecode)");
        }
        const std::size_t planIndex = frame.planIndex;
        frames_.pop_back();
        if (planIndex != plans_.size() - 1) {
            runtimeFail("malformed bytecode: construction plans out of order");
        }
        advancePlan(planIndex);
        return;
    }
    if (frames_.size() < 2) {
        runtimeFail("return outside of a function");
    }
    if (stack_.size() != frame.stackBase) {
        runtimeFail("stack imbalance on return from '" + frame.functionName +
                    "' (malformed bytecode)");
    }
    stack_.push_back(std::move(value));
    ip_ = frame.returnIp;
    frames_.pop_back();
}

// ============================================================================
// The main loop
// ============================================================================

void VirtualMachine::run(const bytecode::Chunk& chunk) {
    stack_.clear();
    globals_ = runtime::Globals{};
    ip_ = 0;
    frames_.clear();
    plans_.clear();
    nextObjectId_ = 1;
    mainStarted_ = false;

    // Build the runtime function registry from the chunk's function table.
    // Overloads share a name and differ by parameter count; the call site
    // picks by argument count. Duplicate (name, arity) pairs are rejected —
    // this is defense in depth (the compiler already refuses them).
    functions_.clear();
    for (const bytecode::FunctionInfo& fn : chunk.functions) {
        for (const bytecode::FunctionInfo* existing : functions_[fn.name]) {
            if (existing->parameters.size() == fn.parameters.size()) {
                runtimeFail("malformed bytecode: duplicate function '" + fn.name +
                                "' (" + std::to_string(fn.parameters.size()) +
                                " parameter(s))");
            }
        }
        functions_[fn.name].push_back(&fn);
    }

    buildClassRegistry(chunk);

    // Static fields initialize once, BEFORE <main> runs (in class
    // registration order). If there is nothing to initialize, main starts
    // immediately.
    runStaticInitializers(chunk);

    while (true) {
        if (ip_ >= chunk.code.size()) {
            runtimeFail("bytecode ended without Halt");
        }

        if (options_.trace) {
            std::ostringstream prefix;
            prefix << '[' << std::hex << std::setw(4) << std::setfill('0') << ip_
                   << std::dec << ' ' << frames_.back().functionName << "] ";
            std::cout << prefix.str();
            bytecode::formatInstructionAt(chunk, ip_, std::cout);
            std::cout << '\n';
        }

        bytecode::OpCode op = static_cast<bytecode::OpCode>(chunk.code[ip_++]);
        switch (op) {
            case OpCode::LoadConst: {
                std::uint32_t index = readU32(chunk);
                if (index >= chunk.constants.size()) {
                    runtimeFail("constant index out of range");
                }
                const bytecode::Constant& constant = chunk.constants[index];
                switch (constant.kind) {
                    case bytecode::Constant::Kind::String:
                        stack_.push_back(runtime::Value::makeString(constant.str));
                        break;
                    case bytecode::Constant::Kind::Number:
                        stack_.push_back(runtime::Value::makeNumber(constant.number));
                        break;
                    case bytecode::Constant::Kind::Boolean:
                        stack_.push_back(runtime::Value::makeBoolean(constant.boolean));
                        break;
                    case bytecode::Constant::Kind::Null:
                        stack_.push_back(runtime::Value::makeNull());
                        break;
                    case bytecode::Constant::Kind::Float:
                        stack_.push_back(runtime::Value::makeFloat(constant.floating));
                        break;
                }
                break;
            }
            case OpCode::LoadGlobal: {
                std::uint32_t index = readU32(chunk);
                if (index >= chunk.names.size()) {
                    runtimeFail("name index out of range");
                }
                const runtime::Value* value = globals_.find(chunk.names[index]);
                if (value == nullptr) {
                    runtimeFail("undefined variable '" + friendlyName(chunk.names[index]) +
                                "'");
                }
                stack_.push_back(*value);
                break;
            }
            case OpCode::StoreGlobal: {
                std::uint32_t index = readU32(chunk);
                if (index >= chunk.names.size()) {
                    runtimeFail("name index out of range");
                }
                if (stack_.empty()) {
                    runtimeFail("stack underflow while storing global '" +
                                chunk.names[index] + "'");
                }
                runtime::Value value = stack_.back();
                stack_.pop_back();
                globals_.define(chunk.names[index], value);
                break;
            }
            case OpCode::LoadVar: {
                // Function-scope read: the invocation's locals first, then
                // globals. Locals vanish with the frame, globals persist.
                // v0.9: a package frame also tries its unit-prefixed global,
                // so library functions can read their package's top-level
                // variables (which are stored under hidden names).
                std::uint32_t index = readU32(chunk);
                if (index >= chunk.names.size()) {
                    runtimeFail("name index out of range");
                }
                const std::string& name = chunk.names[index];
                const CallFrame& frame = frames_.back();
                const auto local = frame.locals.find(name);
                if (local != frame.locals.end()) {
                    stack_.push_back(local->second);
                    break;
                }
                const runtime::Value* value = globals_.find(name);
                if (value == nullptr && !frame.unitPrefix.empty()) {
                    value = globals_.find(frame.unitPrefix + name);
                }
                if (value == nullptr) {
                    runtimeFail("undefined variable '" + friendlyName(name) + "'");
                }
                stack_.push_back(*value);
                break;
            }
            case OpCode::StoreLocal: {
                // Function-scope assignment: always the current invocation's
                // locals — a global is never created or mutated from inside a
                // function (a same-named global is shadowed, not overwritten).
                std::uint32_t index = readU32(chunk);
                if (index >= chunk.names.size()) {
                    runtimeFail("name index out of range");
                }
                if (stack_.empty()) {
                    runtimeFail("stack underflow while storing local '" +
                                chunk.names[index] + "'");
                }
                runtime::Value value = stack_.back();
                stack_.pop_back();
                frames_.back().locals[chunk.names[index]] = std::move(value);
                break;
            }
            case OpCode::Call:
                executeCall(chunk);
                break;
            case OpCode::ReturnValue: {
                // Return a value to the caller and stop this frame.
                if (stack_.empty()) {
                    runtimeFail("stack underflow in return");
                }
                runtime::Value value = stack_.back();
                stack_.pop_back();
                finishReturn(std::move(value));
                break;
            }
            case OpCode::ReturnNone: {
                // Bare `return` / end of body: return the distinct "none"
                // value (never 0, false or ""). In plan frames the value is
                // irrelevant — the plan advances instead.
                finishReturn(runtime::Value::makeNone());
                break;
            }
            case OpCode::Pop:
                // Discard a value (bare call statements leave their result
                // here so the stack stays balanced).
                if (stack_.empty()) {
                    runtimeFail("stack underflow in Pop");
                }
                stack_.pop_back();
                break;
            case OpCode::NewObject:
                executeNewObject(chunk);
                break;
            case OpCode::GetField: {
                std::uint32_t index = readU32(chunk);
                if (index >= chunk.names.size()) {
                    runtimeFail("name index out of range");
                }
                executeGetField(chunk, chunk.names[index]);
                break;
            }
            case OpCode::SetField: {
                std::uint32_t index = readU32(chunk);
                if (index >= chunk.names.size()) {
                    runtimeFail("name index out of range");
                }
                executeSetField(chunk, chunk.names[index]);
                break;
            }
            case OpCode::InitField: {
                std::uint32_t index = readU32(chunk);
                if (index >= chunk.names.size()) {
                    runtimeFail("name index out of range");
                }
                executeInitField(chunk, chunk.names[index]);
                break;
            }
            case OpCode::GetSelf: {
                const CallFrame& frame = frames_.back();
                if (!frame.self) {
                    runtimeFail("malformed bytecode: 'self' is not available in "
                                "this frame");
                }
                stack_.push_back(runtime::Value::makeObject(frame.self));
                break;
            }
            case OpCode::InvokeVirtual: {
                std::uint32_t nameIndex = readU32(chunk);
                std::uint32_t argc = readU32(chunk);
                if (nameIndex >= chunk.names.size()) {
                    runtimeFail("name index out of range");
                }
                executeInvokeVirtual(chunk, chunk.names[nameIndex], argc);
                break;
            }
            case OpCode::InvokeStatic: {
                std::uint32_t classIndex = readU32(chunk);
                std::uint32_t nameIndex = readU32(chunk);
                std::uint32_t argc = readU32(chunk);
                if (nameIndex >= chunk.names.size()) {
                    runtimeFail("name index out of range");
                }
                executeInvokeStatic(chunk, classIndex, chunk.names[nameIndex], argc);
                break;
            }
            case OpCode::InvokeSuper: {
                std::uint32_t nameIndex = readU32(chunk);
                std::uint32_t argc = readU32(chunk);
                if (nameIndex >= chunk.names.size()) {
                    runtimeFail("name index out of range");
                }
                executeInvokeSuper(chunk, chunk.names[nameIndex], argc);
                break;
            }
            case OpCode::GetStatic: {
                std::uint32_t classIndex = readU32(chunk);
                std::uint32_t nameIndex = readU32(chunk);
                if (nameIndex >= chunk.names.size()) {
                    runtimeFail("name index out of range");
                }
                if (classIndex >= classes_.size()) {
                    runtimeFail("malformed bytecode: class index out of range");
                }
                const ClassRuntime& cls = classes_[classIndex];
                const auto owner = cls.fieldOwners.find(chunk.names[nameIndex]);
                if (owner == cls.fieldOwners.end() || !owner->second.isStatic) {
                    runtimeFail("class '" + cls.qualifiedName + "' has no static field '" +
                                chunk.names[nameIndex] + "'");
                }
                if (owner->second.isPrivate &&
                    static_cast<int>(owner->second.owner) != frames_.back().ownerClass) {
                    runtimeFail("private static field '" + chunk.names[nameIndex] +
                                "' of class '" +
                                classes_[owner->second.owner].qualifiedName +
                                "' is not accessible here");
                }
                // Static storage lives in the DECLARING class: an inherited
                // static read through a subclass reads the ancestor's value.
                const auto value =
                    classes_[owner->second.owner].staticStorage.find(chunk.names[nameIndex]);
                if (value == classes_[owner->second.owner].staticStorage.end()) {
                    runtimeFail("malformed bytecode: static field '" +
                                chunk.names[nameIndex] + "' of class '" +
                                cls.qualifiedName + "' was never initialized");
                }
                stack_.push_back(value->second);
                break;
            }
            case OpCode::StoreStatic: {
                std::uint32_t classIndex = readU32(chunk);
                std::uint32_t nameIndex = readU32(chunk);
                if (nameIndex >= chunk.names.size()) {
                    runtimeFail("name index out of range");
                }
                if (classIndex >= classes_.size()) {
                    runtimeFail("malformed bytecode: class index out of range");
                }
                if (stack_.empty()) {
                    runtimeFail("stack underflow while storing static field '" +
                                chunk.names[nameIndex] + "'");
                }
                runtime::Value value = stack_.back();
                stack_.pop_back();
                const ClassRuntime& cls = classes_[classIndex];
                const auto owner = cls.fieldOwners.find(chunk.names[nameIndex]);
                if (owner == cls.fieldOwners.end() || !owner->second.isStatic) {
                    runtimeFail("malformed bytecode: class '" + cls.qualifiedName +
                                "' has no static field '" + chunk.names[nameIndex] + "'");
                }
                classes_[owner->second.owner].staticStorage[chunk.names[nameIndex]] =
                    std::move(value);
                break;
            }
            case OpCode::IsType: {
                std::uint32_t classIndex = readU32(chunk);
                executeIsType(classIndex);
                break;
            }
            case OpCode::EqIdent: {
                // Identity: two references to the SAME object/collection —
                // or value equality for non-reference pairs.
                if (stack_.size() < 2) {
                    runtimeFail("stack underflow in identity comparison");
                }
                const runtime::Value b = stack_.back();
                stack_.pop_back();
                const runtime::Value a = stack_.back();
                stack_.pop_back();
                bool result = false;
                if (a.kind == runtime::Value::Kind::Object &&
                    b.kind == runtime::Value::Kind::Object) {
                    result = a.object.get() == b.object.get();
                } else if (a.kind == runtime::Value::Kind::Collection &&
                           b.kind == runtime::Value::Kind::Collection) {
                    // Two separately built collections with identical members
                    // are equal ('==') but NOT identical ('===') — only an
                    // aliased reference (b = a) shares the identity.
                    result = a.collection.get() == b.collection.get();
                } else if (a.kind == b.kind) {
                    equalityPairs_.clear();  // fresh context (same as '==')
                    result = valuesEqualDeep(a, b, 0);
                }
                stack_.push_back(runtime::Value::makeBoolean(result));
                break;
            }
            case OpCode::CallNative:
                executeCallNative(chunk);
                break;
            case OpCode::BuildCollection:
                executeBuildCollection(chunk);
                break;
            case OpCode::Add:
            case OpCode::Sub:
            case OpCode::Mul:
            case OpCode::Div:
            case OpCode::Mod:
                executeArithmetic(op);
                break;
            case OpCode::Eq:
            case OpCode::Ne:
            case OpCode::Gt:
            case OpCode::Lt:
            case OpCode::Ge:
            case OpCode::Le:
                executeComparison(op);
                break;
            case OpCode::Negate:
            case OpCode::Not:
                executeUnary(op);
                break;
            case OpCode::And:
            case OpCode::Or:
                executeLogical(op);
                break;
            case OpCode::JumpIfFalse: {
                const std::uint32_t target = readU32(chunk);
                if (target > chunk.code.size()) {
                    runtimeFail("jump target out of range");
                }
                if (stack_.empty()) {
                    runtimeFail("stack underflow in conditional jump");
                }
                const runtime::Value condition = stack_.back();
                stack_.pop_back();
                if (condition.kind != runtime::Value::Kind::Boolean) {
                    // Neutral wording: JumpIfFalse drives 'if'/'elif' chains,
                    // 'loop' conditions and the for-loop bound check alike.
                    runtimeFail("condition must be a boolean, got " +
                                condition.typeName());
                }
                if (!condition.boolean) {
                    ip_ = target;  // skip the if block entirely
                }
                break;
            }
            case OpCode::JumpIfFalseKeep:
            case OpCode::JumpIfTrueKeep: {
                // Short-circuit 'and' / 'or' jumps (DECISIONS #127): unlike
                // JumpIfFalse, the value STAYS on the stack when the jump is
                // taken — it is already the expression's result. The fall-
                // through path proceeds to evaluate the right operand.
                const std::uint32_t target = readU32(chunk);
                if (target > chunk.code.size()) {
                    runtimeFail("jump target out of range");
                }
                if (stack_.empty()) {
                    runtimeFail("stack underflow in conditional jump");
                }
                const runtime::Value condition = stack_.back();
                if (condition.kind != runtime::Value::Kind::Boolean) {
                    // The right operand has not been evaluated yet, so the
                    // message names the operator and the offending LEFT
                    // operand only ('and' uses JumpIfFalseKeep, 'or' uses
                    // JumpIfTrueKeep).
                    runtimeFail(std::string("logical '") +
                                (op == OpCode::JumpIfFalseKeep ? "and" : "or") +
                                "' requires a boolean operand, got " +
                                condition.typeName());
                }
                if (op == OpCode::JumpIfFalseKeep && !condition.boolean) {
                    ip_ = target;
                }
                if (op == OpCode::JumpIfTrueKeep && condition.boolean) {
                    ip_ = target;
                }
                break;
            }
            case OpCode::Jump: {
                // Unconditional jump: the if/elif/else chain uses it after a
                // taken branch so the remaining branches are skipped. No
                // stack effect — it just moves the instruction pointer.
                const std::uint32_t target = readU32(chunk);
                if (target > chunk.code.size()) {
                    runtimeFail("jump target out of range");
                }
                ip_ = target;
                break;
            }
            case OpCode::Halt:
                // Program finished. One deterministic stats line when the
                // lifetime test suite asks for it (RUNA_GC_STATS=1).
                runtime::heap::reportIfEnabled();
                return;
            default:
                runtimeFail("unknown opcode " + std::to_string(static_cast<int>(op)));
        }
    }
}

// ============================================================================
// Native bridge (v0.9)
// ============================================================================

native::NativeLibrary VirtualMachine::ensureNativeLibrary(
    const bytecode::NativeFunctionInfo& fn) {
    const auto cached = loadedNatives_.find(fn.packageDir);
    if (cached != loadedNatives_.end() && cached->second.valid()) {
        return cached->second;
    }
    if (options_.nativeRoot.empty()) {
        runtimeFail("native root is not configured (native package '" +
                    fn.namespaceName + "' cannot be resolved)");
    }
    // Deterministic target selection: <os>-<arch> of the running machine.
    const std::string platformKey = platform::nativeTargetKey();
    const std::string* targetPath = nullptr;
    for (const auto& target : fn.targets) {
        if (target.first == platformKey) {
            targetPath = &target.second;
            break;
        }
    }
    if (targetPath == nullptr) {
        std::string available;
        for (const auto& target : fn.targets) {
            available += (available.empty() ? "" : ", ") + target.first;
        }
        if (available.empty()) available = "(none declared)";
        runtimeFail("native target not found for platform '" + platformKey +
                    "' in package '" + fn.namespaceName + "' (available: " +
                    available + ")");
    }
    const std::string fullPath =
        options_.nativeRoot + "/" + fn.packageDir + "/" + *targetPath;
    try {
        native::NativeLibrary library = native::NativeLoader::load(fullPath);
        loadedNatives_.emplace(fn.packageDir, library);
        return library;
    } catch (const native::NativeLoadError& error) {
        runtimeFail("native library load failed for package '" + fn.namespaceName +
                    "': " + error.message + " (" + fullPath + ")");
    }
}

void VirtualMachine::executeCallNative(const bytecode::Chunk& chunk) {
    const std::uint32_t nativeIndex = readU32(chunk);
    const std::uint32_t argc = readU32(chunk);
    if (nativeIndex >= chunk.nativeFunctions.size()) {
        runtimeFail("malformed bytecode: native function index out of range");
    }
    const bytecode::NativeFunctionInfo& fn = chunk.nativeFunctions[nativeIndex];
    if (argc != fn.parameterTypes.size()) {
        runtimeFail("malformed bytecode: native function '" + fn.namespaceName + "." +
                    fn.name + "' expects " +
                    std::to_string(fn.parameterTypes.size()) +
                    " argument(s), got " + std::to_string(argc));
    }
    if (stack_.size() < argc) {
        runtimeFail("stack underflow in native call to '" + fn.namespaceName + "." +
                    fn.name + "'");
    }
    std::vector<runtime::Value> args(stack_.end() - argc, stack_.end());
    stack_.resize(stack_.size() - argc);

    // The bytecode type bytes match the bridge's canonical FfiType values.
    native::NativeSignature signature;
    signature.returnType = static_cast<native::FfiType>(fn.returnType);
    for (std::uint8_t type : fn.parameterTypes) {
        if (!native::isValidFfiType(type)) {
            runtimeFail("malformed bytecode: invalid native parameter type");
        }
        signature.parameterTypes.push_back(static_cast<native::FfiType>(type));
    }

    const native::NativeLibrary library = ensureNativeLibrary(fn);
    const std::uintptr_t symbol = native::NativeLoader::lookup(library, fn.symbol);
    if (symbol == 0) {
        runtimeFail("native symbol not found: '" + fn.symbol +
                    "' in package '" + fn.namespaceName + "'");
    }

    const std::string displayName = fn.namespaceName + "." + fn.name;
    try {
        stack_.push_back(
            native::callNativeFunction(symbol, signature, args, displayName));
    } catch (const native::NativeCallError& error) {
        runtimeFail(error.message);
    }
}

}  // namespace runa::vm
