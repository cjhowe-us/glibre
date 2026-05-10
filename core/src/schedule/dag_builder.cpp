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
//   ("explicit").  If ANY edge in the cycle is access-set → ScheduleAccessConflict.
//   Only when ALL cycle edges are explicit → SystemScheduleCycle.
//
//   Important: when an explicit after/before declaration coincides with an
//   access-set edge, the EdgeKind is NOT upgraded to Explicit.  The original
//   AccessSet kind is preserved so that the cycle classifier can correctly
//   attribute the root cause to the access-set conflict.
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
#include <functional>
#include <queue>
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
                // Dominance: writes∩reads takes priority over writes∩writes for the
                // same (i, j) pair.  Both would produce EdgeKind::AccessSet, so cycle
                // classification is unchanged — but skipping the writes∩writes check
                // avoids a duplicate edge.  This is intentional: a single AccessSet
                // edge per (i,j) direction is sufficient for §4.4 invariant 2
                // enforcement.  (R2 review LOW-2)
                continue;
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
// add_explicit_edge_if_absent — add an Explicit edge i→j only when no prior
// edge exists for that pair; leave an existing AccessSet edge unchanged.
//
// SRP: one responsibility — adding an explicit ordering edge, but only when
// the pair does not already carry an edge of any kind.
//
// Cycle classification rule: a cycle is SystemScheduleCycle only when ALL
// edges in the cycle are Explicit.  If any edge is AccessSet (even when an
// explicit declaration also coincides on that pair), the cycle is a
// ScheduleAccessConflict.  Therefore we must NOT add a second Explicit edge
// when an AccessSet edge already exists — the AccessSet edge must be the one
// classify_cycle inspects, preserving the correct root-cause attribution.
// (Renamed from add_or_upgrade_edge in R2 review MED-3: the post-R1 body
// no longer upgrades; the old name was actively misleading.)
// ---------------------------------------------------------------------------

void add_explicit_edge_if_absent(
    std::vector<NodeState>& states, std::size_t from, std::size_t to
) noexcept {
    if (!edge_exists(states, from, to)) {
        states.at(from).adj.push_back({.to = to, .kind = EdgeKind::Explicit});
        ++states.at(to).in_degree;
        return;
    }
    // Edge already exists from the access-set pass.  Do NOT add a second edge:
    // the cycle classifier checks whether ALL cycle edges are Explicit; an
    // edge that arose from access-set intersection must remain AccessSet even
    // when an explicit declaration also targets the same pair.  The existing
    // edge's kind correctly preserves ScheduleAccessConflict classification.
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
            add_explicit_edge_if_absent(states, j, i);
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
            add_explicit_edge_if_absent(states, i, j);
        }
    }
}

// ---------------------------------------------------------------------------
// kahn_sort — run Kahn's algorithm with lexicographic tiebreaker.
//
// The ready set is a min-heap keyed by (name, idx) so that the smallest-named
// ready node is always at the top.  Inserting a newly-ready successor is
// O(log n); no O(n) erase+merge is required.
//
// Returns the sorted order.  If result.size() < n after the loop, there is
// a cycle and the caller must detect it.
// ---------------------------------------------------------------------------

// Comparator: min-heap on name (lexicographic ascending), with idx as a
// stable tiebreaker for equal names (each system has a unique name, so idx
// is only needed if names were equal — kept for robustness).
struct ReadyEntryGreater {
    bool operator()(
        const std::pair<std::string_view, std::size_t>& a,
        const std::pair<std::string_view, std::size_t>& b
    ) const noexcept {
        if (a.first != b.first) {
            return a.first > b.first;  // min-heap: greater name sinks down.
        }
        return a.second > b.second;
    }
};

[[nodiscard]] std::vector<SystemId>
kahn_sort(std::span<const SystemNode> nodes, std::vector<NodeState>& states) noexcept {
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // std::span::operator[] is bounds-safe here: i ∈ [0, n), idx is i or
    // edge.to which was assigned from i ∈ [0, n).
    const std::size_t n = nodes.size();

    using Entry = std::pair<std::string_view, std::size_t>;

    // Build initial ready set: all nodes with in_degree == 0.
    // std::priority_queue is a max-heap by default; ReadyEntryGreater inverts
    // the comparison to produce a min-heap ordered by name ascending.
    std::priority_queue<Entry, std::vector<Entry>, ReadyEntryGreater> ready;
    for (std::size_t i = 0; i < n; ++i) {
        if (states.at(i).in_degree == 0) {
            ready.emplace(nodes[i].name, i);
        }
    }

    std::vector<SystemId> result;
    result.reserve(n);

    while (!ready.empty()) {
        const auto [name, idx] = ready.top();
        ready.pop();

        result.push_back(nodes[idx].id);

        // Reduce in-degree for successors; push newly-ready ones into the heap.
        // Each push is O(log n); total across all iterations: O(n log n).
        for (const Edge& edge : states.at(idx).adj) {
            --states.at(edge.to).in_degree;
            if (states.at(edge.to).in_degree == 0) {
                ready.emplace(nodes[edge.to].name, edge.to);
            }
        }
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

    return result;
}

// ---------------------------------------------------------------------------
// classify_cycle — classify the cycle using OriginalKind-preserving logic.
//
// Nodes still in the cycle have in_degree > 0 after Kahn.  We inspect all
// edges between cycle-nodes:
//   - If ANY edge is EdgeKind::AccessSet → ScheduleAccessConflict.
//     (The access-set intersection is the root cause; plugin authors must
//     resolve the conflicting write declarations, not the after/before set.)
//   - If ALL edges are EdgeKind::Explicit → SystemScheduleCycle.
//     (The cycle is purely a logical ordering contradiction declared by the
//     plugin author via after/before; no access-set conflict exists.)
//
// Rationale: add_explicit_edge_if_absent does not upgrade AccessSet→Explicit
// when an explicit declaration coincides with an access-set edge.  Therefore
// an AccessSet edge in the cycle reliably signals the access-set root cause.
// The previous early-return on the first Explicit edge was correct only with
// the upgrade semantics; without upgrade, we must scan all cycle edges.
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
            if (edge.kind == EdgeKind::AccessSet) {
                // Any access-set edge → access-set conflict dominates.
                return glibre::Error{glibre::core::Error::ScheduleAccessConflict};
            }
        }
    }
    // All cycle edges are Explicit → pure ordering cycle.
    return glibre::Error{glibre::core::Error::SystemScheduleCycle};
}

}  // namespace

// ---------------------------------------------------------------------------
// build_phase — public entry point
//
// Peak-memory carve-out (R2 review LOW-3, §9/§11.6):
//   build_phase() returns a std::vector<SystemId> via global-new.  Schedule::
//   compile() assigns its contents into the PMR compiled_phases slots and then
//   the transient vector is destroyed.  During the compile() call the in-flight
//   peak is doubled: one transient std::vector<SystemId> (global-new) plus the
//   final pmr::vector<SystemId> (PMR-counted).  The same applies to the
//   phase_nodes std::array<std::vector<SystemNode>> scratch in compile().
//   For MVP scale (~200 systems) this is sub-MB and accepted.
//   Track as a follow-up when a TransientArena lands for compile-time scratch
//   (per perf-budget.md §"Schedule Build").
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
