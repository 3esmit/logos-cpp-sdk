add_executable(generator_tests
    ${CMAKE_CURRENT_SOURCE_DIR}/../../cpp-generator/legacy/generator_lib.cpp
    test_to_pascal_case.cpp
    test_normalize_type.cpp
    test_map_param_type.cpp
    test_map_return_type.cpp
    test_to_qvariant_conversion.cpp
    test_make_header.cpp
    test_make_source.cpp
    test_parse_provider_header.cpp
    test_records.cpp
)

target_include_directories(generator_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../cpp-generator
    ${CMAKE_CURRENT_SOURCE_DIR}/../../cpp-generator/legacy
    ${CMAKE_CURRENT_SOURCE_DIR}/../../cpp
)

target_link_libraries(generator_tests PRIVATE
    GTest::gtest_main
    Qt${QT_VERSION_MAJOR}::Core
)

gtest_discover_tests(generator_tests)
