#ifndef CYTHONPP_RUNTIME_NUMERIC_TAG_H
#define CYTHONPP_RUNTIME_NUMERIC_TAG_H

namespace py {

// Python's numeric tower is bool <: int <: float: a variable declared at one
// rank may hold a value of any lower rank, and behaves at runtime as
// whatever it actually holds -- `x: float = 1` leaves `x` holding the INT
// `1`, not the float `1.0`, because an annotation constrains what a name may
// hold, it does not coerce the value.
//
// This tag is how int_ and float_ record that fact: the C++ STATIC type
// (bool_/int_/float_) is the DECLARED type, chosen by the emitter from the
// checker's own type, while this tag is the ACTUAL rank of the value a
// wider runtime type is currently holding. int_ only ever uses Bool/Int;
// float_ uses all three. See int_.h's and float_.h's own comments for how
// each stores the value behind the tag without losing precision.
enum class NumericTag { Bool, Int, Float };

} // namespace py

#endif // CYTHONPP_RUNTIME_NUMERIC_TAG_H
