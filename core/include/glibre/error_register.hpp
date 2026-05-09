#pragma once
// core/include/glibre/error_register.hpp
//
// Per-context Error enum registration convention.
//
// Authority: reviews/decisions/error-model.md §Decision 2 ("Each context
// owns its enum"), §Consequences ("Adding a new context requires editing
// the glibre::Error variant alias in core. This is a deliberate central-
// registration point."), plan #234.
//
// ## Convention — how to add a new error context
//
//   1. Declare `enum class YourNs::Error : std::uint16_t { ... }` inside
//      the per-context header at `<your-context>/include/glibre/<your-context>/error.hpp`.
//      Forward-declare it in that same header (no other header defines it).
//
//   2. Open `core/include/glibre/error.hpp` and append `your-ns::Error`
//      to `glibre::Error::Variant`.  This file is the **single central
//      registration point** for the union.  Per error-model.md §Consequences,
//      editing this one line is the deliberate, discoverable cost of shipping
//      a new context — no reflection, no auto-discovery, no self-registration.
//
//   3. Open *this* file (`error_register.hpp`) and:
//      a. Add a `// CONTEXT: <your-context>` line to the registered-contexts
//         table below.
//      b. Add `constexpr eastl::string_view` entry in `kAllErrorContexts`.
//      c. Increment `kExpectedArmCount` by 1.
//      d. The file-scope `static_assert` below will fail to compile if you
//         forget step (c) — that is the intended compile-time guard.
//
//   4. Add a per-context enum uniqueness check to the Catch2 test suite under
//      `tests/core/error_register/` (test `error_register_per_context_arms_unique`).
//
// ## Rules that must hold at all times (enforced by static_assert below)
//
//   R1. The total number of Variant arms equals `kExpectedArmCount`.
//       Adding a context without incrementing this constant is a compile
//       error.
//   R2. Enumerator values within each per-context enum are unique (ensured
//       by the C++ definition; no two arms of a single enum share an integer
//       value unless explicitly assigned the same value — which is forbidden
//       by this convention).
//   R3. No two contexts define an enum with identical values such that
//       variant_index_v would become ambiguous; eastl::variant enforces this
//       by type identity, not by integer value.
//
// ## Why not a GLIBRE_REGISTER_ERROR_ENUM macro?
//
//   The decision record (error-model.md §Open Questions #5) explicitly
//   defers plugin-SDK error extension until after MVP.  A macro that
//   "auto-registers" an enum at static-init time would break the deterministic
//   ordering guarantee (Variant arm order affects variant_index, which affects
//   structured log fields).  The deliberate one-line-per-context edit here
//   keeps the order explicit, versioned, and compile-time-verified.
//
// --------------------------------------------------------------------------

#include <array>
#include <cstddef>

#include <EASTL/string_view.h>
#include <EASTL/variant.h>

#include <glibre/error.hpp>

namespace glibre {

// --------------------------------------------------------------------------
// Registered error contexts
//
// Each entry records:
//   - the context name string (used in log tags, error-model.md §Logging)
//   - the canonical header where that context's enum is declared
//
// CONTEXT: core   — glibre::core::Error   — core/include/glibre/error.hpp
// CONTEXT: render — glibre::render::Error — core/include/glibre/error.hpp
//                   (placeholder; render context has no plugin dylib yet)
// CONTEXT: tools  — glibre::tools::Error  — core/include/glibre/error.hpp
//                   (added by plan #219 — foryc skeleton)
//
// When a new context ships, add a CONTEXT line here + an entry in
// kAllErrorContexts + increment kExpectedArmCount.
// --------------------------------------------------------------------------

/// Human-readable names for all currently registered error contexts.
/// Entries are in the same order as glibre::Error::Variant arms.
/// Used by glibre::log_error() to map variant_index → context tag string.
inline constexpr eastl::array<eastl::string_view, 3> kAllErrorContexts{{
    "core",    // index 0 — glibre::core::Error
    "render",  // index 1 — glibre::render::Error
    "tools",   // index 2 — glibre::tools::Error
}};

/// Expected number of arms in glibre::Error::Variant.
///
/// Increment this constant when a new context is registered.  The
/// static_assert below will fail to compile if kExpectedArmCount drifts
/// from the actual variant arm count — catching the common mistake of
/// editing error.hpp without updating this registry.
inline constexpr std::size_t kExpectedArmCount = 3;

// --------------------------------------------------------------------------
// Compile-time invariant R1: arm count must match the manifest.
//
// This static_assert acts as the mechanical link between error.hpp (where
// Variant is defined) and this file (where the convention is documented).
// If a new arm is added to Variant without updating kExpectedArmCount, this
// fires immediately at compile time — preventing silent drift between the
// variant definition and the registry documentation.
// --------------------------------------------------------------------------
static_assert(
    eastl::variant_size_v<glibre::Error::Variant> == kExpectedArmCount,
    "glibre::Error::Variant arm count does not match kExpectedArmCount in "
    "error_register.hpp.  Either increment kExpectedArmCount to reflect the "
    "new context, or remove the extra arm from Error::Variant."
);

// --------------------------------------------------------------------------
// Compile-time invariant: kAllErrorContexts length matches kExpectedArmCount.
//
// Guards against adding a CONTEXT entry but forgetting to add a string
// in kAllErrorContexts, or vice versa.
// --------------------------------------------------------------------------
static_assert(
    kAllErrorContexts.size() == kExpectedArmCount,
    "kAllErrorContexts length does not match kExpectedArmCount.  "
    "Keep both in sync when registering a new error context."
);

}  // namespace glibre
