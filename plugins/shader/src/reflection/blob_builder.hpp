#pragma once
// plugins/shader/src/reflection/blob_builder.hpp
//
// BlobBuilder — projects a SlangcReflectionRecord into a canonical ReflectionBlob.
//
// Responsibility: stage 2 of the two-stage reflection ingest pipeline:
//
//   Stage 1 (slangc_reflection_record.hpp/.cpp):  JSON text → SlangcReflectionRecord
//   Stage 2 (this file):                          SlangcReflectionRecord → ReflectionBlob
//
// Canonicalization rules applied by build():
//   - Binding names are sorted lexicographically within each (register_space, register_index)
//     pair so that textually distinct but semantically-equal records produce equal blobs.
//   - Entry-point names are preserved verbatim and deduplicated: duplicate (name, stage)
//     pairs are silently coalesced into one entry; conflicting (same name, different stage)
//     pairs return ReflectionExtractionFailed.
//   - The final blob is structurally equality-comparable (§4.4 invariant 2 and 3).
//
// Authority: specs/shader/SPEC.md §4.4, §6.3.
// Plan: #514 — feat(shader): slangc reflection record ingester.

#include <memory_resource>

#include <glibre/error.hpp>
#include <glibre/shader/shader.hpp>

#include "slangc_reflection_record.hpp"

namespace glibre::shader {

// build — project rec into a canonical ReflectionBlob.
//
// rec — the parsed record produced by ingest_slangc_reflection().
// mr  — PMR memory resource for all allocations in the returned ReflectionBlob.
//       Must outlive the returned blob.
//
// Returns shader::Error::ReflectionExtractionFailed if:
//   - Any RawBinding carries RawBindingKind::Unknown.
//   - Any RawEntryPoint carries RawStage::Unknown.
//   - The same entry-point name appears with two different stages.
//
// The returned blob satisfies §4.4 invariant 2: calling build() on two
// structurally-equal SlangcReflectionRecord values yields structurally-equal
// ReflectionBlob values.
glibre::Result<ReflectionBlob>
build_reflection_blob(const SlangcReflectionRecord& rec, std::pmr::memory_resource* mr) noexcept;

}  // namespace glibre::shader
