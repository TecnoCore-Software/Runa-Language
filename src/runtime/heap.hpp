// Runa — Object heap with reachability-based cycle reclamation (Runa 1.0,
// spec sections 58-60 + DECISIONS #118).
//
// Lifetime strategy:
//   1. Reference counting (shared_ptr) gives immediate, deterministic
//      reclamation for the common case: an object dies the moment its last
//      reachable reference disappears.
//   2. A registry of all live instances plus a runtime-controlled collector
//      handles what refcounting cannot: cyclic object graphs (A -> B -> A)
//      that become unreachable from the roots. Collection runs when the VM
//      decides (allocation threshold) — never user-directed.
//
// Roots are provided by the VM per collection: globals, the operand stack,
// every call frame's locals + self, class static storage and active
// construction plans. An instance reachable from no root is reclaimed by
// clearing its fields (which drops the cycle's internal references) and
// letting the shared_ptr reference counts fall to zero.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "runtime/runtime.hpp"

namespace runa::runtime::heap {

// Receives every live root Value (by copy) during a collection pass.
using ValueTracer = std::function<void(const Value&)>;
// Installed by the VM for one collection; must invoke the tracer on ALL
// currently reachable root Values.
using RootProvider = std::function<void(const ValueTracer&)>;

// Registers a newly allocated instance with the heap. Cheap: one weak
// reference insert.
void track(const std::shared_ptr<Instance>& instance);

// Runs one collection pass and returns the number of instances reclaimed.
std::size_t collect(const RootProvider& visitRoots);

// Number of live (registered) instances right now.
std::size_t liveInstances();

// Total collections performed so far in this process.
std::uint64_t collectionCount();

// When RUNA_GC_STATS=1, prints one deterministic stderr line:
//   [runa gc] collections=<N> live=<M>
// Used by the lifetime test suite; silent otherwise.
void reportIfEnabled();

}  // namespace runa::runtime::heap
