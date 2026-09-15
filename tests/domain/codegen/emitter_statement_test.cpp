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

// FINAL-REVIEW CRITICAL 3: a function-local is declared at the top of the
// FUNCTION, not at its first assignment, so every assignment to it is a plain
// assignment -- matching Python's own per-function scoping. This test used to
// assert the opposite ("declared at first assignment"), which is exactly the
// behaviour that gave a variable first assigned inside an `if` C++ BLOCK
// scope and left every read of it outside that block an undeclared
// identifier.
TEST(EmitterStatement, AFunctionLocalIsDeclaredAtTheTopOfTheFunctionNotAtFirstAssignment) {
    EXPECT_EQ(emitted("def f() -> None:\n    x: int = 1\n    x = 2\n").value(),
              "py::none_t cy_f() {\n"
              "  py::int_ cy_x;\n"
              "  cy_x = py::int_(1);\n"
              "  cy_x = py::int_(2);\n"
              "  return py::none;\n"
              "}\n");
}

// The shape CRITICAL 3 was reported for, at function scope: `x` is assigned
// in BOTH arms of an `if` and read after it. The declaration must sit above
// the `if`, or the else-arm assignment and the read both reference a name
// that went out of scope with the then-arm's closing brace.
TEST(EmitterStatement, AVariableAssignedInBothBranchesIsDeclaredAboveTheIf) {
    EXPECT_EQ(emitted("def f(c: bool) -> None:\n"
                      "    if c:\n"
                      "        x: int = 1\n"
                      "    else:\n"
                      "        x = 2\n"
                      "    print(x)\n")
                  .value(),
              "py::none_t cy_f(py::bool_ cy_c) {\n"
              "  py::int_ cy_x;\n"
              "  if (py::truthy(cy_c)) {\n"
              "    cy_x = py::int_(1);\n"
              "  } else {\n"
              "    cy_x = py::int_(2);\n"
              "  }\n"
              "  py::print(cy_x);\n"
              "  return py::none;\n"
              "}\n");
}

// A name assigned in a loop body is hoisted the same way -- and, unlike the
// branch case above, a loop body may run zero times, so a name FIRST assigned
// there and read afterwards is refused rather than silently read as a
// default-constructed 0. See the refusal tests below.
TEST(EmitterStatement, AVariableAssignedInsideALoopIsDeclaredAboveIt) {
    EXPECT_EQ(emitted("def f(c: bool) -> None:\n"
                      "    x: int = 0\n"
                      "    while c:\n"
                      "        x = 1\n"
                      "        break\n"
                      "    print(x)\n")
                  .value(),
              "py::none_t cy_f(py::bool_ cy_c) {\n"
              "  py::int_ cy_x;\n"
              "  cy_x = py::int_(0);\n"
              "  while (py::truthy(cy_c)) {\n"
              "    cy_x = py::int_(1);\n"
              "    break;\n"
              "  }\n"
              "  py::print(cy_x);\n"
              "  return py::none;\n"
              "}\n");
}

// FINAL-REVIEW CRITICAL 3, the decision half. A hoisted declaration
// DEFAULT-CONSTRUCTS, so a name assigned only in a branch that does not run
// would read as 0 in C++ where Python raises UnboundLocalError -- silently
// wrong output on a program CPython REJECTS. The definite-assignment check
// refuses the shape instead, which keeps Decision 0 intact: emitted
// correctly, or refused by name, never a third state. Only FALSE refusals are
// possible; the check never concludes "bound" where Python would not have.
TEST(EmitterStatement, AVariableAssignedOnlyInOneBranchIsRefusedRatherThanDefaulted) {
    for (const std::string source :
         {std::string("def f(c: bool) -> None:\n"
                      "    if c:\n"
                      "        x: int = 1\n"
                      "    print(x)\n"),
          // A loop body may run zero times, so it binds nothing definitely.
          std::string("def f(c: bool) -> None:\n"
                      "    while c:\n"
                      "        x: int = 1\n"
                      "    print(x)\n"),
          // The read is in the OTHER arm, so the binding never precedes it.
          std::string("def f(c: bool) -> None:\n"
                      "    if c:\n"
                      "        x: int = 1\n"
                      "    else:\n"
                      "        print(x)\n"),
          // Nested one level deeper: only the inner branch binds.
          std::string("def f(c: bool) -> None:\n"
                      "    if c:\n"
                      "        if c:\n"
                      "            x: int = 1\n"
                      "    print(x)\n")}) {
        Fixture fixture = build(source);
        EXPECT_FALSE(emitted(fixture).has_value()) << source;
        EXPECT_FALSE(fixture.emit_sink.empty()) << source;
    }
}

// The controls for the check above: each of these IS definitely assigned, and
// must keep emitting. A parameter is bound on entry (so `x = x + 1` reads the
// parameter, not an unassigned local); an arm that always LEAVES contributes
// nothing to intersect, so the other arm's binding stands alone; and a read
// inside the very branch that binds is fine.
TEST(EmitterStatement, DefinitelyAssignedShapesAreNotRefused) {
    for (const std::string source :
         {std::string("def f(x: int) -> int:\n    x = x + 1\n    return x\n"),
          std::string("def f(c: bool) -> int:\n"
                      "    if c:\n"
                      "        x: int = 1\n"
                      "    else:\n"
                      "        return 0\n"
                      "    return x\n"),
          std::string("def f(c: bool) -> None:\n"
                      "    if c:\n"
                      "        x: int = 1\n"
                      "        print(x)\n"),
          std::string("def f(c: bool) -> None:\n"
                      "    x: int = 0\n"
                      "    while c:\n"
                      "        print(x)\n"
                      "        x = x + 1\n")}) {
        Fixture fixture = build(source);
        EXPECT_TRUE(emitted(fixture).has_value()) << source;
        EXPECT_TRUE(fixture.emit_sink.empty()) << source;
    }
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
              "  py::float_ cy_x;\n"
              "  cy_x = py::to_float(py::int_(1));\n"
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
              "  py::float_ cy_x;\n"
              "  cy_x = py::float_(1.0);\n"
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

// FINAL-REVIEW IMPORTANT 4: at_module_level_ stays TRUE inside a module-level
// `if`/`while` body (that body is emitted into main()), so the nested-def
// refusal above never fired for a CONDITIONAL def and C++ -- which has no
// function definition inside a block at all -- rejected the result, with the
// forward-declaration pass missing it too. Both a module-level `if` and a
// module-level `while` reach this; a def inside a FUNCTION's `if` is caught
// by the nested-def rule first.
TEST(EmitterStatement, ConditionalFunctionDefinitionIsRefused) {
    for (const std::string source :
         {std::string("if True:\n    def h() -> int:\n        return 1\n"),
          std::string("while True:\n    def h() -> int:\n        return 1\n"),
          std::string("if True:\n    pass\nelse:\n    def h() -> int:\n        return 1\n")}) {
        Fixture fixture = build(source);
        Emitter emitter(fixture.types, fixture.emit_sink);
        EXPECT_FALSE(emitter.emit_module(*fixture.module).has_value()) << source;
        EXPECT_FALSE(fixture.emit_sink.empty()) << source;
    }
}

// The control for the refusal above: it must be narrow enough that an
// ordinary top-level `def` written near a block is untouched. (It does NOT
// pin in_conditional_block_'s restore-on-every-path: emit_module emits every
// definition in Step 5, BEFORE main() carries any module-level block in Step
// 6, so a leaked flag could not reach a def anyway -- verified by neutering
// the restore and watching all 86 codegen tests stay green. The restore is
// written on every path for correctness, not because a test catches it.)
TEST(EmitterStatement, ADefinitionAfterABlockIsNotRefused) {
    for (const std::string source :
         {std::string("n: int = 0\nwhile n < 1:\n    n = n + 1\n\n\n"
                      "def h() -> int:\n    return 1\n\n\nprint(h())\n"),
          std::string("n: int = 0\nwhile n < 1:\n    n = n + 1\nelse:\n    n = 9\n\n\n"
                      "def h() -> int:\n    return 1\n\n\nprint(h())\n"),
          std::string("if True:\n    pass\n\n\ndef h() -> int:\n    return 1\n\n\nprint(h())\n"),
          std::string("if True:\n    pass\nelse:\n    pass\n\n\n"
                      "def h() -> int:\n    return 1\n\n\nprint(h())\n")}) {
        Fixture fixture = build(source);
        Emitter emitter(fixture.types, fixture.emit_sink);
        EXPECT_TRUE(emitter.emit_module(*fixture.module).has_value()) << source;
        EXPECT_TRUE(fixture.emit_sink.empty()) << source;
    }
}

// A bare `x: int` (no value) emits nothing executable -- Python binds nothing
// either -- but it DOES declare, and FINAL-REVIEW IMPORTANT 5 is that a
// function body used to record nothing for it at all while the module prelude
// always did. So the declaration now appears in the function's own prologue,
// carrying the ANNOTATION's type, and the statement itself is still a lone
// `;`. Without the declaration, the next test's program does not compile.
TEST(EmitterStatement, BareAnnotationDeclaresButEmitsNothingExecutable) {
    EXPECT_EQ(emitted("def f() -> None:\n    x: int\n").value(),
              "py::none_t cy_f() {\n"
              "  py::int_ cy_x;\n"
              "  ;\n"
              "  return py::none;\n"
              "}\n");
}

// FINAL-REVIEW IMPORTANT 5, the shape it was reported for: a valueless
// `x: float` must make the later `x = 1` widen into a FLOAT slot. Before the
// fix the function recorded no declared type at all, so `x = 1` declared
// `py::int_ cy_x` from the VALUE and the `x = 2.5` after it had no viable
// `operator=`. The identical program at module level always worked, which is
// what made this a module-vs-function inconsistency rather than a plain gap.
TEST(EmitterStatement, AValuelessAnnotationInAFunctionRecordsItsDeclaredType) {
    EXPECT_EQ(emitted("def f() -> None:\n    x: float\n    x = 1\n    x = 2.5\n").value(),
              "py::none_t cy_f() {\n"
              "  py::float_ cy_x;\n"
              "  ;\n"
              "  cy_x = py::to_float(py::int_(1));\n"
              "  cy_x = py::float_(2.5);\n"
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
