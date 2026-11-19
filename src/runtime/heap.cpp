#include "runtime/heap.hpp"

#include <cstdlib>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace runa::runtime::heap {

namespace {

// Instance registry: raw pointer key -> weak handle. Weak handles mean the
// registry never keeps an instance alive and needs no destructor hook:
// entries whose weak_ptr expired are purged lazily during collection.
using Registry = std::unordered_map<Instance*, std::weak_ptr<Instance>>;

Registry& registry() {
    static Registry instance;
    return instance;
}

std::uint64_t& collectionCounter() {
    static std::uint64_t count = 0;
    return count;
}

}  // namespace

void track(const std::shared_ptr<Instance>& instance) {
    if (instance) {
        registry()[instance.get()] = instance;  // weak copy — no ownership
    }
}

std::size_t collect(const RootProvider& visitRoots) {
    Registry& reg = registry();

    // ---- 1. Snapshot the live instances (purge expired entries) ----------
    std::vector<std::shared_ptr<Instance>> live;
    live.reserve(reg.size());
    for (auto it = reg.begin(); it != reg.end();) {
        if (std::shared_ptr<Instance> locked = it->second.lock()) {
            live.push_back(std::move(locked));
            ++it;
        } else {
            it = reg.erase(it);  // already reclaimed by refcounting
        }
    }

    // ---- 2. Mark: trace everything reachable from the roots --------------
    // The worklist holds Value COPIES (shared_ptr bumps, string copies) so
    // the tracer never stores dangling references into temporaries.
    std::unordered_set<Instance*> marked;
    marked.reserve(live.size() * 2 + 16);
    std::vector<Value> worklist;
    const ValueTracer trace = [&worklist](const Value& value) {
        worklist.push_back(value);
    };
    if (visitRoots) {
        visitRoots(trace);
    }
    while (!worklist.empty()) {
        const Value value = std::move(worklist.back());
        worklist.pop_back();
        if (value.kind == Value::Kind::Object && value.object) {
            Instance* raw = value.object.get();
            if (marked.insert(raw).second) {  // first visit: trace its fields
                for (const auto& field : value.object->fields) {
                    worklist.push_back(field.second);
                }
            }
        }
        // Collections hold only scalar elements (no objects, no nesting) and
        // every other kind is scalar — nothing further to trace.
    }

    // ---- 3. Sweep: break cycles inside every unreachable instance --------
    // Clearing the fields drops the references that hold the cycle together;
    // the shared_ptr counts then fall to zero once this function's local
    // `live` snapshot goes out of scope.
    std::size_t reclaimed = 0;
    for (const std::shared_ptr<Instance>& instance : live) {
        if (marked.count(instance.get()) == 0) {
            instance->fields.clear();
            ++reclaimed;
        }
    }

    ++collectionCounter();
    return reclaimed;
}

std::size_t liveInstances() {
    std::size_t count = 0;
    for (const auto& entry : registry()) {
        if (!entry.second.expired()) ++count;
    }
    return count;
}

std::uint64_t collectionCount() {
    return collectionCounter();
}

void reportIfEnabled() {
    const char* enabled = std::getenv("RUNA_GC_STATS");
    if (enabled == nullptr || enabled[0] != '1') return;
    std::cerr << "[runa gc] collections=" << collectionCount()
              << " live=" << liveInstances() << std::endl;
}

}  // namespace runa::runtime::heap
