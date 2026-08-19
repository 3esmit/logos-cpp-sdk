cmake_minimum_required(VERSION 3.14)
project(LogosSDK)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# logos-cpp-sdk — the Qt-FREE base C++ SDK.
#
# After the protocol extraction (logos-protocol) and the Qt split
# (logos-qt-sdk), what lives here is the standard-C++ developer surface, split
# by CAPABILITY. A program is some combination of three distinct things, and
# each gets its own target so a consumer takes only what it is:
#
#   ::common    logos_json.h, logos_result.h
#               The shared value types. Everything below links this.
#
#   ::consumer  logos_lp_client.h, logos_async_result.h
#               CALLING other modules. Also the compile-time home of the
#               generated <dep>_api.{h,cpp} wrappers and their logos_sdk.h
#               umbrella, which the module builder emits per build.
#
#   ::provider  logos_module_context.h, logos_host_services.h
#               IMPLEMENTING a module. LogosModuleContext is the seam the
#               generated provider injects into; logos_host_services.h is the
#               veneer a module uses for services the HOST granted it. (It is
#               module-side despite the `host_services` name, which is what
#               motivated this split; a rename to logos_privileged_services.h
#               has NOT happened — the file installed below is still
#               logos_host_services.h.)
#
#   ::host      logos_host_core.h
#               STANDING UP a core and loading modules into it. Used by
#               logos-basecamp, logos-logoscore-cli, logos-standalone-app,
#               logos-module-viewer. A module never needs this.
#
# `logos_headers` remains as an UMBRELLA over all four, so the ~70 existing
# consumers keep working unchanged; migrate to the narrow targets when touching
# a repo. Additive first, removal second.
#
# Everything is header-only, so every target is an INTERFACE library.
# Qt-typed wrappers live in logos-qt-sdk, which mirrors this same three-way
# split. Transports, the consumer core and the lp_* C ABI live in
# logos-protocol.
# ---------------------------------------------------------------------------

find_package(nlohmann_json REQUIRED)

# The include dir and nlohmann are common to every capability; each target
# below adds only its link interface, since the headers themselves are found
# through the same include path.
add_library(logos_common INTERFACE)
target_link_libraries(logos_common INTERFACE nlohmann_json::nlohmann_json)
target_include_directories(logos_common INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

add_library(logos_consumer INTERFACE)
target_link_libraries(logos_consumer INTERFACE logos_common)

add_library(logos_provider INTERFACE)
target_link_libraries(logos_provider INTERFACE logos_common)

# NOTE: ::host deliberately does NOT link liblogos. logos-liblogos depends on
# logos-cpp-sdk, so this repo cannot see its headers or its library without
# inverting the graph; logos_host_core.h declares the logos_core_* ABI itself
# and the host program provides it at link time.
add_library(logos_host INTERFACE)
target_link_libraries(logos_host INTERFACE logos_common)

# Umbrella. Pre-split consumers link this and get everything, as before.
add_library(logos_headers INTERFACE)
target_link_libraries(logos_headers INTERFACE
    logos_common
    logos_consumer
    logos_provider
    logos_host
)

install(TARGETS logos_headers logos_common logos_consumer logos_provider logos_host
    EXPORT logos-cpp-sdkTargets
    INCLUDES DESTINATION include
)

install(EXPORT logos-cpp-sdkTargets
    FILE logos-cpp-sdkTargets.cmake
    NAMESPACE logos-cpp-sdk::
    DESTINATION lib/cmake/logos-cpp-sdk
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/logos-cpp-sdkConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/logos-cpp-sdkConfig.cmake"
    INSTALL_DESTINATION lib/cmake/logos-cpp-sdk
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/logos-cpp-sdkConfigVersion.cmake"
    VERSION 0.2.0
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/logos-cpp-sdkConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/logos-cpp-sdkConfigVersion.cmake"
    DESTINATION lib/cmake/logos-cpp-sdk
)

install(FILES
    logos_module_context.h
    logos_json.h
    logos_result.h
    logos_lp_client.h
    logos_async_result.h
    logos_host_services.h
    logos_host_core.h
    DESTINATION include
)
