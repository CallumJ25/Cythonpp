cmake_minimum_required(VERSION 3.16)
project(cythonpp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CYTHONPP_LIB_SOURCES
    src/domain/lexer/token.cpp
    src/domain/lexer/token_stream.cpp
    src/domain/lexer/token_type_name.cpp
    src/domain/lexer/keyword_table.cpp
    src/domain/lexer/operator_table.cpp
    src/domain/lexer/scan_context.cpp
    src/domain/lexer/lexer.cpp
    src/domain/lexer/indentation_pass.cpp
    src/domain/diagnostics/diagnostic_sink.cpp
    src/domain/ast/ast_printer.cpp
    src/domain/parser/precedence_table.cpp
    src/application/compile_pipeline.cpp
    src/adapters/filesystem/filesystem_source_reader.cpp
    src/adapters/filesystem/filesystem_source_lister.cpp
    src/adapters/filesystem/filesystem_output_writer.cpp
    src/adapters/cli/cli_adapter.cpp
    src/adapters/cli/console_diagnostics_reporter.cpp
)

add_library(cythonpp_lib STATIC ${CYTHONPP_LIB_SOURCES})
target_include_directories(cythonpp_lib PUBLIC ${CMAKE_SOURCE_DIR}/src)

add_executable(cythonpp src/main.cpp)
target_link_libraries(cythonpp PRIVATE cythonpp_lib)

option(CYTHONPP_BUILD_TESTS "Build cythonpp unit tests" ON)
if(CYTHONPP_BUILD_TESTS)
    enable_testing()

    include(FetchContent)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(cythonpp_tests
        tests/domain/lexer/token_test.cpp
        tests/domain/lexer/token_stream_test.cpp
        tests/domain/lexer/keyword_table_test.cpp
        tests/domain/lexer/operator_table_test.cpp
        tests/domain/lexer/scan_context_test.cpp
        tests/domain/lexer/lexer_test.cpp
        tests/domain/lexer/lexer_annotation_test.cpp
        tests/domain/lexer/indentation_pass_test.cpp
        tests/domain/ast/source_span_test.cpp
        tests/domain/ast/ast_printer_test.cpp
        tests/domain/parser/precedence_table_test.cpp
        tests/domain/diagnostics/diagnostic_sink_test.cpp
        tests/application/compile_pipeline_test.cpp
        tests/adapters/filesystem/filesystem_source_lister_test.cpp
        tests/adapters/cli/console_diagnostics_reporter_test.cpp
    )
    target_link_libraries(cythonpp_tests PRIVATE cythonpp_lib gtest_main)
    target_include_directories(cythonpp_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

    include(GoogleTest)
    gtest_discover_tests(cythonpp_tests)
endif()
