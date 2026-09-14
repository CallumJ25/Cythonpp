#include <gtest/gtest.h>

#include "cythonpp/compare.h"
#include "cythonpp/str.h"

namespace {

// Python's len counts CODE POINTS, not bytes. Measured: len("héllo") is 5,
// though the UTF-8 encoding is 6 bytes.
TEST(RuntimeStr, LenCountsCodePointsNotBytes) {
    EXPECT_EQ(py::len(py::str("hello")).raw(), 5);
    EXPECT_EQ(py::len(py::str("h\xc3\xa9llo")).raw(), 5);
    EXPECT_EQ(py::len(py::str("")).raw(), 0);
}

TEST(RuntimeStr, ConcatAndRepeat) {
    EXPECT_EQ(py::add(py::str("a"), py::str("b")).raw(), "ab");
    EXPECT_EQ(py::mul(py::str("a"), py::int_(3)).raw(), "aaa");
    EXPECT_EQ(py::mul(py::int_(3), py::str("a")).raw(), "aaa");
    EXPECT_EQ(py::mul(py::str("a"), py::int_(0)).raw(), "");
    EXPECT_EQ(py::mul(py::str("a"), py::int_(-1)).raw(), "");
}

// UTF-8's byte-lexicographic order IS code-point order, so byte comparison is
// correct rather than merely convenient.
TEST(RuntimeStr, ComparisonIsByteWiseAndThatIsCorrect) {
    EXPECT_TRUE(py::lt(py::str("a"), py::str("b")).raw());
    EXPECT_FALSE(py::lt(py::str("b"), py::str("a")).raw());
    EXPECT_TRUE(py::eq(py::str("a"), py::str("a")).raw());
}

TEST(RuntimeStr, Truthiness) {
    EXPECT_TRUE(py::truthy(py::str("a")));
    EXPECT_FALSE(py::truthy(py::str("")));
}

} // namespace
