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
    src/domain/ast/recursive_visitor.cpp
    src/domain/parser/precedence_table.cpp
    src/domain/parser/assignability.cpp
    src/domain/parser/expression_parser.cpp
    src/domain/parser/statement_parser.cpp
    src/domain/semantic/rule_result.cpp
    src/domain/semantic/type_name.cpp
    src/domain/semantic/type_compatibility.cpp
    src/domain/semantic/literal_type.cpp
    src/domain/semantic/operator_rules.cpp
    src/domain/semantic/builtin_type_names.cpp
    src/domain/semantic/class_table.cpp
    src/domain/semantic/annotation_resolver.cpp
    src/domain/semantic/scope_stack.cpp
    src/domain/semantic/narrowing_map.cpp
    src/domain/semantic/expression_typer.cpp
    src/domain/semantic/expression_typer_calls.cpp
    src/domain/semantic/builtin_call_table.cpp
    src/domain/semantic/type_checker.cpp
    src/domain/semantic/typed_printer.cpp
    src/application/compile_pipeline.cpp
    src/adapters/filesystem/filesystem_source_reader.cpp
    src/adapters/filesystem/filesystem_source_lister.cpp
    src/adapters/filesystem/filesystem_output_writer.cpp
    src/adapters/cli/cli_adapter.cpp
    src/adapters/cli/console_diagnostics_reporter.cpp
)

add_library(cythonpp_lib STATIC ${CYTHONPP_LIB_SOURCES})
target_include_directories(cythonpp_lib PUBLIC ${CMAKE_SOURCE_DIR}/src)

# -Wswitch is on by default; this PROMOTES IT TO AN ERROR, and it is the only
# warning flag this project sets. Several places already rely on an exhaustive
# switch with no `default` label as the thing that forces a new enumerator to
# be considered -- semantic/diagnostic_kind.h says so outright for both of its
# tables, and expression_typer.cpp's RuleResult switch carries a comment
# claiming "adding a status warns here". A warning nobody is required to read
# is not a guard, so the claim those comments make is only true with this flag
# set. The blast radius was measured before adding it: the whole project,
# library and tests, compiles clean under it.
set(CYTHONPP_WARNINGS -Werror=switch)
target_compile_options(cythonpp_lib PUBLIC ${CYTHONPP_WARNINGS})

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
        tests/domain/ast/recursive_visitor_test.cpp
        tests/domain/parser/precedence_table_test.cpp
        tests/domain/parser/expression_parser_test.cpp
        tests/domain/parser/expression_parser_error_test.cpp
        tests/domain/parser/statement_parser_test.cpp
        tests/domain/parser/statement_parser_error_test.cpp
        tests/domain/semantic/type_test.cpp
        tests/domain/semantic/rule_result_test.cpp
        tests/domain/semantic/diagnostic_kind_test.cpp
        tests/domain/semantic/type_name_test.cpp
        tests/domain/semantic/type_compatibility_test.cpp
        tests/domain/semantic/join_test.cpp
        tests/domain/semantic/literal_type_test.cpp
        tests/domain/semantic/operator_rules_test.cpp
        tests/domain/semantic/builtin_type_names_test.cpp
        tests/domain/semantic/annotation_resolver_test.cpp
        tests/domain/semantic/builtin_class_table_test.cpp
        tests/domain/semantic/builtin_function_table_test.cpp
        tests/domain/semantic/class_table_test.cpp
        tests/domain/semantic/scope_stack_test.cpp
        tests/domain/semantic/narrowing_map_test.cpp
        tests/domain/semantic/expression_typer_test.cpp
        tests/domain/semantic/builtin_call_table_test.cpp
        tests/domain/semantic/type_checker_test.cpp
        tests/domain/semantic/typed_printer_test.cpp
        tests/domain/semantic/semantic_corpus_test.cpp
        tests/domain/diagnostics/diagnostic_sink_test.cpp
        tests/application/compile_pipeline_test.cpp
        tests/adapters/filesystem/filesystem_source_lister_test.cpp
        tests/adapters/cli/console_diagnostics_reporter_test.cpp
        tests/adapters/cli/cli_adapter_test.cpp
    )
    target_link_libraries(cythonpp_tests PRIVATE cythonpp_lib gtest_main)
    target_include_directories(cythonpp_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(cythonpp_tests PRIVATE
        CYTHONPP_TEST_FILES_DIR="${CMAKE_SOURCE_DIR}/test_files")

    include(GoogleTest)
    gtest_discover_tests(cythonpp_tests)
endif()
