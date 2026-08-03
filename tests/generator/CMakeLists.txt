# logos-lidl: test_optional_spellings runs the LEGACY path end to end — LIDL
# text -> the JSON surface (lidl_to_json) -> the emitter — because the rule it
# asserts (the two optionality spellings emit identical code) is a property of
# that composition and cannot be checked on either half alone.
find_package(logos-lidl REQUIRED)

add_executable(generator_tests
    ${CMAKE_CURRENT_SOURCE_DIR}/../../cpp-generator/legacy/generator_lib.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../cpp-generator/legacy/lidl_to_json.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../cpp-generator/experimental/lidl_emit_common.cpp
    test_to_pascal_case.cpp
    test_normalize_type.cpp
    test_map_param_type.cpp
    test_map_return_type.cpp
    test_to_qvariant_conversion.cpp
    test_make_header.cpp
    test_make_source.cpp
    test_make_umbrella.cpp
    test_parse_provider_header.cpp
    test_records.cpp
    test_async_result.cpp
    test_optional_spellings.cpp
)

target_include_directories(generator_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../cpp-generator
    ${CMAKE_CURRENT_SOURCE_DIR}/../../cpp-generator/legacy
    ${CMAKE_CURRENT_SOURCE_DIR}/../../cpp-generator/experimental
    ${CMAKE_CURRENT_SOURCE_DIR}/../../cpp
)

target_link_libraries(generator_tests PRIVATE
    GTest::gtest_main
    Qt${QT_VERSION_MAJOR}::Core
    logos-lidl::logos_lidl
)

gtest_discover_tests(generator_tests)
