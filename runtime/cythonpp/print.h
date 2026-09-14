#ifndef CYTHONPP_RUNTIME_PRINT_H
#define CYTHONPP_RUNTIME_PRINT_H

#include <iostream>

#include "bool_.h"
#include "float_.h"
#include "int_.h"
#include "none.h"
#include "str.h"

namespace py {

// One overload per printable type. Bool and None need Python's spelling, and
// float needs repr rather than operator<<'s default, which prints 1.0 as "1".
inline void print_one(std::ostream& out, int_ v) { out << v.raw(); }
inline void print_one(std::ostream& out, bool_ v) { out << (v.raw() ? "True" : "False"); }
inline void print_one(std::ostream& out, float_ v) { out << repr(v); }
inline void print_one(std::ostream& out, none_t) { out << "None"; }
inline void print_one(std::ostream& out, const str& v) { out << v.raw(); }

// Measured: print(1, 2) writes "1 2" and print() writes an empty line.
template <typename... Ts>
inline void print(const Ts&... values) {
    bool first = true;
    const auto emit = [&](const auto& value) {
        if (!first) {
            std::cout << ' ';
        }
        first = false;
        print_one(std::cout, value);
    };
    (emit(values), ...);
    std::cout << '\n';
}

} // namespace py

#endif // CYTHONPP_RUNTIME_PRINT_H
