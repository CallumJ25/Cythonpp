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

} // namespace
} // namespace cythonpp::domain::codegen
