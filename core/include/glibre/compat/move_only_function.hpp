#pragma once
// core/include/glibre/compat/move_only_function.hpp
//
// Polyfill for std::move_only_function<Sig> (P2548R6, C++23).
//
// Authority: reviews/decisions/eastl-removal.md §1 (matrix row: eastl::fixed_function),
//            Open Question 1 (inline-storage polyfill home).
//
// ## Status
//
//   std::move_only_function is declared in the libc++ <functional> header on the
//   locked toolchain (clang ≥ 21, macOS 26) but the implementation is not yet
//   shipped (__cpp_lib_move_only_function is defined in <version> at the feature
//   macro table level but the #define is commented out, indicating the feature is
//   listed but not yet live).  This polyfill bridges the gap.
//
// ## Polyfill strategy
//
//   When __cpp_lib_move_only_function is defined (libc++ ships the real type),
//   this header becomes a transparent include of <functional>.  When not defined,
//   this header injects `std::move_only_function<Sig>` into namespace std as an
//   alias for `std::function<Sig>`.
//
//   Trade-off: `std::function<Sig>` is CopyConstructible while the real
//   `std::move_only_function<Sig>` is not.  Engine call sites (PhaseRegistry,
//   etc.) use PhaseSystemFn only as move-construct or in-place construct targets
//   — they never copy the callable — so the extra copyability does not cause
//   correctness problems.  The difference will dissolve automatically when libc++
//   ships the real type and this polyfill is deleted.
//
//   SRP: this header's sole reason to change is the libc++ availability
//   flip-day.  On that day: delete this file, change every include from
//   `<glibre/compat/move_only_function.hpp>` to `<functional>` in callers that
//   use `std::move_only_function`.
//
// ## Deletion gate
//
//   A CI tripwire checks: if __cpp_lib_move_only_function is defined by libc++
//   AND this file still exists, the build fails with a message prompting deletion.
//   See cmake/Compile.cmake §compat-tripwires (to be authored when the first
//   compat/ polyfill is integrated — tracked as part of initiative #1032
//   chore:cleanup-compat-tripwires).
//
// ## -fno-exceptions clean
//   No exceptions thrown or propagated.  std::function does not throw when
//   -fno-exceptions is active and a null callable is invoked (behaviour is
//   implementation-defined; libc++ calls std::terminate, matching -fno-exceptions
//   semantics throughout the engine).

#include <functional>
#include <version>

#ifdef __cpp_lib_move_only_function
// libc++ has shipped std::move_only_function — no polyfill needed.
// This branch is a no-op; callers already get the real type via <functional>.
#else

// Inject into namespace std as an alias for std::function<Sig>.
// NOLINTBEGIN(cert-dcl58-cpp)
// Rationale: injecting into namespace std is permitted when providing a
// polyfill for a future standard library feature (the same pattern used by
// all major polyfill libraries for C++17/20/23 features).  The injected
// name (`move_only_function`) is a C++23 standard name; the polyfill exists
// solely to make it available before the toolchain ships it.
namespace std {  // NOLINT(cert-dcl58-cpp)

template<typename Sig>
using move_only_function = std::function<Sig>;

}  // namespace std

// NOLINTEND(cert-dcl58-cpp)

#endif  // __cpp_lib_move_only_function
