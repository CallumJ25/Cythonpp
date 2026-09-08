#include "builtin_call_table.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "operator_rules.h"
#include "rule_result.h"
#include "type_compatibility.h"
#include "type_kind.h"

namespace cythonpp::domain::semantic {
namespace {

// Bool counts as an integer, matching operator_rules.cpp's own is_integral --
// Python's bool is an int subtype. Unknown ALSO passes here, deliberately:
// an Unknown argument is the residue of an already-reported failure
// elsewhere (an unbound name, say), and treating it as "fine" is what keeps
// that one root cause to one diagnostic instead of drawing a second, false
// "wrong argument type" report on top of it.
bool accepts_integral(TypeKind kind) {
    return kind == TypeKind::Unknown || kind == TypeKind::Bool || kind == TypeKind::Int;
}

// Every builtin container kind `len` accepts, plus Unknown for the same
// absorbing reason as accepts_integral. No typeshed is consulted, so a user
// class's __len__ is out of scope here -- ExpressionTyper's Attribute arm
// already carries that limitation for member access in general.
bool accepts_len(TypeKind kind) {
    return kind == TypeKind::Unknown || kind == TypeKind::List || kind == TypeKind::Dict ||
           kind == TypeKind::Set || kind == TypeKind::FrozenSet || kind == TypeKind::Tuple ||
           kind == TypeKind::Str || kind == TypeKind::Bytes || kind == TypeKind::ByteArray ||
           kind == TypeKind::Range;
}

// Verified against mypy 1.18.1: abs(-1) is int, abs(-1.5) is float,
// abs(True) is int (bool widens, floored at Int like every other arithmetic
// rule in this project), and abs(3+4j) is FLOAT, not complex --
// complex.__abs__ returns the magnitude, a float. The brief's one-line
// table entry ("same numeric kind") is exact for Bool/Int/Float but wrong
// for Complex; this is corrected here rather than silently matching the
// brief, per this task's own instruction to report rather than adjust.
std::optional<Type> abs_result(const std::vector<Type>& args) {
    if (args.size() != 1) {
        return std::nullopt;
    }
    const TypeKind kind = args.front().kind;
    if (kind == TypeKind::Unknown) {
        return Type::unknown();
    }
    if (kind == TypeKind::Complex) {
        return Type::float_();
    }
    const int rank = numeric_rank(kind);
    if (rank == 0) {
        return std::nullopt;
    }
    return rank == 3 ? Type::float_() : Type::int_();
}

// Verified against mypy 1.18.1: round(1.5) is int, round(1) is int,
// round(True) is int -- but round(3+4j) is a genuine [call-overload] error,
// since complex has no __round__. So round is numeric MINUS Complex, not
// "any numeric" -- another correction to the brief's blanket wording, for
// the same reason as abs_result above. Only the one-argument form is
// modelled, per the brief's own table (`round(x)`); round(x, ndigits) is a
// real, different-shaped overload (returns float for a float `x`, verified
// separately) that this table does not attempt.
std::optional<Type> round_result(const std::vector<Type>& args) {
    if (args.size() != 1) {
        return std::nullopt;
    }
    const TypeKind kind = args.front().kind;
    if (kind == TypeKind::Unknown) {
        return Type::int_();
    }
    if (kind != TypeKind::Bool && kind != TypeKind::Int && kind != TypeKind::Float) {
        return std::nullopt;
    }
    return Type::int_();
}

// Verified: divmod(5, 2) is tuple[int, int]. Real Python/mypy also accepts
// divmod on floats (returning tuple[float, float]), but the brief's table
// restricts this row to "two integral", so that shape is left unmodelled
// (nullopt) rather than guessed at.
std::optional<Type> divmod_result(const std::vector<Type>& args) {
    if (args.size() != 2) {
        return std::nullopt;
    }
    if (!accepts_integral(args[0].kind) || !accepts_integral(args[1].kind)) {
        return std::nullopt;
    }
    return Type::tuple_of({Type::int_(), Type::int_()});
}

// sum/min/max share one shape per the brief: exactly one argument is read as
// an ITERABLE (its element type is the answer, via operator_rules.h's own
// element_type -- the same rule `for` and comprehensions use), while two or
// more arguments are read as plain SCALARS whose join is the answer. This is
// a deliberate simplification of mypy's real (and much fussier) overloads
// for `sum` in particular -- verified sum([1.5, 2.5]) is actually
// `float | Literal[0]` in real mypy, an artifact of sum's default `start = 0`
// overload that this project's Type, with no literal types, cannot
// represent -- but the brief directs this uniform rule for all three names,
// and no test in this task's corpus exercises sum's multi-argument form, so
// it is implemented as directed rather than adjusted.
//
// No ClassLookup is available here (builtin_call_result takes none), so a
// join across two user classes falls back to Object rather than walking a
// base chain -- a recorded imprecision, not a false report, since join()
// never reports.
std::optional<Type> reduce_result(const std::vector<Type>& args) {
    if (args.size() == 1) {
        const RuleResult element = element_type(args.front());
        if (element.status != RuleResult::Status::Ok) {
            return std::nullopt;
        }
        return element.type;
    }
    if (args.size() >= 2) {
        Type joined = args.front();
        for (std::size_t index = 1; index < args.size(); ++index) {
            joined = join(joined, args[index], /*classes=*/nullptr);
        }
        return joined;
    }
    return std::nullopt;
}

// sorted(xs) -> list[element]. Verified: sorted([1]) is list[int].
std::optional<Type> sorted_result(const std::vector<Type>& args) {
    if (args.size() != 1) {
        return std::nullopt;
    }
    const RuleResult element = element_type(args.front());
    if (element.status != RuleResult::Status::Ok) {
        return std::nullopt;
    }
    return Type::list_of(element.type);
}

// int/float/str/bool/bytes/bytearray, zero or one argument, ANY argument
// kind accepted (this is a deliberate simplification of mypy's real
// per-overload argument checking, matching the brief's table exactly: "zero
// or one argument -> the corresponding kind" states no constraint on the
// argument's own type). Matched on the caller-verified name rather than
// builtin_type_names.h's builtin_type_kind, which also answers for
// list/dict/set/frozenset/tuple/object/range -- names that do NOT belong to
// this zero-or-one-argument conversion-function shape.
std::optional<Type> conversion_result(const std::string& name, const std::vector<Type>& args) {
    if (args.size() > 1) {
        return std::nullopt;
    }
    if (name == "int") {
        return Type::int_();
    }
    if (name == "float") {
        return Type::float_();
    }
    if (name == "str") {
        return Type::str();
    }
    if (name == "bool") {
        return Type::bool_();
    }
    if (name == "bytes") {
        return Type::bytes();
    }
    if (name == "bytearray") {
        return Type::bytearray_();
    }
    return std::nullopt;
}

// list()/dict()/set()/frozenset()/tuple() with ZERO arguments: identical to
// an empty []/{} display (Task 13) -- WITH a matching `expected` context,
// take it; WITHOUT one, nullopt (the caller reads that as silent Unknown via
// is_empty_display_builtin, never as a report). Only the zero-argument shape
// is modelled, per the brief's table; list(some_iterable) and friends are
// out of scope and also answer nullopt, but the caller does NOT treat that
// nullopt as silent, since is_empty_display_builtin does not know about
// argument count -- see builtin_call_result's own zero-argument gate below,
// which is what actually keeps `list(1, 2)` from being silently accepted.
std::optional<Type> container_constructor_result(const std::string& name, const Type& expected) {
    if (name == "list" && expected.kind == TypeKind::List && expected.args.size() == 1) {
        return expected;
    }
    if (name == "dict" && expected.kind == TypeKind::Dict && expected.args.size() == 2) {
        return expected;
    }
    if (name == "set" && expected.kind == TypeKind::Set && expected.args.size() == 1) {
        return expected;
    }
    if (name == "frozenset" && expected.kind == TypeKind::FrozenSet && expected.args.size() == 1) {
        return expected;
    }
    if (name == "tuple" && expected.kind == TypeKind::Tuple) {
        return expected;
    }
    return std::nullopt;
}

constexpr std::array<const char*, 27> kSupportedBuiltinCalls = {{
    "print", "len",   "range", "abs",  "int",       "float",     "str",
    "bool",  "bytes", "bytearray", "round", "ord",  "chr",       "hex",
    "repr",  "input", "divmod", "sum", "min",       "max",       "sorted",
    "isinstance", "list", "dict", "set", "frozenset", "tuple",
}};

// Real, mypy-defined builtins this model deliberately does not support --
// see is_supported_builtin_call's own comment for why (each needs a generic
// Iterator this project's Type has no constructor for).
constexpr std::array<const char*, 5> kUnsupportedBuiltinCalls = {{
    "enumerate",
    "zip",
    "map",
    "filter",
    "reversed",
}};

constexpr std::array<const char*, 5> kEmptyDisplayBuiltins = {{
    "list",
    "dict",
    "set",
    "frozenset",
    "tuple",
}};

template <std::size_t Count>
bool contains(const std::string& name, const std::array<const char*, Count>& names) {
    for (const char* candidate : names) {
        if (name == candidate) {
            return true;
        }
    }
    return false;
}

} // namespace

bool is_supported_builtin_call(const std::string& name) {
    return contains(name, kSupportedBuiltinCalls);
}

bool is_builtin_callable_name(const std::string& name) {
    return is_supported_builtin_call(name) || contains(name, kUnsupportedBuiltinCalls);
}

bool is_empty_display_builtin(const std::string& name) {
    return contains(name, kEmptyDisplayBuiltins);
}

std::optional<Type> builtin_call_result(const std::string& name, const std::vector<Type>& args,
                                        const Type& expected) {
    if (name == "print") {
        // Any arity, any argument types. Verified: print() and print(1, "s")
        // are both clean, always None.
        return Type::none();
    }
    if (name == "len") {
        if (args.size() != 1 || !accepts_len(args.front().kind)) {
            return std::nullopt;
        }
        return Type::int_();
    }
    if (name == "range") {
        if (args.empty() || args.size() > 3) {
            return std::nullopt;
        }
        for (const Type& arg : args) {
            if (!accepts_integral(arg.kind)) {
                return std::nullopt;
            }
        }
        // Non-generic: verified reveal_type(range(3)) is builtins.range, with
        // no type argument.
        return Type::range_();
    }
    if (name == "abs") {
        return abs_result(args);
    }
    if (name == "int" || name == "float" || name == "str" || name == "bool" ||
        name == "bytes" || name == "bytearray") {
        return conversion_result(name, args);
    }
    if (name == "round") {
        return round_result(args);
    }
    if (name == "ord") {
        if (args.size() != 1) {
            return std::nullopt;
        }
        if (args.front().kind != TypeKind::Str && args.front().kind != TypeKind::Unknown) {
            return std::nullopt;
        }
        return Type::int_();
    }
    if (name == "chr" || name == "hex") {
        if (args.size() != 1 || !accepts_integral(args.front().kind)) {
            return std::nullopt;
        }
        return Type::str();
    }
    if (name == "repr") {
        // Any single argument.
        if (args.size() != 1) {
            return std::nullopt;
        }
        return Type::str();
    }
    if (name == "input") {
        if (args.size() > 1) {
            return std::nullopt;
        }
        return Type::str();
    }
    if (name == "divmod") {
        return divmod_result(args);
    }
    if (name == "sum" || name == "min" || name == "max") {
        return reduce_result(args);
    }
    if (name == "sorted") {
        return sorted_result(args);
    }
    if (name == "isinstance") {
        // Two arguments, any types -- the second is not checked against
        // being "a type" here, matching the brief's table exactly.
        if (args.size() != 2) {
            return std::nullopt;
        }
        return Type::bool_();
    }
    if (is_empty_display_builtin(name)) {
        // Only the ZERO-argument form is modelled; anything else falls to
        // nullopt here and is a genuine (reported) mismatch to the caller,
        // since is_empty_display_builtin alone does not gate on arity.
        if (!args.empty()) {
            return std::nullopt;
        }
        return container_constructor_result(name, expected);
    }
    return std::nullopt;
}

} // namespace cythonpp::domain::semantic
