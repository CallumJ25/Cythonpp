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
