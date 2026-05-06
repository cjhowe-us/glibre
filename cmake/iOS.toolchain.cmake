# iOS toolchain for Ninja generator.
# Sets CMAKE_SYSTEM_* + bitcode/sysroot defaults so first-pass `try_compile`
# is wired correctly. CMAKE_OSX_SYSROOT / CMAKE_OSX_DEPLOYMENT_TARGET /
# CMAKE_OSX_ARCHITECTURES are supplied by the preset.

set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_SYSTEM_PROCESSOR arm64)

# Bitcode is removed in Xcode 14+; off by default.
set(CMAKE_XCODE_ATTRIBUTE_ENABLE_BITCODE NO)

# Universal-binary fat-archive disabled — we ship per-sysroot Ninja outputs
# and let XcodeGen / xcodebuild assemble the .xcframework.
set(CMAKE_IOS_INSTALL_COMBINED OFF)

# `try_compile` defaults to STATIC_LIBRARY so it doesn't require a host
# loader / signing identity at configure-time.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
