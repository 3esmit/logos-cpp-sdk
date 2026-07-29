# Std-only tests for the Qt-free base SDK headers. The Qt-typed suites
# (LogosAPI, provider, auth, events, async, worker-thread IPC) moved to
# logos-qt-sdk with the code they exercise; the transport/component suites
# moved to logos-protocol.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../cpp ${CMAKE_CURRENT_BINARY_DIR}/logos_headers)

add_executable(sdk_tests
    test_logos_module_context.cpp
)

target_link_libraries(sdk_tests PRIVATE
    logos_headers
    GTest::gtest
    GTest::gtest_main
)


gtest_discover_tests(sdk_tests)
