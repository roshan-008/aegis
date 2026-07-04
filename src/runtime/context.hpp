#pragma once
// ExecutionContext — the engine's shared substrate, passed by reference instead
// of reached for through globals/singletons. Every subsystem that needs memory,
// scheduling, kernel lookup, or telemetry receives one `ExecutionContext&` and
// takes its dependencies from there. This is the "engine, not a pile of
// subsystems" seam: `execute(graph)` is the single entry point that runs a DAG
// of kernels over memory-resident data and records how long it took.
//
// The pointers are NON-OWNING. The context is a wiring harness, not an owner —
// the caller constructs the arena/scheduler/telemetry with whatever lifetime it
// wants and hands their addresses in. Any of them may be null; `execute` only
// touches `scheduler` (required) and `telemetry` (optional).
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>

#include "../kernels/registry.hpp"
#include "../mem/arena.hpp"
#include "observability.hpp"
#include "scheduler.hpp"
#include "task_graph.hpp"

namespace aegis::runtime {

struct ExecutionContext {
    mem::Arena* arena = nullptr;       // per-batch scratch; reset by the caller
    Scheduler* scheduler = nullptr;    // where graphs actually run
    RuntimeStats* telemetry = nullptr; // optional counters + latency histogram

    // Run one kernel DAG to completion on the context's scheduler. Returns the
    // wall-clock nanoseconds and, when telemetry is wired, records the run as a
    // latency observation plus an accepted-work count. This is the method that
    // makes "what does Aegis execute? a DAG of kernels over memory-resident
    // data" a literal API call rather than a slogan.
    uint64_t execute(TaskGraph& graph) {
        if (!scheduler) throw std::runtime_error("ExecutionContext: no scheduler");
        const auto begin = std::chrono::steady_clock::now();
        scheduler->submit(graph);
        const uint64_t ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - begin)
                .count());
        if (telemetry) {
            telemetry->wire_to_feature.observe(ns);
            telemetry->accepted.fetch_add(1, std::memory_order_relaxed);
        }
        return ns;
    }

    // Kernel lookup by name flows through the shared registry, so a subsystem
    // never has to know which concrete kernels exist — it asks the context.
    std::optional<kernels::RegistryEntry> kernel(std::string_view name) const {
        return kernels::find(name);
    }
};

}  // namespace aegis::runtime
