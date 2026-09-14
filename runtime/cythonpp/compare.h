#ifndef CYTHONPP_RUNTIME_COMPARE_H
#define CYTHONPP_RUNTIME_COMPARE_H

#include "bool_.h"
#include "float_.h"
#include "int_.h"
#include "str.h"

namespace py {

// Comparison yields Python's bool, not C++'s, so a comparison result prints
// as True/False and composes with the rest of the model.
//
// Defined as templates over the runtime's own wrapper types: each has raw(),
// and the underlying C++ comparison is correct for all of them -- including
// str, per the byte-order property documented in str.h.
template <typename A, typename B>
inline bool_ lt(const A& a, const B& b) { return bool_(a.raw() < b.raw()); }
template <typename A, typename B>
inline bool_ le(const A& a, const B& b) { return bool_(a.raw() <= b.raw()); }
template <typename A, typename B>
inline bool_ gt(const A& a, const B& b) { return bool_(a.raw() > b.raw()); }
template <typename A, typename B>
inline bool_ ge(const A& a, const B& b) { return bool_(a.raw() >= b.raw()); }
template <typename A, typename B>
inline bool_ eq(const A& a, const B& b) { return bool_(a.raw() == b.raw()); }
template <typename A, typename B>
inline bool_ ne(const A& a, const B& b) { return bool_(a.raw() != b.raw()); }

} // namespace py

#endif // CYTHONPP_RUNTIME_COMPARE_H
