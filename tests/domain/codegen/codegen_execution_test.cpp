#include <string>

#include <gtest/gtest.h>

#include "domain/codegen/emitter.h"
#include "compile_and_run.h"

namespace cythonpp::domain::codegen {
namespace {

// Expected stdout below was produced by RUNNING CPython 3.14, not by
// reasoning about what it should print. Task 12's script re-derives it.
TEST(CodegenExecution, ArithmeticAndPrint) {
    const RunResult result = compile_and_run("print(1 + 2)\nprint(7 / 2)\n");

    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(result.stdout_text, "3\n3.5\n");
}

// The corrections of spec Decision 4, end to end rather than only at unit
// level: C++ would print -3 and -1 here.
TEST(CodegenExecution, FloorDivisionAndModuloMatchPythonNotCpp) {
    const RunResult result = compile_and_run("print(-7 // 2)\nprint(-7 % 2)\n");

    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(result.stdout_text, "-4\n1\n");
}

// C++'s operator<< prints 1.0 as "1" and True as "1".
TEST(CodegenExecution, FloatAndBoolAndNoneUsePythonSpelling) {
    const RunResult result = compile_and_run("print(1.0)\nprint(True)\nprint(None)\n");

    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(result.stdout_text, "1.0\nTrue\nNone\n");
}

TEST(CodegenExecution, FunctionsRecursionAndControlFlow) {
    const RunResult result = compile_and_run(
        "def fib(n: int) -> int:\n"
        "    if n < 2:\n"
        "        return n\n"
        "    return fib(n - 1) + fib(n - 2)\n"
        "\n"
        "\n"
        "print(fib(10))\n");

    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(result.stdout_text, "55\n");
}

// A C++ keyword used as a Python identifier. Ordinary Python; must not
// produce C++ that fails to compile.
TEST(CodegenExecution, ACppKeywordAsAnIdentifierCompilesAndRuns) {
    const RunResult result = compile_and_run(
        "def template(class_: int) -> int:\n"
        "    new: int = class_ + 1\n"
        "    return new\n"
        "\n"
        "\n"
        "print(template(1))\n");

    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(result.stdout_text, "2\n");
}

// The exact defect class this file exists to catch. A `-> None` function
// whose body does not end in an explicit `return` falls off the end of a
// non-void C++ function -- undefined behaviour, measured on this project's
// own clang++/-O0 as a guaranteed SIGILL trap -- unless visit(FunctionDef)
// appends its own trailing `return py::none;`. None of the other samples in
// this file exercise that path (fib/template both return int, and the rest
// have no functions at all), so this one exists specifically to make sure a
// regression here is caught by RUNNING the emitted program, not merely by
// reading its text.
TEST(CodegenExecution, ANoneFunctionThatFallsOffTheEndActuallyRuns) {
    const RunResult result = compile_and_run(
        "def announce(n: int) -> None:\n"
        "    print(n)\n"
        "\n"
        "\n"
        "announce(1)\n"
        "announce(2)\n");

    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(result.stdout_text, "1\n2\n");
}

// FINAL-REVIEW CRITICAL 1, end to end. Every expected line below was produced
// by RUNNING CPython 3.14. std::to_chars' format-less form picks fixed vs
// scientific by shortest STRING; CPython picks by decimal EXPONENT, so each of
// these printed the wrong text while every test in the suite stayed green --
// the compiled program exited 0 and simply said something else. This is the
// only test in the file that would have caught it, because the defect is in
// what the program PRINTS, not in whether it compiles.
TEST(CodegenExecution, FloatReprMatchesCPythonAcrossTheFixedScientificThresholds) {
    const RunResult result = compile_and_run("print(100000.0)\n"
                                             "print(1000000.0)\n"
                                             "print(20000000.0)\n"
                                             "print(1000000000000000.0)\n"
                                             "print(0.0001)\n"
                                             "print(123456789012345678.0)\n"
                                             "print(1e16)\n"
                                             "print(0.00001)\n");

    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(result.stdout_text, "100000.0\n"
                                  "1000000.0\n"
                                  "20000000.0\n"
                                  "1000000000000000.0\n"
                                  "0.0001\n"
                                  "1.2345678901234568e+17\n"
                                  "1e+16\n"
                                  "1e-05\n");
}

// FINAL-REVIEW CRITICAL 2. `bool <: int <: float` is a real subtyping
// relationship mypy enforces, so both calls below are mypy-clean and
// CPython-clean -- but py::bool_ has no implicit conversion to py::int_, so
// the unwidened `cy_f(py::bool_(true))` this used to emit was
// `no matching function for call to 'cy_f'`. A text test would have asserted
// the wrong string happily; only compiling catches it.
TEST(CodegenExecution, CallArgumentsAreWidenedIntoTheParametersDeclaredType) {
    const RunResult result = compile_and_run("def f(x: int) -> int:\n"
                                             "    return x\n"
                                             "\n"
                                             "\n"
                                             "def g(y: float) -> float:\n"
                                             "    return y\n"
                                             "\n"
                                             "\n"
                                             "print(f(True))\n"
                                             "print(g(2))\n"
                                             "print(g(True))\n");

    EXPECT_EQ(result.exit_code, 0);
    // An annotation CONSTRAINS, it does not COERCE, so the values print as
    // the bool and int they actually are -- measured under CPython.
    EXPECT_EQ(result.stdout_text, "True\n2\nTrue\n");
}

// FINAL-REVIEW CRITICAL 3, at both scopes and in both directions. Python
// scoping is per-function/per-module, not per-block, so a name first assigned
// inside an `if` is an ordinary local of the enclosing scope; declaring it
// inside the emitted C++ block left every other reference to it an undeclared
// identifier. These two shapes both assign on EVERY path, so both compile and
// run; the shapes that do not are refused, which
// EmitterStatement.AVariableAssignedOnlyInOneBranchIsRefusedRatherThanDefaulted
// pins.
TEST(CodegenExecution, AVariableFirstAssignedInsideABlockHasScopeNotBlockScope) {
    const RunResult module_level = compile_and_run("x: int = 1\n"
                                                   "if x > 0:\n"
                                                   "    y: int = 2\n"
                                                   "else:\n"
                                                   "    y = 3\n"
                                                   "print(y)\n");
    EXPECT_EQ(module_level.exit_code, 0);
    EXPECT_EQ(module_level.stdout_text, "2\n");

    const RunResult function_level = compile_and_run("def g(c: bool) -> None:\n"
                                                     "    if c:\n"
                                                     "        x: int = 1\n"
                                                     "    else:\n"
                                                     "        x = 2\n"
                                                     "    print(x)\n"
                                                     "\n"
                                                     "\n"
                                                     "g(True)\n"
                                                     "g(False)\n");
    EXPECT_EQ(function_level.exit_code, 0);
    EXPECT_EQ(function_level.stdout_text, "1\n2\n");
}

// FINAL-REVIEW IMPORTANT 5: a valueless `x: float` inside a function used to
// record no declared type, so `x = 1` declared an int slot and `x = 2.5`
// after it had no viable operator=. The same program at module level always
// compiled, which is what made it an inconsistency rather than a plain gap --
// so both halves are run here.
TEST(CodegenExecution, AValuelessAnnotationDeclaresTheSameTypeAtBothScopes) {
    const RunResult function_level = compile_and_run("def f() -> None:\n"
                                                     "    x: float\n"
                                                     "    x = 1\n"
                                                     "    x = 2.5\n"
                                                     "    print(x)\n"
                                                     "\n"
                                                     "\n"
                                                     "f()\n");
    EXPECT_EQ(function_level.exit_code, 0);
    EXPECT_EQ(function_level.stdout_text, "2.5\n");

    const RunResult module_level = compile_and_run("x: float\nx = 1\nx = 2.5\nprint(x)\n");
    EXPECT_EQ(module_level.exit_code, 0);
    EXPECT_EQ(module_level.stdout_text, "2.5\n");
}

// Overflow exits non-zero rather than wrapping. Nothing reaches stdout.
TEST(CodegenExecution, OverflowExitsNonZeroWithNoStdout) {
    const RunResult result = compile_and_run("x: int = 9223372036854775807\nprint(x + 1)\n");

    EXPECT_NE(result.exit_code, 0);
    EXPECT_EQ(result.stdout_text, "");
}

// POST-WAVE CRITICAL, the ACCEPTING half. The cross-boundary refusal that
// closes the "a function reads a global the module may never assign" hole
// (emitter_module_test.cpp pins the refusals) must not over-refuse the
// ordinary case, and only actually RUNNING these proves it -- a text
// assertion would pass for a program that does not compile, which is how
// five separate defects reached review on this project. Every expected
// stdout below was produced by RUNNING CPython 3.14.
TEST(CodegenExecution, AFunctionReadingADefinitelyAssignedGlobalStillRunsCorrectly) {
    // Unconditional assignment at module level.
    const RunResult unconditional =
        compile_and_run("s: str = \"cfg\"\n\n\n"
                        "def label() -> str:\n    return s + \"!\"\n\n\nprint(label())\n");
    EXPECT_EQ(unconditional.exit_code, 0);
    EXPECT_EQ(unconditional.stdout_text, "cfg!\n");

    // Assigned in BOTH arms, so the intersection binds it.
    const RunResult both_arms =
        compile_and_run("c: bool = False\nif c:\n    s: str = \"cfg\"\nelse:\n    s = \"alt\"\n\n\n"
                        "def label() -> str:\n    return s + \"!\"\n\n\nprint(label())\n");
    EXPECT_EQ(both_arms.exit_code, 0);
    EXPECT_EQ(both_arms.stdout_text, "alt!\n");

    // Assigned before the `if` and merely reassigned inside it.
    const RunResult reassigned =
        compile_and_run("c: bool = True\ns: str = \"base\"\nif c:\n    s = \"cfg\"\n\n\n"
                        "def label() -> str:\n    return s + \"!\"\n\n\nprint(label())\n");
    EXPECT_EQ(reassigned.exit_code, 0);
    EXPECT_EQ(reassigned.stdout_text, "cfg!\n");

    // A PARAMETER shadowing a global the module may never assign: the
    // parameter wins, and the global is never read at all.
    const RunResult shadowed =
        compile_and_run("c: bool = False\nif c:\n    s: str = \"cfg\"\n\n\n"
                        "def label(s: str) -> str:\n    return s + \"!\"\n\n\n"
                        "print(label(\"arg\"))\n");
    EXPECT_EQ(shadowed.exit_code, 0);
    EXPECT_EQ(shadowed.stdout_text, "arg!\n");

    // A LOCAL of the same name, assigned before its own read: likewise the
    // function's own, so the cross-boundary check must leave it alone and
    // the ordinary in-scope check must find it bound.
    const RunResult own_local =
        compile_and_run("c: bool = False\nif c:\n    s: str = \"cfg\"\n\n\n"
                        "def label() -> str:\n    s = \"own\"\n    return s + \"!\"\n\n\n"
                        "print(label())\n");
    EXPECT_EQ(own_local.exit_code, 0);
    EXPECT_EQ(own_local.stdout_text, "own!\n");
}

// Spec Decision 2: the representation stays swappable only while the emitter
// names the type and calls its operations. A convention nobody checks is not
// a guarantee, so this inspects the emitted text directly.
TEST(CodegenExecution, EmittedTextNeverSpellsInt64OrBareArithmetic) {
    Fixture fixture = build("a: int = 1\nb: int = 2\nprint(a + b * a - b)\n");
    Emitter emitter(fixture.types, fixture.emit_sink);
    const std::string text = emitter.emit_module(*fixture.module).value();

    EXPECT_EQ(text.find("int64_t"), std::string::npos);
    EXPECT_EQ(text.find("cy_a + "), std::string::npos);
    EXPECT_EQ(text.find("cy_a * "), std::string::npos);
    EXPECT_EQ(text.find("cy_a - "), std::string::npos);
}

// 2026-09-16, THE ACCEPTING HALF of the block-first-assignment fix. The
// declared type of a module-level name now comes from its FIRST assignment
// wherever that sits, in the type checker as well as in codegen's hoisted
// declaration -- the two disagreeing is what let a `py::float_` be assigned
// into a `py::int_` slot. These four are programs BOTH oracles accept, so the
// new refusals must not touch them, and only RUNNING them proves the declared
// slot, the hoist and the widening all agree. Every expected stdout was
// produced by RUNNING CPython 3.14.
TEST(CodegenExecution, ABlockFirstAssignmentReassignedAtTopLevelStillRunsCorrectly) {
    // The `if` body fixes `int`; a later top-level `int` needs no widening.
    const RunResult plain = compile_and_run("c: bool = True\nif c:\n    x = 1\nx = 2\nprint(x)\n");
    EXPECT_EQ(plain.exit_code, 0);
    EXPECT_EQ(plain.stdout_text, "2\n");

    // The `if` body's ANNOTATION fixes `float`; the later top-level `int`
    // widens into it -- and prints `2`, not `2.0`, because widening preserves
    // the value's own Int tag (see numeric_tag.h).
    const RunResult annotated =
        compile_and_run("c: bool = True\nif c:\n    x: float = 1.0\nx = 2\nprint(x)\n");
    EXPECT_EQ(annotated.exit_code, 0);
    EXPECT_EQ(annotated.stdout_text, "2\n");

    // The `if` body's VALUE fixes `float`, with no annotation anywhere.
    const RunResult inferred =
        compile_and_run("c: bool = True\nif c:\n    x = 1.5\nx = 2\nprint(x)\n");
    EXPECT_EQ(inferred.exit_code, 0);
    EXPECT_EQ(inferred.stdout_text, "2\n");

    // A `while` body that never runs: the declaration is still hoisted from
    // it, and the top-level assignment is what the program actually prints.
    const RunResult loop =
        compile_and_run("c: bool = False\nwhile c:\n    n = 1\n    c = False\nn = 7\nprint(n)\n");
    EXPECT_EQ(loop.exit_code, 0);
    EXPECT_EQ(loop.stdout_text, "7\n");
}

// ADVERSARIAL REVIEW, 2026-09-16, the two unary-on-bool CRITICALs. Python's
// bool is an int subtype, so `-True` is -1 and `+True` is 1 -- an INT in both
// cases. Every shape below is mypy `Success` and CPython exit 0, and every
// expected string was produced by RUNNING CPython 3.14.
//
// These COMPILE AND RUN rather than asserting on text, and that is the whole
// point: the pre-existing text test (Emitter.UnaryOperators) asserts
// "py::neg(py::int_(1))" for `-1` and passed throughout, because it uses an
// int operand only. With a BOOL operand, OP_MINUS emitted
// py::neg(py::bool_(true)) -- clang++ `no matching function for call to
// 'neg'` from a cythonpp run that exited 0 with the file WRITTEN -- and
// OP_PLUS emitted the operand unchanged, compiling cleanly and printing
// `True` where CPython prints `1`. The eighth instance of the process lesson
// in CLAUDE.md: a text assertion cannot tell you the text compiles, let
// alone runs.
TEST(CodegenExecution, UnaryOperatorsOnABoolOperandRunAsPythonDoes) {
    // OP_MINUS, the uncompilable one, in each of the three value positions
    // plus print (which is widening-exempt and so reached by no other guard).
    const RunResult minus_init = compile_and_run("x: int = -True\nprint(x)\n");
    EXPECT_EQ(minus_init.exit_code, 0);
    EXPECT_EQ(minus_init.stdout_text, "-1\n");

    const RunResult minus_print = compile_and_run("b: bool = True\nprint(-b)\n");
    EXPECT_EQ(minus_print.exit_code, 0);
    EXPECT_EQ(minus_print.stdout_text, "-1\n");

    const RunResult minus_return =
        compile_and_run("def f() -> int:\n    return -True\n\n\nprint(f())\n");
    EXPECT_EQ(minus_return.exit_code, 0);
    EXPECT_EQ(minus_return.stdout_text, "-1\n");

    const RunResult minus_arg =
        compile_and_run("def f(v: int) -> int:\n    return v\n\n\nprint(f(-True))\n");
    EXPECT_EQ(minus_arg.exit_code, 0);
    EXPECT_EQ(minus_arg.stdout_text, "-1\n");

    // OP_PLUS, the silently-wrong-output one. `1`, not `True` -- and this is
    // what proves py::pos sheds the Bool tag, since py::to_int deliberately
    // PRESERVES it (numeric_tag.h), so a widening-only fix would compile and
    // still print `True` here.
    const RunResult plus_print = compile_and_run("b: bool = True\nprint(+b)\n");
    EXPECT_EQ(plus_print.exit_code, 0);
    EXPECT_EQ(plus_print.stdout_text, "1\n");

    const RunResult plus_init = compile_and_run("x: int = +True\nprint(x)\n");
    EXPECT_EQ(plus_init.exit_code, 0);
    EXPECT_EQ(plus_init.stdout_text, "1\n");

    // The bool has to shed its tag before becoming an operand of something
    // else, too: `+True + 0` is 1. Pins the interaction with emit_binary's
    // own widening, which reads the UnaryOp node's type (int) and so declines
    // to widen -- leaving the shedding entirely to this arm.
    const RunResult plus_operand = compile_and_run("print(+True + 0)\n");
    EXPECT_EQ(plus_operand.exit_code, 0);
    EXPECT_EQ(plus_operand.stdout_text, "1\n");

    // A float-declared slot: widening composes with the tag shedding, and the
    // value still prints as an INT, because to_float preserves whatever tag
    // reaches it and neg/pos have already made that Int. CPython: -1.
    const RunResult widened = compile_and_run("x: float = -True\nprint(x)\n");
    EXPECT_EQ(widened.exit_code, 0);
    EXPECT_EQ(widened.stdout_text, "-1\n");
}

// CONTROLS for the arm above: a non-bool operand must be untouched by the
// widening it added. Every expected string produced by RUNNING CPython 3.14.
TEST(CodegenExecution, UnaryOperatorsOnNonBoolOperandsAreUnchanged) {
    const RunResult ints =
        compile_and_run("a: int = 7\nprint(-a)\nprint(+a)\nprint(-(-a))\n");
    EXPECT_EQ(ints.exit_code, 0);
    EXPECT_EQ(ints.stdout_text, "-7\n7\n7\n");

    // repr spells the sign, so a float round-trips visibly -- and an integral
    // value held in a float slot must stay integral rather than being routed
    // through double (int_ reaches 2^63, a double is exact only to 2^53).
    const RunResult floats = compile_and_run(
        "a: float = 2.5\nb: float = 4.0\nprint(-a)\nprint(+a)\nprint(-b)\n");
    EXPECT_EQ(floats.exit_code, 0);
    EXPECT_EQ(floats.stdout_text, "-2.5\n2.5\n-4.0\n");

    // `not` is deliberately NOT routed through the widening: its result IS a
    // bool, so shedding would be wrong. CPython: False, True.
    const RunResult logical = compile_and_run("b: bool = True\nprint(not b)\nprint(not 0)\n");
    EXPECT_EQ(logical.exit_code, 0);
    EXPECT_EQ(logical.stdout_text, "False\nTrue\n");
}

} // namespace
} // namespace cythonpp::domain::codegen
