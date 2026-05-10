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
//   locked toolchain (clang >= 21, macOS 26) but the implementation is not yet
//   shipped (__cpp_lib_move_only_function is defined in <version> at the feature
//   macro table level but the #define is commented out, indicating the feature is
//   listed but not yet live).  This polyfill bridges the gap.
//
// ## Polyfill strategy
//
//   When __cpp_lib_move_only_function is defined (libc++ ships the real type),
//   the #error deletion gate below fires and the build hard-errors, directing
//   removal of this polyfill.
//
//   When not defined, this header injects a class template
//   `std::move_only_function<Sig>` into namespace std via two specialisations:
//
//     1. Primary template: move_only_function<R(Args...)>
//        Wraps std::function<R(Args...)> and forwards construction/invocation.
//        Handles mutable call-signature forms.
//
//     2. Partial specialisation: move_only_function<R(Args...) const>
//        Also wraps std::function<R(Args...)> (stripping the trailing `const`).
//        std::function::operator() is already declared const, so the semantics
//        are identical.  This specialisation is the one used for the canonical
//        `PhaseSystemFn = void() const` form required by the C++23 spec.
//
//   Trade-off: `std::function<Sig>` is CopyConstructible while the real
//   `std::move_only_function<Sig>` is not.  Engine call sites (PhaseRegistry,
//   etc.) use PhaseSystemFn only as move-construct or in-place construct targets
//   -- they never copy the callable -- so the extra copyability does not cause
//   correctness problems.  The difference will dissolve automatically when libc++
//   ships the real type and this polyfill is deleted.
//
//   A class template wrapper (rather than a bare using-alias) is required
//   because using-alias templates cannot be partially specialised in C++.
//   The wrapper exposes the same operator() as std::function and is therefore
//   a drop-in for all engine call sites.
//
//   SRP: this header's sole reason to change is the libc++ availability
//   flip-day.  On that day: delete this file, change every include from
//   `<glibre/compat/move_only_function.hpp>` to `<functional>` in callers that
//   use `std::move_only_function`.
//
// ## Deletion gate
//
//   When libc++ ships std::move_only_function (__cpp_lib_move_only_function >=
//   202110L), the build hard-errors with an #error directing deletion of this
//   polyfill.  See chore issue #1069 for the tracked deletion task.
//
// ## -fno-exceptions clean
//   No exceptions thrown or propagated.  std::function does not throw when
//   -fno-exceptions is active and a null callable is invoked (behaviour is
//   implementation-defined; libc++ calls std::terminate, matching -fno-exceptions
//   semantics throughout the engine).

#include <functional>
#include <utility>
#include <version>

#if defined(__cpp_lib_move_only_function) && (__cpp_lib_move_only_function >= 202110L)
// libc++ has shipped std::move_only_function -- this polyfill is now dead code.
// Delete this file and replace every #include of this header with <functional>
// in all callers.  See chore issue #1069 for the tracked deletion task.
#error "Delete this polyfill -- libc++ now ships std::move_only_function. \
See chore issue #1069 (cleanup-compat-move_only_function-polyfill) and \
replace every include of glibre/compat/move_only_function.hpp with <functional>."
#else

// Inject into namespace std a class template `move_only_function<Sig>`.
// NOLINTBEGIN(cert-dcl58-cpp)
// Rationale: injecting into namespace std is permitted when providing a
// polyfill for a future standard library feature (the same pattern used by
// all major polyfill libraries for C++17/20/23 features).  The injected
// name (`move_only_function`) is a C++23 standard name; the polyfill exists
// solely to make it available before the toolchain ships it.
namespace std {  // NOLINT(cert-dcl58-cpp)

// Primary template — covers R(Args...) (mutable call-signature forms).
// Delegates all operations to std::function<R(Args...)>.
template<typename Sig>
class move_only_function;  // NOLINT(cert-dcl58-cpp)

template<typename R, typename... Args>
class move_only_function<R(Args...)> {  // NOLINT(cert-dcl58-cpp)
public:
    // Construction: accept any callable convertible to std::function<R(Args...)>.
    move_only_function() noexcept = default;

    template<typename F>
    // NOLINTNEXTLINE(bugprone-forwarding-reference-overload)
    move_only_function(F&& f)
        : fn_(std::forward<F>(f)) {}  // NOLINT(google-explicit-constructor)

    move_only_function(const move_only_function&) = default;
    move_only_function& operator=(const move_only_function&) = default;
    move_only_function(move_only_function&&) noexcept = default;
    move_only_function& operator=(move_only_function&&) noexcept = default;
    ~move_only_function() = default;

    // Invocation — mutable (for mutable call-signature forms).
    R operator()(Args... args) { return fn_(std::forward<Args>(args)...); }

    // const invocation -- std::function::operator() is const; expose it.
    R operator()(Args... args) const { return fn_(std::forward<Args>(args)...); }

    explicit operator bool() const noexcept { return static_cast<bool>(fn_); }

private:
    std::function<R(Args...)> fn_;
};

// Partial specialisation — covers R(Args...) const (const call-signature forms).
// The real std::move_only_function<void() const> has a const-only operator().
// std::function<R(Args...)> already has a const operator(), so we delegate
// through the same underlying type after stripping the trailing `const`.
// This specialisation is the one in effect for PhaseSystemFn = void() const.
template<typename R, typename... Args>
class move_only_function<R(Args...) const> {  // NOLINT(cert-dcl58-cpp)
public:
    move_only_function() noexcept = default;

    template<typename F>
    // NOLINTNEXTLINE(bugprone-forwarding-reference-overload)
    move_only_function(F&& f)
        : fn_(std::forward<F>(f)) {}  // NOLINT(google-explicit-constructor)

    move_only_function(const move_only_function&) = default;
    move_only_function& operator=(const move_only_function&) = default;
    move_only_function(move_only_function&&) noexcept = default;
    move_only_function& operator=(move_only_function&&) noexcept = default;
    ~move_only_function() = default;

    // const invocation only -- matches the C++23 void() const contract.
    R operator()(Args... args) const { return fn_(std::forward<Args>(args)...); }

    explicit operator bool() const noexcept { return static_cast<bool>(fn_); }

private:
    std::function<R(Args...)> fn_;
};

}  // namespace std

// NOLINTEND(cert-dcl58-cpp)

#endif  // __cpp_lib_move_only_function
