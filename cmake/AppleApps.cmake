# Drives xcodegen + xcodebuild from CMake so a single
# `cmake --build --preset macos-{debug,release}` invocation produces
# Glibre.xcodeproj plus the GlibreMacOS.app and GlibreIOS.app bundles.
#
# Only loaded on host-macOS preset (CMAKE_SYSTEM_NAME=Darwin). The
# iOS / iOS-sim presets target CMAKE_SYSTEM_NAME=iOS and skip this file
# (preventing recursion when the macOS-host build kicks off iOS sub-builds).

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    return()
endif()

option(GLIBRE_BUILD_APPLE_APPS
       "Run xcodegen + xcodebuild after the host build to produce \
        GlibreMacOS.app + GlibreIOS.app bundles."
       ON)

if(NOT GLIBRE_BUILD_APPLE_APPS)
    return()
endif()

find_program(XCODEGEN_EXECUTABLE  xcodegen   REQUIRED)
find_program(XCODEBUILD_EXECUTABLE xcodebuild REQUIRED)

# Map host CMAKE_BUILD_TYPE → xcodebuild config + sub-preset suffix.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_xc_config "Debug")
    set(_sub_suffix "debug")
else()
    set(_xc_config "Release")
    set(_sub_suffix "release")
endif()

# 1. iOS device + simulator static-lib sub-builds.
#    Recursive cmake invocations reuse our presets; their CMAKE_SYSTEM_NAME=iOS
#    makes this file early-return there, so no infinite recursion.
#
#    TODO: iOS deployment is post-MVP; cross-build infra needs work.
#    EXCLUDE_FROM_ALL prevents CI gate failures from platform toolchain issues.
add_custom_target(glibre_ios_libs EXCLUDE_FROM_ALL
    COMMAND ${CMAKE_COMMAND} --preset ios-${_sub_suffix}
    COMMAND ${CMAKE_COMMAND} --build --preset ios-${_sub_suffix}
    COMMAND ${CMAKE_COMMAND} --preset ios-sim-${_sub_suffix}
    COMMAND ${CMAKE_COMMAND} --build --preset ios-sim-${_sub_suffix}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    COMMENT "Building iOS device + simulator static libs"
    VERBATIM
)

# 2. xcodegen — regenerate Glibre.xcodeproj whenever project.yml changes.
set(_xcodeproj ${CMAKE_SOURCE_DIR}/Glibre.xcodeproj/project.pbxproj)
add_custom_command(
    OUTPUT ${_xcodeproj}
    COMMAND ${XCODEGEN_EXECUTABLE} generate --spec ${CMAKE_SOURCE_DIR}/project.yml
    DEPENDS ${CMAKE_SOURCE_DIR}/project.yml
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "xcodegen generate Glibre.xcodeproj"
    VERBATIM
)
add_custom_target(glibre_xcodeproj DEPENDS ${_xcodeproj})

# 3. xcodebuild — produce .app bundles. CODE_SIGNING_ALLOWED=NO so plain
#    `cmake --build` works without a developer signing identity; pass
#    -DGLIBRE_APPLE_SIGN=ON (and DEVELOPMENT_TEAM via env) for signed builds.
set(_sign_args CODE_SIGNING_ALLOWED=NO)
if(GLIBRE_APPLE_SIGN)
    set(_sign_args)
endif()

add_custom_target(glibre_macos_app ALL
    COMMAND ${XCODEBUILD_EXECUTABLE}
        -project   ${CMAKE_SOURCE_DIR}/Glibre.xcodeproj
        -scheme    GlibreMacOS
        -configuration ${_xc_config}
        -destination "generic/platform=macOS"
        ${_sign_args}
        build
    DEPENDS glibre_xcodeproj
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    COMMENT "xcodebuild → GlibreMacOS.app"
    VERBATIM
)

add_custom_target(glibre_ios_app EXCLUDE_FROM_ALL
    COMMAND ${XCODEBUILD_EXECUTABLE}
        -project   ${CMAKE_SOURCE_DIR}/Glibre.xcodeproj
        -scheme    GlibreIOS
        -configuration ${_xc_config}
        -destination "generic/platform=iOS"
        ${_sign_args}
        build
    DEPENDS glibre_xcodeproj glibre_ios_libs
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    COMMENT "xcodebuild → GlibreIOS.app"
    VERBATIM
)
