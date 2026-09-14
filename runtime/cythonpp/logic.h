#ifndef CYTHONPP_RUNTIME_LOGIC_H
#define CYTHONPP_RUNTIME_LOGIC_H

#include "bool_.h"

namespace py {

// Python's `and` and `or` return an OPERAND, not a bool, and short-circuit.
// The right-hand side arrives as a callable so it is evaluated at most once
// and only when needed; a C++ && would both coerce to bool and discard the
// operand's value.
//
// Both operands share one type T. A program where they differ has a union
// type, which this slice's type surface does not represent, so the emitter
// refuses it before reaching here.
template <typename T, typename RhsFn>
inline T and_(T a, RhsFn rhs) {
    return truthy(a) ? rhs() : a;
}

template <typename T, typename RhsFn>
inline T or_(T a, RhsFn rhs) {
    return truthy(a) ? a : rhs();
}

template <typename T>
inline bool_ not_(const T& a) {
    return bool_(!truthy(a));
}

} // namespace py

#endif // CYTHONPP_RUNTIME_LOGIC_H
