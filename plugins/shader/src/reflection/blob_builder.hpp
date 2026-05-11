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
//   - Entry-point names are preserved verbatim and deduplicated: duplicate (name, stage)
//     pairs are silently coalesced into one entry; conflicting (same name, different stage)
//     pairs return ReflectionExtractionFailed.
//   - The final blob is structurally equality-comparable (§4.4 invariant 2).
//
// Authority: specs/shader/SPEC.md §4.4, §6.3.
// Plan: #514 — feat(shader): slangc reflection record ingester.

#include <memory_resource>

#include <glibre/error.hpp>
#include <glibre/shader/shader.hpp>

#include "slangc_reflection_record.hpp"

namespace glibre::shader {

// build_reflection_blob — project rec into a canonical ReflectionBlob.
//
// rec — the parsed record produced by ingest_slangc_reflection().
// mr  — PMR memory resource for all allocations in the returned ReflectionBlob.
//       Must outlive the returned blob.
//
// Precondition: rec must have been produced by ingest_slangc_reflection(); in
// particular, no RawBinding may carry RawBindingKind::Unknown and no
// RawEntryPoint may carry RawStage::Unknown — the parser enforces this and
// reaching either Unknown arm in the translator is a programming error that
// triggers std::unreachable() (not a recoverable Result failure).
//
// Returns shader::Error::ReflectionExtractionFailed if:
//   - The same entry-point name appears with two different stages.
//
// The returned blob satisfies §4.4 invariant 2: calling build_reflection_blob()
// on two structurally-equal SlangcReflectionRecord values yields structurally-equal
// ReflectionBlob values.
//
// WARNING — §4.4 invariant 3 is NOT satisfied by this function alone.
// Every BindingSlot in the returned blob carries
// DescriptorFrequencyGroup::PerDraw as a placeholder default.  Invariant 3
// requires every binding to be annotated with exactly one resolved frequency
// group; that obligation belongs to the frequency_tagger pass (specs/shader/
// SPEC.md §6.3, tracked in #515).  Callers MUST NOT pass a blob produced
// solely by build_reflection_blob() to DescriptorLayout::derive() or any
// consumer that assumes invariant 3 holds — pass it through frequency_tagger
// first.
glibre::Result<ReflectionBlob>
build_reflection_blob(const SlangcReflectionRecord& rec, std::pmr::memory_resource* mr) noexcept;

}  // namespace glibre::shader
