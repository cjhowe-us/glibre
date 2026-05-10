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
// writes∩writes serialization:
//   Two systems that both write the same component must be serialized but
//   have no semantic dependency on ordering.  We add exactly ONE directed
//   edge per (i, j) pair where i < j (nodes are pre-sorted by name in
//   compile() before being passed here, so i < j ↔ name[i] < name[j]).
//   This avoids an immediate bidirectional cycle while preserving the lex
//   tiebreaker (the smaller-named system runs first).
//
// -fno-exceptions clean; errors returned via std::unexpected.

#include "dag_builder.hpp"

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "glibre/error.hpp"

namespace glibre::core::detail {

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

namespace {

// Returns true if two TypeId spans share at least one element.
// O(n*m); acceptable at schedule-build time (SPEC §6.11 / §9).
[[nodiscard]] bool intersects(const std::vector<TypeId>& a, const std::vector<TypeId>& b) noexcept {
    return std::ranges::any_of(a, [&](const TypeId& ta) {
        return std::ranges::any_of(b, [&](const TypeId& tb) { return ta == tb; });
    });
}

// Edge in the adjacency representation used by the Kahn pass.
struct Edge {
    std::size_t to;  // Index into the nodes[] array.
    EdgeKind kind;
};

// Per-node working state used during the Kahn pass.
struct NodeState {
    std::size_t in_degree{0};
    std::vector<Edge> adj;  // Outgoing edges (this node → adj[i].to).
};

}  // namespace

// ---------------------------------------------------------------------------
// build_access_set_edges — add writes∩reads and writes∩writes edges.
//
// Called once per phase from build_phase().  Mutates states[] in-place.
// ---------------------------------------------------------------------------

namespace {

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
// std::span does not provide .at() in C++23; operator[] is bounds-safe
// here because all loop variables i, j are bounded to [0, n) and
// edge.to values are assigned from i/j which are also in [0, n).

void build_access_set_edges(
    std::span<const SystemNode> nodes, std::vector<NodeState>& states
) noexcept {
    const std::size_t n = nodes.size();

    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            if (i == j) {
                continue;
            }

            // writes∩reads: i must run before j (read-after-write).
            if (intersects(nodes[i].writes, nodes[j].reads)) {
                states.at(i).adj.push_back({.to = j, .kind = EdgeKind::AccessSet});
                ++states.at(j).in_degree;
                continue;  // Already added i→j; skip writes∩writes for same pair.
            }

            // writes∩writes: serialize the pair with i→j only when i < j.
            // Nodes are pre-sorted by name so i < j ↔ name[i] < name[j].
            // This ensures exactly one directed edge per conflicting pair
            // without creating an immediate bidirectional cycle.
            if (i < j && intersects(nodes[i].writes, nodes[j].writes)) {
                states.at(i).adj.push_back({.to = j, .kind = EdgeKind::AccessSet});
                ++states.at(j).in_degree;
            }
        }
    }
}

// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

// ---------------------------------------------------------------------------
// edge_exists — O(degree) check for an existing i→j edge.
// ---------------------------------------------------------------------------

[[nodiscard]] bool
edge_exists(const std::vector<NodeState>& states, std::size_t from, std::size_t to) noexcept {
    return std::ranges::any_of(states.at(from).adj, [to](const Edge& e) { return e.to == to; });
}

// ---------------------------------------------------------------------------
// add_or_upgrade_edge — add an explicit edge i→j, or upgrade an existing
// access-set edge to Explicit so cycle detection sees the right kind.
// ---------------------------------------------------------------------------

void add_or_upgrade_edge(
    std::vector<NodeState>& states, std::size_t from, std::size_t to
) noexcept {
    if (!edge_exists(states, from, to)) {
        states.at(from).adj.push_back({.to = to, .kind = EdgeKind::Explicit});
        ++states.at(to).in_degree;
        return;
    }
    // Edge already exists from access-set pass; upgrade kind to Explicit so
    // cycle classification treats it as an explicit edge.
    for (Edge& e : states.at(from).adj) {
        if (e.to == to) {
            e.kind = EdgeKind::Explicit;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// build_explicit_edges — add after/before ordering edges.
// ---------------------------------------------------------------------------

void build_explicit_edges(
    std::span<const SystemNode> nodes,
    const std::unordered_map<std::string_view, std::size_t>& name_to_idx,
    std::vector<NodeState>& states
) noexcept {
    const std::size_t n = nodes.size();

    for (std::size_t i = 0; i < n; ++i) {
        // after: system i runs after each named system j.
        // → edge j→i (j must complete before i starts).
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        for (const std::string_view& after_name : nodes[i].after) {
            const auto it = name_to_idx.find(after_name);
            if (it == name_to_idx.end()) {
                continue;  // Named system not in this phase; silently skip.
            }
            const std::size_t j = it->second;
            if (j == i) {
                continue;
            }
            add_or_upgrade_edge(states, j, i);
        }

        // before: system i runs before each named system j.
        // → edge i→j (i must complete before j starts).
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        for (const std::string_view& before_name : nodes[i].before) {
            const auto it = name_to_idx.find(before_name);
            if (it == name_to_idx.end()) {
                continue;
            }
            const std::size_t j = it->second;
            if (j == i) {
                continue;
            }
            add_or_upgrade_edge(states, i, j);
        }
    }
}

// ---------------------------------------------------------------------------
// kahn_sort — run Kahn's algorithm with lexicographic tiebreaker.
//
// ready holds indices sorted by name (ascending lexicographic).  On each
// iteration we pop the smallest-named ready node, add it to result, and
// reduce in-degree for its successors.  Newly ready nodes are merged into
// the sorted ready list.
//
// Returns the sorted order.  If result.size() < n after the loop, there is
// a cycle and the caller must detect it.
// ---------------------------------------------------------------------------

[[nodiscard]] std::vector<SystemId>
kahn_sort(std::span<const SystemNode> nodes, std::vector<NodeState>& states) noexcept {
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // std::span::operator[] is bounds-safe here: i ∈ [0, n), idx is i or
    // edge.to which was assigned from i ∈ [0, n).
    const std::size_t n = nodes.size();

    // Build initial ready set: all nodes with in_degree == 0.
    std::vector<std::pair<std::string_view, std::size_t>> ready;
    ready.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (states.at(i).in_degree == 0) {
            ready.emplace_back(nodes[i].name, i);
        }
    }
    std::ranges::sort(ready, [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<SystemId> result;
    result.reserve(n);

    while (!ready.empty()) {
        const auto [name, idx] = ready.front();
        ready.erase(ready.begin());

        result.push_back(nodes[idx].id);

        // Collect newly-ready successors.
        std::vector<std::pair<std::string_view, std::size_t>> newly_ready;
        for (const Edge& edge : states.at(idx).adj) {
            --states.at(edge.to).in_degree;
            if (states.at(edge.to).in_degree == 0) {
                newly_ready.emplace_back(nodes[edge.to].name, edge.to);
            }
        }

        // Merge newly_ready into ready (both lists already sorted).
        std::ranges::sort(newly_ready, [](const auto& a, const auto& b) {
            return a.first < b.first;
        });
        std::vector<std::pair<std::string_view, std::size_t>> merged;
        merged.reserve(ready.size() + newly_ready.size());
        std::ranges::merge(
            ready, newly_ready, std::back_inserter(merged), [](const auto& a, const auto& b) {
                return a.first < b.first;
            }
        );
        ready = std::move(merged);
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

    return result;
}

// ---------------------------------------------------------------------------
// classify_cycle — determine whether the cycle involves any explicit edges.
//
// Nodes still in the cycle have in_degree > 0 after Kahn.  We inspect all
// edges between cycle-nodes: if any has EdgeKind::Explicit → SystemScheduleCycle;
// otherwise → ScheduleAccessConflict.
// ---------------------------------------------------------------------------

[[nodiscard]] glibre::Error classify_cycle(const std::vector<NodeState>& states) noexcept {
    for (const NodeState& state : states) {
        if (state.in_degree == 0) {
            continue;  // Not in a cycle.
        }
        for (const Edge& edge : state.adj) {
            if (states.at(edge.to).in_degree == 0) {
                continue;  // Successor already processed; not in the cycle.
            }
            if (edge.kind == EdgeKind::Explicit) {
                return glibre::Error{glibre::core::Error::SystemScheduleCycle};
            }
        }
    }
    return glibre::Error{glibre::core::Error::ScheduleAccessConflict};
}

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

    // Build name→index lookup for after/before resolution.
    std::unordered_map<std::string_view, std::size_t> name_to_idx;
    name_to_idx.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        name_to_idx.emplace(nodes[i].name, i);
    }

    // Working state: adjacency + in-degree per node.
    std::vector<NodeState> states(n);

    // Phase 1: access-set edges (writes∩reads, writes∩writes).
    build_access_set_edges(nodes, states);

    // Phase 2: explicit after/before edges.
    build_explicit_edges(nodes, name_to_idx, states);

    // Phase 3: Kahn topological sort with lex tiebreaker.
    std::vector<SystemId> result = kahn_sort(nodes, states);

    // Phase 4: cycle detection.
    if (result.size() < n) {
        return std::unexpected(classify_cycle(states));
    }

    return result;
}

}  // namespace glibre::core::detail
