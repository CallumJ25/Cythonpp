#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "domain/ast/ast_printer.h"
#include "domain/ast/name.h"

namespace cythonpp::domain::ast {
namespace {

// Every node in these tests needs a span, and which span is almost never what
// the test is about, so one placeholder keeps the assertions readable.
constexpr SourceSpan kSpan{1, 1, 1, 2};

std::string print(const Node& node) { return AstPrinter().print(node); }

TEST(AstPrinter, NameRendersItsIdentifier) {
    const Name node(kSpan, "total");
    EXPECT_EQ(print(node), "(Name total)");
}

TEST(Node, CarriesTheSpanItWasBuiltWith) {
    const Name node(SourceSpan{4, 9, 4, 14}, "total");
    EXPECT_EQ(node.span(), (SourceSpan{4, 9, 4, 14}));
}

} // namespace
} // namespace cythonpp::domain::ast
