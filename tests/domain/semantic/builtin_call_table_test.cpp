#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "domain/semantic/builtin_call_table.h"
#include "domain/semantic/type.h"
#include "domain/semantic/type_name.h"

namespace cythonpp::domain::semantic {
namespace {

std::string name_of(const std::optional<Type>& type) {
    return type.has_value() ? type_name(*type) : "<nullopt>";
}

TEST(BuiltinCallTable, PrintAcceptsAnyArityAndTypesReturningNone) {
    EXPECT_EQ(name_of(builtin_call_result("print", {}, Type::unknown())), "None");
    EXPECT_EQ(name_of(builtin_call_result("print", {Type::int_()}, Type::unknown())), "None");
    EXPECT_EQ(name_of(builtin_call_result(
                  "print", {Type::int_(), Type::str(), Type::class_of("Widget")},
                  Type::unknown())),
              "None");
}

TEST(BuiltinCallTable, LenAcceptsAContainerAndReturnsInt) {
    EXPECT_EQ(name_of(builtin_call_result("len", {Type::list_of(Type::int_())}, Type::unknown())),
              "int");
    EXPECT_EQ(name_of(builtin_call_result("len", {Type::str()}, Type::unknown())), "int");
    EXPECT_EQ(name_of(builtin_call_result("len", {Type::dict_of(Type::str(), Type::int_())},
                                          Type::unknown())),
              "int");
    EXPECT_EQ(name_of(builtin_call_result("len", {Type::tuple_of({Type::int_()})},
                                          Type::unknown())),
              "int");
}

// Pinned explicitly by the task brief: len(1) is not modelled -- int is not
// a container.
TEST(BuiltinCallTable, LenRejectsANonContainer) {
    EXPECT_FALSE(builtin_call_result("len", {Type::int_()}, Type::unknown()).has_value());
}

TEST(BuiltinCallTable, LenRejectsTheWrongArity) {
    EXPECT_FALSE(builtin_call_result("len", {}, Type::unknown()).has_value());
    EXPECT_FALSE(builtin_call_result("len", {Type::str(), Type::str()}, Type::unknown())
                     .has_value());
}

TEST(BuiltinCallTable, RangeAcceptsOneToThreeIntegralArgumentsReturningANonGenericRange) {
    EXPECT_EQ(name_of(builtin_call_result("range", {Type::int_()}, Type::unknown())), "range");
    EXPECT_EQ(name_of(builtin_call_result("range", {Type::int_(), Type::int_()},
                                          Type::unknown())),
              "range");
    EXPECT_EQ(name_of(builtin_call_result(
                  "range", {Type::int_(), Type::int_(), Type::int_()}, Type::unknown())),
              "range");
    // Bool counts as integral, matching every other numeric rule in this
    // project.
    EXPECT_EQ(name_of(builtin_call_result("range", {Type::bool_()}, Type::unknown())), "range");
}

// Pinned explicitly by the task brief.
TEST(BuiltinCallTable, RangeRejectsAFloatArgument) {
    EXPECT_FALSE(builtin_call_result("range", {Type::float_()}, Type::unknown()).has_value());
}

// Pinned explicitly by the task brief.
TEST(BuiltinCallTable, RangeRejectsZeroArguments) {
    EXPECT_FALSE(builtin_call_result("range", {}, Type::unknown()).has_value());
}

TEST(BuiltinCallTable, RangeRejectsMoreThanThreeArguments) {
    EXPECT_FALSE(builtin_call_result(
                     "range", {Type::int_(), Type::int_(), Type::int_(), Type::int_()},
                     Type::unknown())
                     .has_value());
}

// Verified against mypy 1.18.1: abs(-1) is int, abs(-1.5) is float, and
// abs(True) is int (bool widens under arithmetic, floored at Int).
TEST(BuiltinCallTable, AbsReturnsTheSameNumericKindForIntAndFloat) {
    EXPECT_EQ(name_of(builtin_call_result("abs", {Type::int_()}, Type::unknown())), "int");
    EXPECT_EQ(name_of(builtin_call_result("abs", {Type::float_()}, Type::unknown())), "float");
    EXPECT_EQ(name_of(builtin_call_result("abs", {Type::bool_()}, Type::unknown())), "int");
}

// Verified against mypy 1.18.1: reveal_type(abs(3+4j)) is float, NOT
// complex -- complex.__abs__ returns the magnitude. The brief's table row
// ("abs(x): one numeric -> the same numeric kind") is wrong for Complex;
// this pins the corrected, verified behaviour instead of the brief's claim.
TEST(BuiltinCallTable, AbsOnComplexReturnsFloatNotComplex) {
    EXPECT_EQ(name_of(builtin_call_result("abs", {Type::complex_()}, Type::unknown())), "float");
}

TEST(BuiltinCallTable, AbsRejectsANonNumericArgument) {
    EXPECT_FALSE(builtin_call_result("abs", {Type::str()}, Type::unknown()).has_value());
}

TEST(BuiltinCallTable, ConversionFunctionsAcceptZeroOrOneArgumentOfAnyType) {
    EXPECT_EQ(name_of(builtin_call_result("int", {}, Type::unknown())), "int");
    EXPECT_EQ(name_of(builtin_call_result("int", {Type::str()}, Type::unknown())), "int");
    EXPECT_EQ(name_of(builtin_call_result("float", {Type::int_()}, Type::unknown())), "float");
    EXPECT_EQ(name_of(builtin_call_result("str", {Type::int_()}, Type::unknown())), "str");
    EXPECT_EQ(name_of(builtin_call_result("bool", {Type::int_()}, Type::unknown())), "bool");
    EXPECT_EQ(name_of(builtin_call_result("bytes", {}, Type::unknown())), "bytes");
    EXPECT_EQ(name_of(builtin_call_result("bytearray", {}, Type::unknown())), "bytearray");
}

TEST(BuiltinCallTable, ConversionFunctionsRejectMoreThanOneArgument) {
    EXPECT_FALSE(
        builtin_call_result("int", {Type::str(), Type::int_()}, Type::unknown()).has_value());
}

// Verified against mypy 1.18.1: round(1.5) is int, round(1) is int. Only the
// single-argument form is modelled, per the brief's own table row.
TEST(BuiltinCallTable, RoundReturnsIntForAnyNumericArgument) {
    EXPECT_EQ(name_of(builtin_call_result("round", {Type::float_()}, Type::unknown())), "int");
    EXPECT_EQ(name_of(builtin_call_result("round", {Type::int_()}, Type::unknown())), "int");
    EXPECT_EQ(name_of(builtin_call_result("round", {Type::bool_()}, Type::unknown())), "int");
}

// Verified against mypy 1.18.1: round(3+4j) is a genuine [call-overload]
// error -- complex has no __round__. Not in the brief's table; found while
// verifying the "same numeric kind" claim for abs.
TEST(BuiltinCallTable, RoundRejectsComplex) {
    EXPECT_FALSE(builtin_call_result("round", {Type::complex_()}, Type::unknown()).has_value());
}

TEST(BuiltinCallTable, OrdAcceptsStrReturningInt) {
    EXPECT_EQ(name_of(builtin_call_result("ord", {Type::str()}, Type::unknown())), "int");
}

TEST(BuiltinCallTable, OrdRejectsANonStrArgument) {
    EXPECT_FALSE(builtin_call_result("ord", {Type::int_()}, Type::unknown()).has_value());
}

TEST(BuiltinCallTable, ChrAndHexAcceptIntegralReturningStr) {
    EXPECT_EQ(name_of(builtin_call_result("chr", {Type::int_()}, Type::unknown())), "str");
    EXPECT_EQ(name_of(builtin_call_result("hex", {Type::int_()}, Type::unknown())), "str");
}

TEST(BuiltinCallTable, ReprAndInputReturnStr) {
    EXPECT_EQ(name_of(builtin_call_result("repr", {Type::int_()}, Type::unknown())), "str");
    EXPECT_EQ(name_of(builtin_call_result("input", {}, Type::unknown())), "str");
    EXPECT_EQ(name_of(builtin_call_result("input", {Type::str()}, Type::unknown())), "str");
}

TEST(BuiltinCallTable, DivmodAcceptsTwoIntegralReturningATupleOfTwoInts) {
    EXPECT_EQ(name_of(builtin_call_result("divmod", {Type::int_(), Type::int_()},
                                          Type::unknown())),
              "tuple[int, int]");
}

TEST(BuiltinCallTable, DivmodRejectsANonIntegralArgument) {
    EXPECT_FALSE(
        builtin_call_result("divmod", {Type::float_(), Type::int_()}, Type::unknown())
            .has_value());
}

TEST(BuiltinCallTable, SumMinMaxOnOneIterableReturnItsElementType) {
    EXPECT_EQ(name_of(builtin_call_result("sum", {Type::list_of(Type::int_())}, Type::unknown())),
              "int");
    EXPECT_EQ(name_of(builtin_call_result("min", {Type::list_of(Type::str())}, Type::unknown())),
              "str");
    EXPECT_EQ(name_of(builtin_call_result("max", {Type::list_of(Type::int_())}, Type::unknown())),
              "int");
}

TEST(BuiltinCallTable, SumMinMaxOnTwoOrMoreScalarsReturnTheirJoin) {
    EXPECT_EQ(name_of(builtin_call_result("min", {Type::int_(), Type::float_()},
                                          Type::unknown())),
              "float");
    EXPECT_EQ(name_of(builtin_call_result("max", {Type::int_(), Type::int_(), Type::float_()},
                                          Type::unknown())),
              "float");
}

TEST(BuiltinCallTable, SortedReturnsAListOfTheIterablesElementType) {
    EXPECT_EQ(name_of(builtin_call_result("sorted", {Type::list_of(Type::int_())},
                                          Type::unknown())),
              "list[int]");
}

TEST(BuiltinCallTable, IsinstanceAcceptsTwoArgumentsReturningBool) {
    EXPECT_EQ(name_of(builtin_call_result("isinstance", {Type::int_(), Type::class_of("X")},
                                          Type::unknown())),
              "bool");
}

TEST(BuiltinCallTable, IsinstanceRejectsTheWrongArity) {
    EXPECT_FALSE(builtin_call_result("isinstance", {Type::int_()}, Type::unknown()).has_value());
}

// list() with a matching `expected` context takes it, exactly like an empty
// [] display.
TEST(BuiltinCallTable, ListWithContextTakesTheContext) {
    const std::optional<Type> result =
        builtin_call_result("list", {}, Type::list_of(Type::int_()));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(type_name(*result), "list[int]");
}

// list() with NO usable context answers nullopt -- the caller (not this
// function) is what turns that into a SILENT Unknown rather than a report,
// via is_empty_display_builtin.
TEST(BuiltinCallTable, ListWithNoContextReturnsNullopt) {
    EXPECT_FALSE(builtin_call_result("list", {}, Type::unknown()).has_value());
}

TEST(BuiltinCallTable, EveryContainerConstructorTakesItsMatchingContext) {
    EXPECT_EQ(name_of(builtin_call_result("dict", {},
                                          Type::dict_of(Type::str(), Type::int_()))),
              "dict[str, int]");
    EXPECT_EQ(name_of(builtin_call_result("set", {}, Type::set_of(Type::int_()))), "set[int]");
    EXPECT_EQ(name_of(builtin_call_result("frozenset", {}, Type::frozenset_of(Type::int_()))),
              "frozenset[int]");
    EXPECT_EQ(name_of(builtin_call_result(
                  "tuple", {}, Type::tuple_of({Type::int_(), Type::str()}))),
              "tuple[int, str]");
}

// The pair the brief calls out explicitly: is_supported_builtin_call is
// false for zip while is_builtin_callable_name is true -- what lets the
// caller distinguish "not supported" from "not defined."
TEST(BuiltinCallTable, DistinguishesUnsupportedFromUndefinedBuiltins) {
    for (const std::string& name : {"enumerate", "zip", "map", "filter", "reversed"}) {
        EXPECT_FALSE(is_supported_builtin_call(name)) << name;
        EXPECT_TRUE(is_builtin_callable_name(name)) << name;
    }
}

TEST(BuiltinCallTable, EverySupportedNameIsAlsoABuiltinCallableName) {
    for (const std::string& name :
         {"print", "len", "range", "abs", "int", "float", "str", "bool", "bytes", "bytearray",
          "round", "ord", "chr", "hex", "repr", "input", "divmod", "sum", "min", "max", "sorted",
          "isinstance", "list", "dict", "set", "frozenset", "tuple"}) {
        EXPECT_TRUE(is_supported_builtin_call(name)) << name;
        EXPECT_TRUE(is_builtin_callable_name(name)) << name;
    }
}

TEST(BuiltinCallTable, AnUnrelatedNameIsNeitherSupportedNorABuiltinCallableName) {
    EXPECT_FALSE(is_supported_builtin_call("frobnicate"));
    EXPECT_FALSE(is_builtin_callable_name("frobnicate"));
}

TEST(BuiltinCallTable, OnlyTheFiveContainerConstructorsAreEmptyDisplayBuiltins) {
    for (const std::string& name : {"list", "dict", "set", "frozenset", "tuple"}) {
        EXPECT_TRUE(is_empty_display_builtin(name)) << name;
    }
    for (const std::string& name : {"print", "len", "range", "zip"}) {
        EXPECT_FALSE(is_empty_display_builtin(name)) << name;
    }
}

} // namespace
} // namespace cythonpp::domain::semantic
