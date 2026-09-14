#include <memory>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "domain/ast/expr_stmt.h"
#include "domain/codegen/emitter.h"

#include "emitter_fixture.h"

namespace cythonpp::domain::codegen {
namespace {

// Emits the expression of a single trailing ExprStmt. `source` must end with
// one, e.g. "x: int = 1\nx\n".
std::optional<std::string> emit_last_expression(Fixture& fixture) {
    const ast::Stmt& last = *fixture.module->body().back();
    const auto* statement = dynamic_cast<const ast::ExprStmt*>(&last);
    EXPECT_NE(statement, nullptr) << "fixture must end in an expression statement";
    Emitter emitter(fixture.types, fixture.emit_sink);
    return emitter.emit_expression_for_test(statement->value());
}

std::optional<std::string> emitted(const std::string& source) {
    Fixture fixture = build(source);
    return emit_last_expression(fixture);
}

TEST(Emitter, IntegerLiteral) {
    EXPECT_EQ(emitted("5\n").value(), "py::int_(5)");
    EXPECT_EQ(emitted("1_000\n").value(), "py::int_(1000)");
}

TEST(Emitter, FloatLiteral) {
    EXPECT_EQ(emitted("1.5\n").value(), "py::float_(1.5)");
}

TEST(Emitter, BoolAndNoneLiterals) {
    EXPECT_EQ(emitted("True\n").value(), "py::bool_(true)");
    EXPECT_EQ(emitted("False\n").value(), "py::bool_(false)");
    EXPECT_EQ(emitted("None\n").value(), "py::none");
}

// Bytes are emitted with three-digit OCTAL escapes, never \\x: a hex escape in
// C++ is greedy, so "\\x41" followed by a literal 'B' would be read as one
// escape "\\x41B". Octal is exactly three digits and cannot run on.
TEST(Emitter, StringLiteralUsesNonGreedyOctalEscapes) {
    EXPECT_EQ(emitted("\"hi\"\n").value(), "py::str(std::string(\"\\150\\151\", 2))");
    EXPECT_EQ(emitted("\"\"\n").value(), "py::str(std::string(\"\", 0))");
}

TEST(Emitter, StringEscapesAreDecodedBeforeReEncoding) {
    // Python "a\\nb" is three bytes: 'a', newline, 'b'.
    EXPECT_EQ(emitted("\"a\\nb\"\n").value(), "py::str(std::string(\"\\141\\012\\142\", 3))");
}

TEST(Emitter, NameIsMangled) {
    EXPECT_EQ(emitted("x: int = 1\nx\n").value(), "cy_x");
}

// Refusals: each reports exactly one diagnostic and yields nullopt.
TEST(Emitter, AnUnsupportedConstructIsRefusedNotEmitted) {
    Fixture fixture = build("xs: list[int] = [1]\nxs\n");
    const std::optional<std::string> text = emit_last_expression(fixture);

    EXPECT_FALSE(text.has_value());
    ASSERT_EQ(fixture.emit_sink.diagnostics().size(), 1U);
    EXPECT_EQ(fixture.emit_sink.diagnostics().front().code, "NotImplementedError");
}

TEST(Emitter, ANonDecimalIntegerLiteralIsRefused) {
    Fixture fixture = build("0x10\n");
    EXPECT_FALSE(emit_last_expression(fixture).has_value());
    EXPECT_FALSE(fixture.emit_sink.empty());
}

TEST(Emitter, ANonAsciiIdentifierIsRefused) {
    Fixture fixture = build("caf\xc3\xa9: int = 1\ncaf\xc3\xa9\n");
    EXPECT_FALSE(emit_last_expression(fixture).has_value());
    EXPECT_FALSE(fixture.emit_sink.empty());
}

} // namespace
} // namespace cythonpp::domain::codegen
