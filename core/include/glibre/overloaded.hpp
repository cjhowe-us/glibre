#pragma once
// core/include/glibre/overloaded.hpp
//
// Canonical Overloaded visitor helper for std::visit — one struct + one
// deduction guide per reviews/decisions/eastl-removal.md §4.
//
// SRP: the only reason this header changes is if C++ standardises this
// pattern (P2826 or similar) — at that point the header is deleted and
// callers are updated to the stdlib name.
//
// Usage:
//   std::visit(glibre::Overloaded{
//       [](glibre::core::Error e)   { /* ... */ },
//       [](glibre::render::Error e) { /* ... */ },
//       [](glibre::tools::Error e)  { /* ... */ },
//       [](glibre::shader::Error e) { /* ... */ },
//   }, err.code());
//
// std::visit requires the visitor to exhaustively cover every arm of the
// variant (all operator() overloads must accept every alternative).
// The Overloaded helper enables ad-hoc visitors inline without boilerplate.
//
// Authority: reviews/decisions/eastl-removal.md §4 ("The helper lives in
// core/include/glibre/overloaded.hpp; SRP-clean, no other reason to change").

#include <variant>  // std::visit uses the visitor via <variant>

namespace glibre {

/// Overloaded — ad-hoc multi-lambda visitor for std::visit.
///
/// Inherits operator() from each lambda and exposes all overloads for
/// ADL-based overload resolution inside std::visit.
template<class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

/// Deduction guide — allows `Overloaded{lambda1, lambda2, ...}` without
/// explicit template arguments.
template<class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

}  // namespace glibre
