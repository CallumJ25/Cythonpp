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

// MINOR, flagged by review round 1: an outer-level `break` that comes AFTER
// a nested inner loop has already fully closed must still set the OUTER
// loop's flag -- loop_has_else_ is restored to the outer loop's own value
// once the inner loop's body finishes emitting, so this break (which is
// textually inside the outer loop's body but outside the inner loop
// entirely) sees loop_has_else_ == true again, same as
// NestedLoopWithoutElseInsideOneWithElseEmitsBareBreak's inner break sees it
// restored to false once its own inner loop's turn is done. Exact text
// asserted (rather than substring checks) since the whole shape is small
// enough to pin precisely.
TEST(EmitterStatement, OuterBreakAfterAClosedInnerLoopStillSetsTheOuterFlag) {
    EXPECT_EQ(emitted("while True:\n"
                       "    while True:\n"
                       "        break\n"
                       "    break\n"
                       "else:\n"
                       "    print(1)\n")
                  .value(),
              "{\n"
              "  bool _cy_broke_0 = false;\n"
              "  while (py::truthy(py::bool_(true))) {\n"
              "    while (py::truthy(py::bool_(true))) {\n"
              "      break;\n"
              "    }\n"
              "    _cy_broke_0 = true; break;\n"
              "  }\n"
              "  if (!_cy_broke_0) {\n"
              "    py::print(py::int_(1));\n"
              "  }\n"
              "}\n");
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
              "  return py::none;\n"
              "}\n");
}

// A body already ending in a bare `return` still gets the trailing
// `return py::none;` appended (see CRITICAL FIX, post-review round 2, on
// visit(FunctionDef) below) -- the second one is unreachable, which is legal
// C++ with no diagnostic, and the alternative (skipping it here) would need
// a reachability model this stage does not otherwise have.
TEST(EmitterStatement, BareReturn) {
    EXPECT_EQ(emitted("def f() -> None:\n    return\n").value(),
              "py::none_t cy_f() {\n"
              "  return py::none;\n"
              "  return py::none;\n"
              "}\n");
}

// HOLE (B)'s own regression case: the declared C++ type must come from the
// ANNOTATION, not from the value's own inferred type. `x: float = 1` binds a
// float-declared local from an int LITERAL -- mypy-clean, since int widens to
// float -- so the declaration must read `py::float_`, not `py::int_` (the
// literal's own type).
//
// FIX (post-review round 1): getting the DECLARED type right isn't the whole
// fix -- the value `1` is still typed Int by the checker, and Int has no
// implicit conversion to Float (see runtime/cythonpp/int_.h's own comment),
// so the initializer itself must be explicitly widened: `py::to_float(...)`,
// not a bare `py::int_(1)` handed to a `py::float_` variable. This test used
// to assert the latter, which is invalid C++ -- see this file's clang++
// verification in the round-1 fix report for the compile check that caught
// it. The second line (`x = 2.5`, a plain reassignment) still needs no
// widening of its own -- 2.5 is already Float -- but IS the reassignment
// case that needs the DECLARED type remembered rather than re-derived from
// this value: see the dedicated reassignment test below for the case where
// that distinction actually bites.
TEST(EmitterStatement, AnnotatedLocalDeclaresTheAnnotationsTypeNotTheValues) {
    EXPECT_EQ(emitted("def f() -> None:\n    x: float = 1\n    x = 2.5\n").value(),
              "py::none_t cy_f() {\n"
              "  py::float_ cy_x = py::to_float(py::int_(1));\n"
              "  cy_x = py::float_(2.5);\n"
              "  return py::none;\n"
              "}\n");
}

// CRITICAL FIX (post-review round 1), the reassignment half specifically:
// `function_declared_` must remember the DECLARED type (Float, from the
// annotation), not merely THAT the name was declared, so a later plain
// reassignment of a LOWER-ranked value (`2`, Int) widens against the
// variable's real C++ type rather than declaring how `2` alone would type.
// `x = 2` here reuses `cy_x`'s existing `py::float_` slot, so the value must
// be `py::to_float(py::int_(2))`, not a bare `py::int_(2)` -- which, again,
// does not compile against a `py::float_` variable.
TEST(EmitterStatement, ReassigningALowerRankedValueToAnAlreadyDeclaredLocalWidensIt) {
    EXPECT_EQ(emitted("def f() -> None:\n    x: float = 1.0\n    x = 2\n").value(),
              "py::none_t cy_f() {\n"
              "  py::float_ cy_x = py::float_(1.0);\n"
              "  cy_x = py::to_float(py::int_(2));\n"
              "  return py::none;\n"
              "}\n");
}

// CRITICAL FIX (post-review round 1), the return half: a function returning
// a WIDER type than one of its parameters must widen the returned value the
// same way an assignment's initializer does. `bool <: int` is a real
// subtyping relationship is_subtype enforces, so this is mypy-clean, but
// `return cy_x;` alone would hand back a bare py::bool_ where py::int_ is
// declared -- which does not compile.
TEST(EmitterStatement, ReturnWidensABoolParameterToTheDeclaredIntReturnType) {
    EXPECT_EQ(emitted("def f(x: bool) -> int:\n    return x\n").value(),
              "py::int_ cy_f(py::bool_ cy_x) {\n"
              "  return py::to_int(cy_x);\n"
              "}\n");
}

// IMPORTANT FIX (post-review round 1): standard C++ has no nested function
// definitions at all, not even as a Clang extension, while Python's are
// fully supported upstream -- so a nested `def`, mypy-clean and
// CPython-clean, must be refused by name rather than silently emitted as
// invalid syntax (or, worse, silently dropped).
TEST(EmitterStatement, NestedFunctionDefinitionIsRefused) {
    Fixture fixture =
        build("def outer() -> None:\n    def inner() -> None:\n        pass\n");
    EXPECT_FALSE(emitted(fixture).has_value());
    EXPECT_FALSE(fixture.emit_sink.empty());
}

// A bare `x: int` (no value) declares nothing executable here -- Python binds
// nothing either -- but must not crash resolving an annotation with no value
// alongside it.
TEST(EmitterStatement, BareAnnotationEmitsNothingExecutable) {
    EXPECT_EQ(emitted("def f() -> None:\n    x: int\n").value(),
              "py::none_t cy_f() {\n"
              "  ;\n"
              "  return py::none;\n"
              "}\n");
}

// CRITICAL FIX (post-review round 2): a `-> None` function is the one return
// kind mypy never requires an explicit return on every path for, so its body
// can legitimately end in anything -- a bare expression statement, an `if`
// with no `else`, a loop -- with no `return` anywhere at all. Before this
// fix, visit(FunctionDef) emitted nothing to compensate, so the generated
// C++ function fell off the end: undefined behaviour that the re-reviewer
// measured as a guaranteed `ud2`/SIGILL trap on this project's own
// clang++/-O0 for exactly this shape. See this file's round-2 fix report for
// the compile-AND-RUN verification (a text-only test cannot detect a crash).
// These three pin the three shapes the reviewer asked for explicitly.
TEST(EmitterStatement, NoneFunctionEndingInAnExpressionStatementGetsATrailingReturn) {
    EXPECT_EQ(emitted("def f() -> None:\n    print(1)\n").value(),
              "py::none_t cy_f() {\n"
              "  py::print(py::int_(1));\n"
              "  return py::none;\n"
              "}\n");
}

TEST(EmitterStatement, NoneFunctionEndingInAnIfWithNoElseGetsATrailingReturn) {
    EXPECT_EQ(emitted("def f() -> None:\n    if True:\n        print(1)\n").value(),
              "py::none_t cy_f() {\n"
              "  if (py::truthy(py::bool_(true))) {\n"
              "    py::print(py::int_(1));\n"
              "  }\n"
              "  return py::none;\n"
              "}\n");
}

TEST(EmitterStatement, NoneFunctionEndingInAWhileLoopGetsATrailingReturn) {
    EXPECT_EQ(emitted("def f() -> None:\n    while True:\n        break\n").value(),
              "py::none_t cy_f() {\n"
              "  while (py::truthy(py::bool_(true))) {\n"
              "    break;\n"
              "  }\n"
              "  return py::none;\n"
              "}\n");
}

// A non-None return type never reaches this point without a guaranteed
// return on every path -- TypeChecker's own missing-return check
// (type_checker.cpp, end of visit(FunctionDef)) reports unless
// `return_type.kind` is NoneType or Unknown, and Unknown always carries its
// own diagnostic (every Type::unknown() return in AnnotationResolver traces
// back to an error() call), which keeps such a module out of both
// Fixture::build and the real pipeline. So `-> int`/`-> bool`/`-> float`/
// `-> str` are deliberately NOT given a trailing-return safety net: control
// pinning that FunctionDefinition's existing single-`return`-statement output
// is unaffected by this fix.
TEST(EmitterStatement, ANonNoneReturnTypeGetsNoTrailingReturnInjected) {
    EXPECT_EQ(emitted("def f(a: int) -> int:\n    return a\n").value(),
              "py::int_ cy_f(py::int_ cy_a) {\n"
              "  return cy_a;\n"
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
