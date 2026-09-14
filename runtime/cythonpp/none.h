#ifndef CYTHONPP_RUNTIME_NONE_H
#define CYTHONPP_RUNTIME_NONE_H

namespace py {

// A distinct unit type rather than an alias for anything else, so `print`
// overload resolution can pick the "None" spelling without ambiguity.
struct none_t {};

inline constexpr none_t none{};

inline bool truthy(none_t) { return false; }

} // namespace py

#endif // CYTHONPP_RUNTIME_NONE_H
