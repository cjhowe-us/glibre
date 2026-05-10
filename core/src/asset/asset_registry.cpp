// core/src/asset/asset_registry.cpp
//
// AssetRegistry translation unit.
//
// Authority: specs/core/SPEC.md §4.7, §6.8; plan #598.
//
// AssetRegistry is a template-heavy header; this TU ensures the singleton
// object is compiled into glibre-core even when no plugin yet calls
// insert<T>().  The template specialisations for concrete payload types
// are instantiated lazily by their first call sites.

#include "asset_registry.hpp"
