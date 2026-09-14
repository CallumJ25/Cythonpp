#include <gtest/gtest.h>

#include "domain/codegen/name_mangler.h"

namespace cythonpp::domain::codegen {
namespace {

// Every C++ keyword listed here is a LEGAL Python identifier. A program using
// one is ordinary Python and must not produce C++ that fails to compile.
TEST(NameMangler, CppKeywordsSurviveMangling) {
    EXPECT_EQ(mangle("class"), "cy_class");
    EXPECT_EQ(mangle("new"), "cy_new");
    EXPECT_EQ(mangle("delete"), "cy_delete");
    EXPECT_EQ(mangle("template"), "cy_template");
    EXPECT_EQ(mangle("this"), "cy_this");
    EXPECT_EQ(mangle("operator"), "cy_operator");
}

TEST(NameMangler, OrdinaryNamesAreMangledToo) {
    EXPECT_EQ(mangle("x"), "cy_x");
    EXPECT_EQ(mangle("total_count"), "cy_total_count");
}

// The scheme must be INJECTIVE: two distinct Python names must never produce
// one C++ name, because a collision is a silent miscompile. Prefixing is
// injective by construction -- Python "cy_x" becomes "cy_cy_x".
TEST(NameMangler, ManglingIsInjective) {
    EXPECT_NE(mangle("cy_x"), mangle("x"));
    EXPECT_EQ(mangle("cy_x"), "cy_cy_x");
}

// Non-ASCII identifiers are legal Python and the lexer preserves them, but
// C++'s rules for them differ enough that emitting one is a guess. Refused
// rather than guessed.
TEST(NameMangler, NonAsciiIdentifiersAreNotManglable) {
    EXPECT_TRUE(is_manglable_identifier("ok_name"));
    EXPECT_TRUE(is_manglable_identifier("_leading"));
    EXPECT_TRUE(is_manglable_identifier("n1"));
    EXPECT_FALSE(is_manglable_identifier("caf\xc3\xa9"));
    EXPECT_FALSE(is_manglable_identifier(""));
}

} // namespace
} // namespace cythonpp::domain::codegen
