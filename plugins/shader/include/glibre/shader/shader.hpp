#pragma once
// plugins/shader/include/glibre/shader/shader.hpp
//
// Public interface for the shader bounded context.
//
// Authority: specs/shader/SPEC.md §5.
//
// This header is the single compileable stub of the shader context public
// boundary. Compile cleanly with clang++ -std=c++23 -fsyntax-only, both
// with -DGLIBRE_SHIPPING=0 (default; tooling builds) and -DGLIBRE_SHIPPING=1
// (the entire compile() virtual is #if-guarded out so shipping plugins never
// link slangc subprocess code — §4.3 inv 3, §4.8 cross-aggregate inv 3).
//
// Error type: glibre::shader::Error is declared in core/include/glibre/error.hpp
// and registered in the engine-wide glibre::Error variant (error-model.md §Decision 2).
// Include glibre/error.hpp to get the enum; this header re-exports it via the
// include below.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>

#include <EASTL/array.h>
#include <EASTL/span.h>
#include <EASTL/string.h>
#include <EASTL/string_view.h>
#include <EASTL/vector.h>

// glibre::shader::Error is defined in error.hpp (central registration point).
// This also gives callers glibre::Result<T> and the GLIBRE_TRY macros.
#include <glibre/error.hpp>

#ifndef GLIBRE_SHIPPING
#define GLIBRE_SHIPPING 0
#endif

namespace glibre::shader {

// shader::Error is defined in glibre/error.hpp — imported above.

// ---------------------------------------------------------------------------
// Closed enums for the 4-axis permutation key (§4.2)
// ---------------------------------------------------------------------------

enum class ShadingModel : std::uint8_t {
    Standard,
    Skin,
    Hair,
    Cloth,
    Foliage,
    Eye,
    Water,
    ClearCoat,
};
inline constexpr std::size_t kShadingModelCount = 8;

enum class FeatureBit : std::uint8_t {
    Skinned = 0,
    MotionVectors = 1,
    AlphaTest = 2,
    Decal = 3,
    VirtualTexture = 4,
    RT = 5,
};
inline constexpr std::size_t kFeatureBitCount = 6;
// Bit-mask covering all defined FeatureBit positions.
// Used in from_bytes validation to reject reserved-bit usage.
inline constexpr std::uint16_t kFeatureBitMask =
    static_cast<std::uint16_t>((std::uint16_t{1} << kFeatureBitCount) - 1u);  // = 0x003F

class FeatureSet {
public:
    constexpr FeatureSet() noexcept = default;

    constexpr explicit FeatureSet(std::uint16_t bits) noexcept
        : bits_{bits} {}

    [[nodiscard]] constexpr bool test(FeatureBit b) const noexcept {
        return (bits_ & (std::uint16_t{1} << static_cast<std::uint8_t>(b))) != 0;
    }

    constexpr void set(FeatureBit b) noexcept {
        bits_ |= static_cast<std::uint16_t>(std::uint16_t{1} << static_cast<std::uint8_t>(b));
    }

    [[nodiscard]] constexpr std::uint16_t bits() const noexcept { return bits_; }

    friend constexpr bool operator==(FeatureSet, FeatureSet) noexcept = default;

private:
    std::uint16_t bits_{0};
};

enum class RenderPath : std::uint8_t {
    Forward,
    Deferred,
    DepthOnly,
    Shadow,
    Velocity,
    Probe,
};
inline constexpr std::size_t kRenderPathCount = 6;

enum class LODTier : std::uint8_t { Mobile, Switch, Desktop, HighEnd };
inline constexpr std::size_t kLODTierCount = 4;

// ---------------------------------------------------------------------------
// PermutationKey (value object, §4.2)
// ---------------------------------------------------------------------------

struct PermutationKey {
    ShadingModel shading_model{ShadingModel::Standard};
    FeatureSet features{};
    RenderPath render_path{RenderPath::Forward};
    LODTier lod_tier{LODTier::Desktop};

    friend constexpr bool operator==(PermutationKey, PermutationKey) noexcept = default;

    // Total, injective, bit-stable encoding (§4.2 invariant 1).
    using PackedBytes = eastl::array<std::byte, 6>;
    PackedBytes to_bytes() const noexcept;
    static glibre::Result<PermutationKey> from_bytes(const PackedBytes&) noexcept;

    bool is_well_formed() const noexcept;
};

// Dense codegen ordinal across the enumerable cross-product (§4.2).
struct PermutationIndex {
    std::uint32_t value{0};
    friend constexpr bool operator==(PermutationIndex, PermutationIndex) noexcept = default;
};

// Total number of distinct well-formed permutations.
// = kShadingModelCount x 2^kFeatureBitCount x kRenderPathCount x kLODTierCount = 12288.
inline constexpr std::uint32_t kPermutationCrossProductCardinality =
    static_cast<std::uint32_t>(kShadingModelCount) * (1u << kFeatureBitCount) *
    static_cast<std::uint32_t>(kRenderPathCount) * static_cast<std::uint32_t>(kLODTierCount);

PermutationIndex to_index(const PermutationKey&) noexcept;
glibre::Result<PermutationKey> from_index(const PermutationIndex&) noexcept;

bool permutation_key_byte_less(const PermutationKey& a, const PermutationKey& b) noexcept;

// ---------------------------------------------------------------------------
// Shader stages, targets, content hash
// ---------------------------------------------------------------------------

enum class Stage : std::uint8_t {
    Vertex,
    Pixel,
    Compute,
    Mesh,
    Amplification,
    Library,
};

// MVP target is MetalLib; DXIL added post-MVP for D3D12/Windows.
enum class CompileTarget : std::uint8_t { MetalLib, DXIL };

// BLAKE3 of (preprocessed source union resolved key union canonical flags union target).
struct ShaderHash {
    eastl::array<std::byte, 32> bytes{};
    friend constexpr bool operator==(ShaderHash, ShaderHash) noexcept = default;
};

// ---------------------------------------------------------------------------
// ShaderSource value types (aggregate root §4.1)
// ---------------------------------------------------------------------------

struct SourceId {
    eastl::string project_relative_path;
    friend bool operator==(const SourceId&, const SourceId&) noexcept = default;
};

struct EntryPoint {
    eastl::string name;
    Stage stage{Stage::Vertex};
    friend bool operator==(const EntryPoint&, const EntryPoint&) noexcept = default;
};

struct IncludeNode {
    eastl::string project_relative_path;
    ShaderHash content_hash{};
};

struct PreprocessedSource {
    eastl::vector<std::byte> bytes;              // post-include byte stream
    eastl::vector<IncludeNode> include_closure;  // ordered, acyclic, project-rooted
    ShaderHash total_hash{};
};

// ---------------------------------------------------------------------------
// ShaderSource (aggregate root, §4.1)
// ---------------------------------------------------------------------------

class ShaderSource {
public:
    /// Open a Slang translation unit.
    ///
    /// project_root     — absolute path to the project source root.
    /// project_relative — path of the .slang file relative to project_root.
    ///
    /// Returns shader::Error::SourceNotFound if the file does not exist.
    /// Returns shader::Error::IncludeEscape if any resolved include lies outside
    ///   project_root or is absolute / ../-relative.
    /// Returns shader::Error::IncludeCycle if the include graph contains a cycle.
    /// Returns shader::Error::EntryPointStageAmbiguous if any function bears more
    ///   than one [shader("...")] attribute.
    ///
    /// The return type is glibre::Result<ShaderSource> = std::expected<ShaderSource,
    /// glibre::Error> per reviews/decisions/error-model.md §Decision 1.
    static glibre::Result<ShaderSource>
    open(const std::filesystem::path& project_root, const std::filesystem::path& project_relative);

    [[nodiscard]] const SourceId& id() const noexcept;
    [[nodiscard]] eastl::span<const EntryPoint> entry_points() const noexcept;
    [[nodiscard]] const PreprocessedSource& preprocessed() const noexcept;

private:
    ShaderSource() = default;

    SourceId id_{};
    eastl::vector<EntryPoint> entry_points_{};
    PreprocessedSource preprocessed_{};
};

// ---------------------------------------------------------------------------
// ReflectionBlob (value object, §4.4)  — forward-declared for completeness
// ---------------------------------------------------------------------------

enum class DescriptorFrequencyGroup : std::uint8_t {
    PerFrame,
    PerPass,
    PerMaterial,
    PerDraw,
};

enum class BindingKind : std::uint8_t {
    ConstantBuffer,
    SampledImage,
    StorageImage,
    Sampler,
    StructuredBuffer,
    RWStructuredBuffer,
    AccelerationStructure,
    PushConstant,
};

struct StageMask {
    std::uint8_t bits{0};
    friend constexpr bool operator==(StageMask, StageMask) noexcept = default;
};

struct BindingSlot {
    BindingKind kind{BindingKind::ConstantBuffer};
    std::uint32_t register_space{0};
    std::uint32_t register_index{0};
    std::uint32_t array_size{1};
    StageMask stages{};
    DescriptorFrequencyGroup frequency{DescriptorFrequencyGroup::PerDraw};
    eastl::string name;

    friend bool operator==(const BindingSlot&, const BindingSlot&) noexcept = default;
};

struct VertexInputElement {
    eastl::string semantic;
    std::uint32_t semantic_index{0};
    std::uint32_t location{0};
    std::uint32_t format_code{0};  // backend-neutral format ordinal
};

struct VertexIOLayout {
    eastl::vector<VertexInputElement> elements;
};

struct PushConstantRange {
    std::uint32_t offset{0};
    std::uint32_t size{0};
    StageMask stages{};
    friend constexpr bool operator==(PushConstantRange, PushConstantRange) noexcept = default;
};

struct MaterialParameterBlock {
    eastl::string name;
    std::uint32_t size_bytes{0};
    eastl::vector<BindingSlot> members;
};

struct SpecializationConstantSlot {
    eastl::string name;
    std::uint32_t id{0};
    std::uint32_t size_bytes{0};
};

struct ReflectionBlob {
    eastl::vector<EntryPoint> entry_points;
    eastl::vector<BindingSlot> bindings;
    VertexIOLayout vertex_io;
    eastl::vector<PushConstantRange> push_constants;
    MaterialParameterBlock material_parameters;
    eastl::vector<SpecializationConstantSlot> spec_constants;
    std::uint32_t rt_payload_bytes{0};
};

// ---------------------------------------------------------------------------
// DescriptorLayout (value object, §4.5)
// ---------------------------------------------------------------------------

struct DescriptorTable {
    // Ordered by (register_space, register_index, stage_mask) — §4.5 inv 3.
    eastl::vector<BindingSlot> slots;
    friend bool operator==(const DescriptorTable&, const DescriptorTable&) noexcept = default;
};

struct StaticSampler {
    std::uint32_t register_space{0};
    std::uint32_t register_index{0};
    StageMask stages{};
    friend constexpr bool operator==(StaticSampler, StaticSampler) noexcept = default;
};

struct RootSignatureSchema {
    DescriptorTable per_frame;
    DescriptorTable per_pass;
    DescriptorTable per_material;
    DescriptorTable per_draw;
    eastl::vector<StaticSampler> static_samplers;
    eastl::vector<PushConstantRange> push_constants;

    friend bool
    operator==(const RootSignatureSchema&, const RootSignatureSchema&) noexcept = default;
};

class DescriptorLayout {
public:
    static glibre::Result<DescriptorLayout> derive(const ReflectionBlob&, CompileTarget) noexcept;

    [[nodiscard]] const DescriptorTable& table(DescriptorFrequencyGroup g) const noexcept;
    [[nodiscard]] const RootSignatureSchema& schema() const noexcept;

private:
    DescriptorLayout() = default;
    RootSignatureSchema schema_{};
};

}  // namespace glibre::shader
