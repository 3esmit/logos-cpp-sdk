# Std-only tests for the Qt-free base SDK headers. The Qt-typed suites
# (LogosAPI, provider, auth, events, async, worker-thread IPC) moved to
# logos-qt-sdk with the code they exercise; the transport/component suites
# moved to logos-protocol.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../cpp ${CMAKE_CURRENT_BINARY_DIR}/logos_headers)

add_executable(sdk_tests
    test_logos_module_context.cpp
    test_logos_host_services.cpp
    test_logos_host_core.cpp
)

# logos_host_services.h is a veneer over the lp_* C ABI, so this suite needs
# logos-protocol's HEADERS. It deliberately does not need its LIBRARY: the
# functions that call lp_* are `inline` and never ODR-used by these tests, so
# nothing references a protocol symbol at link time. If a future test does call
# one, this will fail to LINK rather than silently pull the library in.
# LOGOS_PROTOCOL_ROOT is the logos-protocol SOURCE tree here (nix/tests.nix
# passes the flake input, not the built package), so the headers are under
# cpp/ — a package would put them under include/. Accept either rather than
# hard-coding the layout that happens to be in use today.
if(EXISTS "${LOGOS_PROTOCOL_ROOT}/cpp/logos_protocol.h")
    set(LOGOS_PROTOCOL_INCLUDE "${LOGOS_PROTOCOL_ROOT}/cpp")
elseif(EXISTS "${LOGOS_PROTOCOL_ROOT}/include/logos_protocol.h")
    set(LOGOS_PROTOCOL_INCLUDE "${LOGOS_PROTOCOL_ROOT}/include")
else()
    message(FATAL_ERROR
        "logos_protocol.h not found under LOGOS_PROTOCOL_ROOT=${LOGOS_PROTOCOL_ROOT} "
        "(looked in cpp/ and include/). sdk_tests compiles logos_host_services.h, "
        "which is a veneer over the lp_* C ABI.")
endif()
target_include_directories(sdk_tests PRIVATE "${LOGOS_PROTOCOL_INCLUDE}")

target_link_libraries(sdk_tests PRIVATE
    logos_headers
    GTest::gtest
    GTest::gtest_main
)


gtest_discover_tests(sdk_tests)
