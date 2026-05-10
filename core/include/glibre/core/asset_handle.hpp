#pragma once
// core/include/glibre/core/asset_handle.hpp
//
// AssetHandle<T> — opaque 64-bit generational handle into the asset table.
//
// Authority: specs/core/SPEC.md §4.7, §5.2, §6.8; plan #598.
//
// ## Bit layout (§6.8)
//
//   bits[39: 0] = slot index    (low 40 bits,  max 2^40 ≈ 1 trillion slots)
//   bits[61:40] = generation    (next 22 bits,  max 2^22 ≈ 4 million reuses)
//   bits[63:62] = type_tag      (high 2 bits,   max 4 registered types)
//
//   Rationale for index-low layout:
//     A zero-initialized AssetHandle (bits == 0) corresponds to slot 0,
//     generation 0, type_tag 0.  AssetTable's slot at index 0 with
//     generation 0 is the "null" / never-issued state — generation 0 is
//     never issued to a caller, so a default-constructed AssetHandle{} is
//     always stale (SPEC §4.7 invariant 1).
//
// ## Opacity
//
//   The bit layout is private to `core::detail`.  Public plugin code
//   receives `AssetHandle<T>` as an opaque 64-bit value and never
//   interprets the payload — only the resolving plugin does (§4.7 inv. 2).
//
// ## -fno-exceptions clean
//   All functions are `noexcept`.  No exceptions thrown or propagated.

#include <cstdint>
#include <utility>

namespace glibre::core {

// ---------------------------------------------------------------------------
// AssetHandle<T> — opaque 64-bit generational handle (SPEC §4.7, §5.2)
//
// The template parameter T identifies the payload type for type-safety at
// call sites; it does not affect the binary representation.
// ---------------------------------------------------------------------------

template<class T>
struct AssetHandle {
    std::uint64_t bits{};

    // Equality compares the raw bits (SPEC §5.2): two AssetHandle values are
    // equal iff they encode the same (index, generation, type_tag) triple.
    friend constexpr bool operator==(AssetHandle, AssetHandle) noexcept = default;
};

// ---------------------------------------------------------------------------
// core::detail — internal packing helpers (NOT part of public plugin ABI)
//
// Used by AssetTable and AssetRegistry to pack/unpack the index/generation/
// type_tag triple.  Public plugin code never calls these helpers; they live
// in a sub-namespace (not a private-class scope) so that internal TUs can
// access them without friend declarations.
//
// Layout:
//   bits[39: 0] = index    (40 bits)
//   bits[61:40] = generation (22 bits)
//   bits[63:62] = type_tag  (2 bits)
//
// Constants:
//   kAssetIndexBits      = 40
//   kAssetGenerationBits = 22
//   kAssetTypeTagBits    = 2
// ---------------------------------------------------------------------------

namespace detail {

using AssetIndex = std::uint64_t;       // 40-bit slot index
using AssetGeneration = std::uint32_t;  // 22-bit generation counter
using AssetTypeTag = std::uint8_t;      // 2-bit type tag

inline constexpr std::uint32_t kAssetIndexBits = 40;
inline constexpr std::uint32_t kAssetGenerationBits = 22;
inline constexpr std::uint32_t kAssetTypeTagBits = 2;

inline constexpr std::uint64_t kAssetIndexMask =
    (std::uint64_t{1} << kAssetIndexBits) - 1;  // 0x0000'00FF'FFFF'FFFF
inline constexpr std::uint64_t kAssetGenerationMask =
    (std::uint64_t{1} << kAssetGenerationBits) - 1;  // 0x3F'FFFF (22 bits)
inline constexpr std::uint64_t kAssetTypeTagMask =
    (std::uint64_t{1} << kAssetTypeTagBits) - 1;  // 0x3 (2 bits)

// Maximum values (exclusive) for each field.
inline constexpr AssetIndex kAssetIndexMax = kAssetIndexMask;
inline constexpr AssetGeneration kAssetGenerationMax =
    static_cast<AssetGeneration>(kAssetGenerationMask);
inline constexpr AssetTypeTag kAssetTypeTagMax = static_cast<AssetTypeTag>(kAssetTypeTagMask);

/// Pack a (index, generation, type_tag) triple into an AssetHandle's bits.
///
/// Preconditions (callers must satisfy):
///   index    < 2^40  (fits in 40 bits)
///   generation < 2^22 (fits in 22 bits)
///   type_tag < 4     (fits in 2 bits)
///
/// No runtime bounds checks in release builds; the allocator invariants
/// ensure these are always satisfied.
template<class T>
[[nodiscard]] constexpr AssetHandle<T>
asset_pack(AssetIndex index, AssetGeneration generation, AssetTypeTag type_tag) noexcept {
    return AssetHandle<T>{
        (index & kAssetIndexMask) |
        ((static_cast<std::uint64_t>(generation) & kAssetGenerationMask) << kAssetIndexBits) |
        ((static_cast<std::uint64_t>(type_tag) & kAssetTypeTagMask)
         << (kAssetIndexBits + kAssetGenerationBits))
    };
}

/// Unpack the (index, generation, type_tag) triple from an AssetHandle's bits.
///
/// Returns {index, generation, type_tag} in that order.
template<class T>
[[nodiscard]] constexpr std::tuple<AssetIndex, AssetGeneration, AssetTypeTag>
asset_unpack(AssetHandle<T> h) noexcept {
    auto index = static_cast<AssetIndex>(h.bits & kAssetIndexMask);
    auto generation =
        static_cast<AssetGeneration>((h.bits >> kAssetIndexBits) & kAssetGenerationMask);
    auto type_tag = static_cast<AssetTypeTag>(
        (h.bits >> (kAssetIndexBits + kAssetGenerationBits)) & kAssetTypeTagMask
    );
    return {index, generation, type_tag};
}

}  // namespace detail

}  // namespace glibre::core
