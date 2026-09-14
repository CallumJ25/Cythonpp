#ifndef CYTHONPP_RUNTIME_BOOL_H
#define CYTHONPP_RUNTIME_BOOL_H

namespace py {

// Wrapped rather than a bare `bool` so that `print` can render it as `True`
// and `False` rather than `1` and `0`, which is what an unwrapped bool would
// give through operator<<.
class bool_ {
public:
    constexpr bool_() = default;
    constexpr explicit bool_(bool value) : value_(value) {}

    constexpr bool raw() const { return value_; }

private:
    bool value_ = false;
};

inline bool truthy(bool_ v) { return v.raw(); }

} // namespace py

#endif // CYTHONPP_RUNTIME_BOOL_H
