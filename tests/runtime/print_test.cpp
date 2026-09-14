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

} // namespace
