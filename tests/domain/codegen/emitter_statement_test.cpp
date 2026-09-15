#include <memory>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "domain/codegen/emitter.h"

#include "emitter_fixture.h"

namespace cythonpp::domain::codegen {
namespace {

// Emits the FIRST statement of the module in isolation.
std::optional<std::string> emitted(Fixture& fixture) {
    Emitter emitter(fixture.types, fixture.emit_sink);
    return emitter.emit_statement_for_test(*fixture.module->body().front());
}

std::optional<std::string> emitted(const std::string& source) {
    Fixture fixture = build(source);
    return emitted(fixture);
}

TEST(EmitterStatement, ExpressionStatement) {
    EXPECT_EQ(emitted("print(1)\n").value(), "py::print(py::int_(1));\n");
}

TEST(EmitterStatement, PassEmitsNothingExecutable) {
    EXPECT_EQ(emitted("pass\n").value(), ";\n");
}

// At module level the declaration lives in the file-scope prelude, so the
// statement itself is a plain assignment.
TEST(EmitterStatement, ModuleLevelAssignmentIsAssignmentNotDeclaration) {
    EXPECT_EQ(emitted("x: int = 1\n").value(), "cy_x = py::int_(1);\n");
    EXPECT_EQ(emitted("x = 1\n").value(), "cy_x = py::int_(1);\n");
}

TEST(EmitterStatement, IfElse) {
    EXPECT_EQ(emitted("if True:\n    print(1)\nelse:\n    print(2)\n").value(),
              "if (py::truthy(py::bool_(true))) {\n"
              "  py::print(py::int_(1));\n"
              "} else {\n"
              "  py::print(py::int_(2));\n"
              "}\n");
}

TEST(EmitterStatement, IfWithNoElseEmitsNoElseBlock) {
    EXPECT_EQ(emitted("if True:\n    print(1)\n").value(),
              "if (py::truthy(py::bool_(true))) {\n"
              "  py::print(py::int_(1));\n"
              "}\n");
}

// No else clause means no break flag: the simplest loop stays simple.
TEST(EmitterStatement, WhileWithoutElseEmitsNoBreakFlag) {
    const std::string text = emitted("while True:\n    break\n").value();

    EXPECT_EQ(text.find("_cy_broke"), std::string::npos);
    EXPECT_NE(text.find("while (py::truthy(py::bool_(true)))"), std::string::npos);
    EXPECT_NE(text.find("break;"), std::string::npos);
}

// Python runs a loop's else ONLY when the loop was not exited by break, so
// the flag is what makes the else correct rather than unconditional.
TEST(EmitterStatement, WhileWithElseTracksBreakWithAFlag) {
    const std::string text = emitted("while True:\n    break\nelse:\n    print(1)\n").value();

    EXPECT_NE(text.find("bool _cy_broke_0 = false;"), std::string::npos);
    EXPECT_NE(text.find("_cy_broke_0 = true; break;"), std::string::npos);
    EXPECT_NE(text.find("if (!_cy_broke_0)"), std::string::npos);
}

// HOLE (A)'s own regression case: an inner loop with NO else, nested inside
// an outer loop that HAS one, must still emit a bare `break;` for its own
// break -- loop_has_else_ has to be saved/restored around the outer loop's
// body, or the inner break would either reference a flag that does not exist
// at its depth or (worse) wrongly set the OUTER loop's flag.
TEST(EmitterStatement, NestedLoopWithoutElseInsideOneWithElseEmitsBareBreak) {
    const std::string text =
        emitted("while True:\n"
                "    while True:\n"
                "        break\n"
                "    print(1)\n"
                "else:\n"
                "    print(2)\n")
            .value();

    EXPECT_NE(text.find("bool _cy_broke_0 = false;"), std::string::npos);
    // The INNER loop's break is bare: no _cy_broke_1 is ever declared, and
    // the inner break must not touch _cy_broke_0 either.
    EXPECT_EQ(text.find("_cy_broke_1"), std::string::npos);
    EXPECT_EQ(text.find("_cy_broke_0 = true"), std::string::npos);
    EXPECT_NE(text.find("break;"), std::string::npos);
}

TEST(EmitterStatement, FunctionDefinition) {
    EXPECT_EQ(emitted("def f(a: int) -> int:\n    return a\n").value(),
              "py::int_ cy_f(py::int_ cy_a) {\n"
              "  return cy_a;\n"
              "}\n");
}

// A function-local variable IS declared at its first assignment, unlike a
// module-level one.
TEST(EmitterStatement, AFunctionLocalIsDeclaredAtFirstAssignmentAndAssignedAfter) {
    EXPECT_EQ(emitted("def f() -> None:\n    x: int = 1\n    x = 2\n").value(),
              "py::none_t cy_f() {\n"
              "  py::int_ cy_x = py::int_(1);\n"
              "  cy_x = py::int_(2);\n"
              "}\n");
}

TEST(EmitterStatement, BareReturn) {
    EXPECT_EQ(emitted("def f() -> None:\n    return\n").value(),
              "py::none_t cy_f() {\n"
              "  return py::none;\n"
              "}\n");
}

// HOLE (B)'s own regression case: the declared C++ type must come from the
// ANNOTATION, not from the value's own inferred type. `x: float = 1` binds a
// float-declared local from an int LITERAL -- mypy-clean, since int widens to
// float -- so the declaration must read `py::float_`, not `py::int_` (the
// literal's own type). Getting this wrong doesn't just mistype the variable:
// a later `x = 2.5` (also mypy-clean, since 2.5 is compatible with the
// DECLARED type float) would then try to assign a py::float_ onto a variable
// declared py::int_, which does not compile at all (int_ has no operator=
// taking a float_).
TEST(EmitterStatement, AnnotatedLocalDeclaresTheAnnotationsTypeNotTheValues) {
    EXPECT_EQ(emitted("def f() -> None:\n    x: float = 1\n    x = 2.5\n").value(),
              "py::none_t cy_f() {\n"
              "  py::float_ cy_x = py::int_(1);\n"
              "  cy_x = py::float_(2.5);\n"
              "}\n");
}

// A bare `x: int` (no value) declares nothing executable here -- Python binds
// nothing either -- but must not crash resolving an annotation with no value
// alongside it.
TEST(EmitterStatement, BareAnnotationEmitsNothingExecutable) {
    EXPECT_EQ(emitted("def f() -> None:\n    x: int\n").value(),
              "py::none_t cy_f() {\n"
              "  ;\n"
              "}\n");
}

TEST(EmitterStatement, ConstructsOutsideTheSliceAreRefused) {
    for (const std::string source : {std::string("for x in [1]:\n    pass\n"),
                                     std::string("class K:\n    pass\n")}) {
        Fixture fixture = build(source);
        EXPECT_FALSE(emitted(fixture).has_value()) << source;
        EXPECT_FALSE(fixture.emit_sink.empty()) << source;
    }
}

} // namespace
} // namespace cythonpp::domain::codegen
