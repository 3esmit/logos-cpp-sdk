cmake_minimum_required(VERSION 3.14)
project(LogosSDK)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# logos-cpp-sdk — the Qt-FREE base C++ SDK.
#
# After the protocol extraction (logos-protocol) and the Qt split
# (logos-qt-sdk), what lives here is the standard-C++ developer surface for
# universal module implementations plus the code generator (built from
# ../cpp-generator as a separate tool):
#
#   logos_module_context.h  - LogosModuleContext (module identity/context,
#                             typed modules() accessor, onContextReady)
#   logos_result.h          - StdLogosResult (std/nlohmann result type)
#   logos_json.h            - LogosMap/LogosList aliases for impl classes
#
# Everything is header-only; the exported target is an INTERFACE library.
# Qt-typed wrappers, the legacy QObject provider path and the Qt plugin
# glue live in logos-qt-sdk. Transports, the consumer core and the lp_* C
# ABI live in logos-protocol.
# ---------------------------------------------------------------------------

find_package(nlohmann_json REQUIRED)

add_library(logos_headers INTERFACE)
target_link_libraries(logos_headers INTERFACE nlohmann_json::nlohmann_json)
target_include_directories(logos_headers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

install(TARGETS logos_headers
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
    DESTINATION include
)
