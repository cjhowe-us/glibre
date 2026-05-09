#pragma once
// plugins/shader/include/glibre/shader/shader_source.hpp
//
// ShaderSource aggregate root — §4.1 of specs/shader/SPEC.md.
//
// Re-exports the ShaderSource class and related value types from shader.hpp
// so callers that only care about source-level operations can include this
// focused header instead of the full shader.hpp.
//
// SRP: "Validate and present an authored Slang translation unit."

#include <glibre/shader/shader.hpp>
