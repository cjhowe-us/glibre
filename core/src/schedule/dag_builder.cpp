// core/src/schedule/dag_builder.cpp
//
// Implementation of build_phase(): topological sort (Kahn's algorithm) over
// the per-phase system dependency graph derived from access sets and explicit
// after/before ordering edges.
//
// Authority: specs/core/SPEC.md §6.4.
//            plan #584 — Schedule DAG builder from access sets.
//
// Cycle classification (per plan #584 Scope):
//   Kahn's algorithm removes nodes with zero in-degree.  Nodes that remain
//   after the loop are part of one or more cycles.  To distinguish error
//   kinds the build records, for each edge, whether it arose from access-set
//   intersection ("access") or from an explicit after/before declaration
//   ("explicit").  If ALL edges incident to the remaining cycle nodes are
//   access-set edges → ScheduleAccessConflict.  If any explicit edge is
//   involved → SystemScheduleCycle.
//
// Deterministic tiebreaker (SPEC §4.4 invariant 5):
//   The Kahn ready-queue is a sorted list of (name, SystemId) pairs.
//   Nodes with zero in-degree are inserted in lexicographic name order so
//   that the output order is identical regardless of registration order.
//
// -fno-exceptions clean; errors returned via std::unexpected.

#include "dag_builder.hpp"

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "glibre/error.hpp"

namespace glibre::core::detail {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

// Returns true if two TypeId spans share at least one element.
// O(n*m); acceptable at schedule-build time (SPEC §6.11 / §9).
[[nodiscard]] bool intersects(
    const std::vector<TypeId>& a,
    const std::vector<TypeId>& b
) noexcept {
    for (const auto& ta : a) {
        for (const auto& tb : b) {
            if (ta == tb) return true;
        }
    }
    return false;
}

// Edge in the adjacency representation used by the Kahn pass.
struct Edge {
    std::size_t to;    // Index into the nodes[] array.
    EdgeKind kind;
};

}  // namespace

// ---------------------------------------------------------------------------
// build_phase — public entry point
// ---------------------------------------------------------------------------

[[nodiscard]] Result<std::vector<SystemId>>
build_phase(std::span<const SystemNode> nodes) noexcept {
    const std::size_t n = nodes.size();

    if (n == 0) {
        return std::vector<SystemId>{};
    }

    // Build a name→index lookup for after/before resolution.
    // Using unordered_map with string_view keys; keys are stable non-owning
    // views into Schedule::Impl's internal string storage.
    std::unordered_map<std::string_view, std::size_t> name_to_idx;
    name_to_idx.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        name_to_idx.emplace(nodes[i].name, i);
    }

    // Adjacency list: adj[i] = list of (j, kind) edges meaning i must run before j.
    std::vector<std::vector<Edge>> adj(n);
    // In-degree per node.
    std::vector<std::size_t> in_degree(n, 0);

    // Build edges.
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            if (i == j) continue;

            // Access-set edges: i.writes ∩ j.reads ≠ ∅ → i→j
            if (intersects(nodes[i].writes, nodes[j].reads)) {
                adj[i].push_back({j, EdgeKind::AccessSet});
                ++in_degree[j];
                continue;  // Already added i→j; skip further checks for (i,j).
            }

            // Access-set edges: i.writes ∩ j.writes ≠ ∅ → i→j
            if (intersects(nodes[i].writes, nodes[j].writes)) {
                adj[i].push_back({j, EdgeKind::AccessSet});
                ++in_degree[j];
                continue;
            }
        }
    }

    // Explicit after/before edges.  These are processed after access-set edges
    // so that duplicates (same pair already connected via access-set) can be
    // detected and skipped to avoid inflated in-degree counts.
    //
    // Note: we scan adj[i] to detect duplicates (O(degree), small in practice).
    auto edge_exists = [&](std::size_t from, std::size_t to_idx) -> bool {
        for (const auto& e : adj[from]) {
            if (e.to == to_idx) return true;
        }
        return false;
    };

    for (std::size_t i = 0; i < n; ++i) {
        // after: nodes[j] where nodes[i].name ∈ nodes[j].after
        // → edge j→i (j runs before i)
        // Equivalently: for each name in nodes[i].after, find j and add j→i.
        for (const auto& after_name : nodes[i].after) {
            auto it = name_to_idx.find(after_name);
            if (it == name_to_idx.end()) continue;  // Unknown system; skip.
            const std::size_t j = it->second;
            if (j == i) continue;
            // Edge j→i (j must run before i).
            if (!edge_exists(j, i)) {
                adj[j].push_back({i, EdgeKind::Explicit});
                ++in_degree[i];
            } else {
                // Edge j→i already exists (access-set); upgrade kind to Explicit
                // so cycle detection sees it as an explicit edge.
                for (auto& e : adj[j]) {
                    if (e.to == i) {
                        e.kind = EdgeKind::Explicit;
                        break;
                    }
                }
            }
        }

        // before: nodes[i].name ∈ nodes[j].before → edge i→j (i runs before j)
        // Equivalently: for each name in nodes[i].before, find j and add i→j.
        for (const auto& before_name : nodes[i].before) {
            auto it = name_to_idx.find(before_name);
            if (it == name_to_idx.end()) continue;
            const std::size_t j = it->second;
            if (j == i) continue;
            // Edge i→j (i must run before j).
            if (!edge_exists(i, j)) {
                adj[i].push_back({j, EdgeKind::Explicit});
                ++in_degree[j];
            } else {
                for (auto& e : adj[i]) {
                    if (e.to == j) {
                        e.kind = EdgeKind::Explicit;
                        break;
                    }
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // Kahn's algorithm with lexicographic tiebreaker.
    //
    // ready holds indices of nodes with in_degree == 0, sorted by name
    // (ascending lexicographic) so that output order is deterministic.
    // -----------------------------------------------------------------------

    // Build the initial ready set.
    // sorted_ready: pairs of (name, index) for easy lexicographic sorting.
    std::vector<std::pair<std::string_view, std::size_t>> sorted_ready;
    sorted_ready.reserve(n);

    for (std::size_t i = 0; i < n; ++i) {
        if (in_degree[i] == 0) {
            sorted_ready.push_back({nodes[i].name, i});
        }
    }
    std::sort(sorted_ready.begin(), sorted_ready.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<SystemId> result;
    result.reserve(n);

    while (!sorted_ready.empty()) {
        // Pop the lexicographically smallest ready node.
        const auto [name, idx] = sorted_ready.front();
        sorted_ready.erase(sorted_ready.begin());

        result.push_back(nodes[idx].id);

        // Reduce in-degree for all successors.
        std::vector<std::pair<std::string_view, std::size_t>> newly_ready;

        for (const auto& edge : adj[idx]) {
            --in_degree[edge.to];
            if (in_degree[edge.to] == 0) {
                newly_ready.push_back({nodes[edge.to].name, edge.to});
            }
        }

        // Merge newly_ready into sorted_ready maintaining sorted order.
        std::sort(newly_ready.begin(), newly_ready.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        // Merge two sorted lists.
        std::vector<std::pair<std::string_view, std::size_t>> merged;
        merged.reserve(sorted_ready.size() + newly_ready.size());
        std::merge(
            sorted_ready.begin(), sorted_ready.end(),
            newly_ready.begin(), newly_ready.end(),
            std::back_inserter(merged),
            [](const auto& a, const auto& b) { return a.first < b.first; }
        );
        sorted_ready = std::move(merged);
    }

    // -----------------------------------------------------------------------
    // Cycle detection: if result.size() < n, there is a cycle.
    // -----------------------------------------------------------------------
    if (result.size() < n) {
        // Determine whether the cycle is purely from access-set edges or
        // involves at least one explicit after/before edge.
        //
        // Strategy: inspect all edges where both endpoints are still in the
        // cycle (in_degree > 0 after Kahn).  Nodes still in the cycle have
        // in_degree > 0.
        //
        // Build a set of cycle-node indices (those whose in_degree > 0).
        // Then scan all edges between cycle nodes; if any has EdgeKind::Explicit
        // → SystemScheduleCycle, otherwise → ScheduleAccessConflict.

        bool has_explicit_edge = false;
        for (std::size_t i = 0; i < n; ++i) {
            if (in_degree[i] == 0) continue;  // Already processed (not in cycle).
            for (const auto& edge : adj[i]) {
                if (in_degree[edge.to] == 0) continue;  // Successor not in cycle.
                if (edge.kind == EdgeKind::Explicit) {
                    has_explicit_edge = true;
                    break;
                }
            }
            if (has_explicit_edge) break;
        }

        if (has_explicit_edge) {
            return std::unexpected(glibre::Error{glibre::core::Error::SystemScheduleCycle});
        }
        return std::unexpected(glibre::Error{glibre::core::Error::ScheduleAccessConflict});
    }

    return result;
}

}  // namespace glibre::core::detail
