// tests/core/error_propagation/transfer.hpp
//
// Private header shared between stub_error_returns.cpp and
// error_propagation_test.cpp (plan #240).
//
// Defines the flat POD layout used to transfer ErrorContext data across the
// C-ABI boundary, plus the sentinel string constants baked into the stub.
//
// Rationale: duplicating this struct across both TUs with a "keep in sync"
// comment is an SRP violation — two reasons to change one definition.
// Extracting here removes the manual-sync hazard.
//
// Layout stability: changing any field type or order is a breaking change for
// tests that memcpy the struct through the ABI boundary; both TUs must be
// recompiled together (they are in the same CMake target group).

#pragma once

#include <cstddef>

namespace glibre::test::error_propagation {

inline constexpr std::size_t kFileMax = 256;
inline constexpr std::size_t kDetailMax = 256;

// GlibreTestContextTransfer — POD mirror of the stub's transfer struct.
//
// ErrorContext holds std::string_view members (non-owning pointer+size
// references per reviews/decisions/eastl-removal.md §4, matrix row 2).
// Transferring raw bytes across the ABI boundary would produce dangling
// string_view pointers in the host. This struct copies the string data into
// fixed-size char arrays, making it safe to memcpy through the
// caller-supplied buffer.
struct GlibreTestContextTransfer {
    char file[kFileMax];  // null-terminated
    int line;
    char detail[kDetailMax];  // null-terminated
};

// Sentinel string values baked into the stub so the host can check them.
// There is no kSentinelLine constant — the line is captured via __LINE__ at
// the ErrorContext construction site, removing the fragile host-side magic
// number that required hand-mirroring.
inline constexpr const char* kSentinelFile = "stub_error_returns.cpp";
inline constexpr const char* kSentinelDetail = "PluginInitFailed-context-sentinel";

}  // namespace glibre::test::error_propagation
