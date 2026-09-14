#ifndef CYTHONPP_RUNTIME_STR_H
#define CYTHONPP_RUNTIME_STR_H

#include <string>
#include <utility>

#include "int_.h"

namespace py {

// Python's str over UTF-8 bytes.
//
// Correct for every operation this slice admits: concatenation, repetition
// and printing are byte-wise correct, and comparison is too, because UTF-8's
// byte-lexicographic order IS code-point order. Only len differs, and it is a
// scan. Indexing and slicing -- the operations that would force UTF-32 or an
// index -- are outside this slice, so the representation question is deferred
// rather than answered prematurely.
class str {
public:
    str() = default;
    explicit str(std::string value) : value_(std::move(value)) {}

    const std::string& raw() const { return value_; }

private:
    std::string value_;
};

inline bool truthy(const str& v) { return !v.raw().empty(); }

// Counts code points by skipping UTF-8 continuation bytes, which are exactly
// those matching 0b10xxxxxx.
inline int_ len(const str& v) {
    std::int64_t count = 0;
    for (const char byte : v.raw()) {
        if ((static_cast<unsigned char>(byte) & 0xC0) != 0x80) {
            count += 1;
        }
    }
    return int_(count);
}

inline str add(const str& a, const str& b) { return str(a.raw() + b.raw()); }

inline str mul(const str& a, int_ count) {
    std::string result;
    for (std::int64_t i = 0; i < count.raw(); ++i) {
        result += a.raw();
    }
    return str(std::move(result));
}

inline str mul(int_ count, const str& a) { return mul(a, count); }

} // namespace py

#endif // CYTHONPP_RUNTIME_STR_H
