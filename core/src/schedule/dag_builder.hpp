#pragma once
// core/src/schedule/dag_builder.hpp
//
// DagBuilder — internal helper that turns a per-phase set of SystemDesc
// records into a topologically sorted CompiledPhase.
//
// Authority: specs/core/SPEC.md §6.4 (DAG from declared access sets).
//            plan #584 — Schedule DAG builder from access sets.
//
// This header is internal to the schedule/ module; it is NOT installed
// and must NOT be included by headers under core/include/.
//
// Cycle classification:
//   - A cycle where ANY edge is an access-set intersection edge
//     (writes∩reads or writes∩writes) → core::Error::ScheduleAccessConflict.
//   - A cycle where ALL edges are explicit after/before edges
//     → core::Error::SystemScheduleCycle.
//   When an explicit declaration coincides with an access-set edge, the
//   EdgeKind is preserved as AccessSet (not upgraded), so the access-set
//   root cause dominates classification.
//
// -fno-exceptions clean.

#include <span>
#include <string_view>
#include <vector>

#include "glibre/core/schedule.hpp"
#include "glibre/error.hpp"

namespace glibre::core::detail {

// ---------------------------------------------------------------------------
// SystemNode — per-system node held inside DagBuilder during a build pass.
// ---------------------------------------------------------------------------

struct SystemNode {
    SystemId id{};
    std::string_view name;                 // Non-owning; points into Schedule::Impl storage.
    std::vector<TypeId> reads;             // Copied from AccessSet.
    std::vector<TypeId> writes;            // Copied from AccessSet.
    std::vector<std::string_view> after;   // Non-owning name spans.
    std::vector<std::string_view> before;  // Non-owning name spans.
    SystemFn body{nullptr};
};

// ---------------------------------------------------------------------------
// EdgeKind — tracks the origin of each directed graph edge.
//
// Used during cycle detection to classify whether the cycle arose from
// access-set intersection logic (ScheduleAccessConflict) or explicit
// ordering edges (SystemScheduleCycle).
// ---------------------------------------------------------------------------

enum class EdgeKind : std::uint8_t {
    AccessSet,  // Edge arose from writes∩reads or writes∩writes intersection.
    Explicit,   // Edge arose from after/before declarations.
};

// ---------------------------------------------------------------------------
// build_phase — compile one Phase into a topological order.
//
// Accepts a span of SystemNode records for one phase.  Returns a vector of
// SystemId in resolved execution order (Kahn's algorithm, lexicographic
// tiebreaker on system name).
//
// Cycle classification follows the rule in the file header:
//   - Pure access-set cycle → ScheduleAccessConflict.
//   - Cycle with at least one explicit edge involved → SystemScheduleCycle.
//
// On success returns a vector of SystemId in resolved order.
// On cycle returns std::unexpected<glibre::Error>.
//
// Uses std::vector (stdlib, heap) for the build scratch; does NOT use the
// PMR allocator (build scratch is transient and freed after each compile).
// ---------------------------------------------------------------------------

[[nodiscard]] Result<std::vector<SystemId>> build_phase(std::span<const SystemNode> nodes) noexcept;

}  // namespace glibre::core::detail
