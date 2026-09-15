#include <string>

#include <gtest/gtest.h>

#include "cythonpp/cythonpp.h"

namespace {

// print writes to std::cout, so the test captures it rather than inspecting a
// returned string -- the emitted program's real behaviour is what matters.
std::string captured(void (*body)()) {
    testing::internal::CaptureStdout();
    body();
    return testing::internal::GetCapturedStdout();
}

TEST(RuntimePrint, BoolAndNoneUsePythonSpelling) {
    EXPECT_EQ(captured([] { py::print(py::bool_(true)); }), "True\n");
    EXPECT_EQ(captured([] { py::print(py::bool_(false)); }), "False\n");
    EXPECT_EQ(captured([] { py::print(py::none); }), "None\n");
}

TEST(RuntimePrint, FloatUsesReprNotStreamDefault) {
    EXPECT_EQ(captured([] { py::print(py::float_(1.0)); }), "1.0\n");
}

// Measured: print(1, 2) writes "1 2" and print() writes an empty line.
TEST(RuntimePrint, ArgumentsAreSpaceJoinedAndZeroArgsIsABlankLine) {
    EXPECT_EQ(captured([] { py::print(py::int_(1), py::int_(2)); }), "1 2\n");
    EXPECT_EQ(captured([] { py::print(); }), "\n");
}

TEST(RuntimePrint, StrPrintsItsContentsUnquoted) {
    EXPECT_EQ(captured([] { py::print(py::str("hi")); }), "hi\n");
}

// TASK 10B, obligation 1: print renders by the RUNTIME tag, not the static
// C++ type -- `x: float = 1; print(x)` must print `1`, not `1.0`, and
// `def f(x: bool) -> int: return x` must print `True`/`False`, not `1`/`0`.
TEST(RuntimePrint, RendersByRuntimeTagNotStaticType) {
    EXPECT_EQ(captured([] { py::print(py::to_float(py::int_(1))); }), "1\n");
    EXPECT_EQ(captured([] { py::print(py::to_int(py::bool_(true))); }), "True\n");
    EXPECT_EQ(captured([] { py::print(py::to_int(py::bool_(false))); }), "False\n");
    EXPECT_EQ(captured([] { py::print(py::to_float(py::bool_(true))); }), "True\n");
    // A genuine float still prints via the shortest-round-trip repr.
    EXPECT_EQ(captured([] { py::print(py::float_(1.0)); }), "1.0\n");
}

} // namespace
