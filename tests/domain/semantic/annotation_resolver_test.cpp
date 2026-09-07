#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "domain/ast/ann_assign.h"
#include "domain/ast/module.h"
#include "domain/diagnostics/diagnostic.h"
#include "domain/diagnostics/diagnostic_sink.h"
#include "domain/lexer/indentation_pass.h"
#include "domain/lexer/lexer.h"
#include "domain/lexer/token_stream.h"
#include "domain/parser/statement_parser.h"
#include "domain/semantic/annotation_resolver.h"
#include "domain/semantic/type.h"
#include "domain/semantic/type_name.h"
#include "fake_class_lookup.h"

namespace cythonpp::domain::semantic {
namespace {

using semantic_test_support::FakeClassLookup;

struct Resolved {
    Type type;
    std::string printed;
    std::vector<diagnostics::Diagnostic> diagnostics;
};

// One resolution through the REAL chain: source text -> Lexer ->
// IndentationPass -> StatementParser -> AnnAssign::annotation() ->
// AnnotationResolver.
//
// Nothing is hand-constructed, deliberately. A hand-built
// Subscript(Name dict, TupleExpr(...)) would let this test agree with a wrong
// belief about what the parser produces -- and the Constant None arm exists
// precisely because that belief was wrong.
//
// `source` must be a single annotated assignment, e.g. "x: int = 1\n".
Resolved resolve_annotation(const std::string& source, const ClassLookup& classes) {
    diagnostics::DiagnosticSink parse_sink;
    lexer::TokenStream stream = lexer::IndentationPass().run(
        lexer::TokenStream(lexer::Lexer(source).tokenize()), parse_sink);
    const std::unique_ptr<ast::Module> module =
        parser::StatementParser(stream, parse_sink).parse_module();

    Resolved resolved;
    EXPECT_TRUE(parse_sink.diagnostics().empty())
        << "fixture failed to parse cleanly: " << source;
    if (module == nullptr || module->body().size() != 1) {
        ADD_FAILURE() << "fixture did not produce exactly one statement: " << source;
        return resolved;
    }
    const auto* annotated = dynamic_cast<const ast::AnnAssign*>(module->body().front().get());
    if (annotated == nullptr) {
        ADD_FAILURE() << "fixture is not an annotated assignment: " << source;
        return resolved;
    }

    diagnostics::DiagnosticSink sink;
    resolved.type = AnnotationResolver(classes, sink).resolve(annotated->annotation());
    resolved.printed = type_name(resolved.type);
    resolved.diagnostics = sink.diagnostics();
    return resolved;
}

// The type a clean resolution produces, rendered. Asserts no diagnostics, so
// a test reading only the string still fails loudly if the resolver reported
// on the way to the right answer.
std::string resolved_name(const std::string& source, const ClassLookup& classes) {
    const Resolved resolved = resolve_annotation(source, classes);
    EXPECT_TRUE(resolved.diagnostics.empty())
        << "unexpected diagnostics resolving: " << source;
    return resolved.printed;
}

std::string resolved_name(const std::string& source) {
    const FakeClassLookup no_classes;
    return resolved_name(source, no_classes);
}

// The single diagnostic a failing resolution must produce. Asserts the COUNT,
// so a rule that reports twice cannot hide behind a message match -- the only
// thing that pins "one diagnostic per root cause".
diagnostics::Diagnostic only_error(const Resolved& resolved) {
    EXPECT_EQ(resolved.diagnostics.size(), 1u) << "expected exactly one diagnostic";
    if (resolved.diagnostics.size() != 1) {
        return diagnostics::Diagnostic{diagnostics::Severity::Error, "", "", 0, 0};
    }
    EXPECT_EQ(resolved.type, Type::unknown()) << "a failed resolution must yield Unknown";
    return resolved.diagnostics.front();
}

TEST(AnnotationResolver, ResolvesEveryNonGenericBuiltinTypeName) {
    EXPECT_EQ(resolved_name("x: int = 1\n"), "int");
    EXPECT_EQ(resolved_name("x: float = 1.0\n"), "float");
    EXPECT_EQ(resolved_name("x: bool = True\n"), "bool");
    EXPECT_EQ(resolved_name("x: str = \"a\"\n"), "str");
    EXPECT_EQ(resolved_name("x: bytes = b\"a\"\n"), "bytes");
    EXPECT_EQ(resolved_name("x: bytearray = b\"a\"\n"), "bytearray");
    EXPECT_EQ(resolved_name("x: complex = 1\n"), "complex");
    EXPECT_EQ(resolved_name("x: object = 1\n"), "object");
}

// range is not one of keyword_table's thirteen builtin type names, so it
// arrives as an IDENTIFIER -- but x: range is legal Python and TypeKind::Range
// exists, so the resolver's table carries it as a fourteenth entry.
TEST(AnnotationResolver, ResolvesRangeEvenThoughTheLexerDoesNotSpecialCaseIt) {
    EXPECT_EQ(resolved_name("x: range = range(3)\n"), "range");
}

// `None` lexes as KEYWORD_NONE and parses as a Constant, NOT a Name. The
// single most surprising fact in this task.
TEST(AnnotationResolver, ResolvesNoneFromAConstantRatherThanAName) {
    EXPECT_EQ(resolved_name("x: None = None\n"), "None");
}

TEST(AnnotationResolver, ResolvesAKnownClassByName) {
    const FakeClassLookup classes({{"Widget", {}}, {"Gadget", {"Widget"}}});

    EXPECT_EQ(resolved_name("x: Widget = w\n", classes), "Widget");
    EXPECT_EQ(resolved_name("x: Gadget = g\n", classes), "Gadget");
}

TEST(AnnotationResolver, ReportsAnUndefinedName) {
    const FakeClassLookup no_classes;
    const Resolved resolved = resolve_annotation("x: Widget = w\n", no_classes);
    const diagnostics::Diagnostic error = only_error(resolved);

    EXPECT_EQ(error.severity, diagnostics::Severity::Error);
    EXPECT_EQ(error.code, "NameError");
    EXPECT_EQ(error.message, "name 'Widget' is not defined");
    EXPECT_EQ(error.line, 1);
    EXPECT_EQ(error.column, 4);
}

// mypy's type-arg rule under --strict's disallow-any-generics: verified,
// `x: list = []` is "Missing type parameters for generic type "list"".
TEST(AnnotationResolver, ReportsABareGenericBuiltin) {
    const FakeClassLookup no_classes;
    for (const std::string& name : {"list", "dict", "set", "frozenset", "tuple"}) {
        const Resolved resolved = resolve_annotation("x: " + name + " = y\n", no_classes);
        const diagnostics::Diagnostic error = only_error(resolved);

        EXPECT_EQ(error.code, "TypeError") << name;
        EXPECT_EQ(error.message, "missing type parameters for generic type \"" + name + "\"")
            << name;
        EXPECT_EQ(error.column, 4) << name;
    }
}

// mypy resolves these. Rejecting is allowed because an unsupported-construct
// message is not a TypeError, and 5b's two-pass collection already makes a
// plain Name forward reference work.
TEST(AnnotationResolver, ReportsAStringForwardReferenceAsUnsupported) {
    const FakeClassLookup classes({{"C", {}}});
    const Resolved resolved = resolve_annotation("x: \"C\" = None\n", classes);
    const diagnostics::Diagnostic error = only_error(resolved);

    EXPECT_EQ(error.code, "NotImplementedError");
    EXPECT_EQ(error.message, "string forward references are not supported");
    EXPECT_EQ(error.column, 4);
}

TEST(AnnotationResolver, ReportsANonNoneLiteralAsNotAValidAnnotation) {
    const FakeClassLookup no_classes;
    for (const std::string& literal : {"1", "1.5", "True", "..."}) {
        const Resolved resolved = resolve_annotation("x: " + literal + " = y\n", no_classes);
        const diagnostics::Diagnostic error = only_error(resolved);

        EXPECT_EQ(error.code, "TypeError") << literal;
        EXPECT_EQ(error.message, "not a valid type annotation") << literal;
    }
}

TEST(AnnotationResolver, ReportsAnExpressionShapeThatIsNotAnAnnotation) {
    const FakeClassLookup classes({{"C", {}}});
    for (const std::string& annotation : {"f(1)", "C.inner", "[int]", "{1: 2}", "-int",
                                          "int and str"}) {
        const Resolved resolved = resolve_annotation("x: " + annotation + " = y\n", classes);
        const diagnostics::Diagnostic error = only_error(resolved);

        EXPECT_EQ(error.code, "TypeError") << annotation;
        EXPECT_EQ(error.message, "not a valid type annotation") << annotation;
    }
}

// The spec's headline acceptance test: the exact shape the spec names.
TEST(AnnotationResolver, ResolvesASubscriptedDictToItsKeyAndValueTypes) {
    EXPECT_EQ(resolved_name("x: dict[str, int] = {}\n"), "dict[str, int]");
}

TEST(AnnotationResolver, ResolvesTheSingleArgumentGenericBuiltins) {
    EXPECT_EQ(resolved_name("x: list[int] = []\n"), "list[int]");
    EXPECT_EQ(resolved_name("x: set[str] = s\n"), "set[str]");
    EXPECT_EQ(resolved_name("x: frozenset[bool] = f\n"), "frozenset[bool]");
}

TEST(AnnotationResolver, ResolvesTuplesOfAnyArity) {
    EXPECT_EQ(resolved_name("x: tuple[int] = t\n"), "tuple[int]");
    EXPECT_EQ(resolved_name("x: tuple[int, str] = t\n"), "tuple[int, str]");
    EXPECT_EQ(resolved_name("x: tuple[int, str, bool] = t\n"), "tuple[int, str, bool]");
}

TEST(AnnotationResolver, ResolvesNestedGenerics) {
    EXPECT_EQ(resolved_name("x: list[list[int]] = []\n"), "list[list[int]]");
    EXPECT_EQ(resolved_name("x: dict[str, list[int]] = {}\n"), "dict[str, list[int]]");
    EXPECT_EQ(resolved_name("x: dict[str, dict[str, int]] = {}\n"),
              "dict[str, dict[str, int]]");
}

TEST(AnnotationResolver, ResolvesAClassAsAGenericArgument) {
    const FakeClassLookup classes({{"Widget", {}}});

    EXPECT_EQ(resolved_name("x: list[Widget] = []\n", classes), "list[Widget]");
    EXPECT_EQ(resolved_name("x: dict[str, Widget] = {}\n", classes), "dict[str, Widget]");
}

// PEP 604. `None` here comes through the Constant arm, not the Name arm.
TEST(AnnotationResolver, ResolvesAPipeUnion) {
    EXPECT_EQ(resolved_name("x: str | None = None\n"), "str | None");
    EXPECT_EQ(resolved_name("x: int | str = y\n"), "int | str");
    EXPECT_EQ(resolved_name("x: int | str | None = y\n"), "int | str | None");
}

TEST(AnnotationResolver, ResolvesAUnionOfGenerics) {
    EXPECT_EQ(resolved_name("x: list[int] | None = None\n"), "list[int] | None");
    EXPECT_EQ(resolved_name("x: dict[str, int] | list[int] = y\n"),
              "dict[str, int] | list[int]");
}

TEST(AnnotationResolver, CollapsesADuplicatedUnionMember) {
    EXPECT_EQ(resolved_name("x: int | int = y\n"), "int");
}

TEST(AnnotationResolver, ReportsWrongArityOnAGenericBuiltin) {
    const FakeClassLookup no_classes;

    const Resolved one_for_dict = resolve_annotation("x: dict[str] = {}\n", no_classes);
    const diagnostics::Diagnostic dict_error = only_error(one_for_dict);
    EXPECT_EQ(dict_error.code, "TypeError");
    EXPECT_EQ(dict_error.message, "\"dict\" expects 2 type arguments, but 1 given");

    const Resolved two_for_list = resolve_annotation("x: list[int, str] = []\n", no_classes);
    const diagnostics::Diagnostic list_error = only_error(two_for_list);
    EXPECT_EQ(list_error.code, "TypeError");
    EXPECT_EQ(list_error.message, "\"list\" expects 1 type argument, but 2 given");
}

// mypy accepts tuple[int, ...], so this must be NotImplementedError rather
// than TypeError or the hard invariant breaks.
TEST(AnnotationResolver, ReportsAVariadicTupleAsUnsupported) {
    const FakeClassLookup no_classes;
    const Resolved resolved = resolve_annotation("x: tuple[int, ...] = t\n", no_classes);
    const diagnostics::Diagnostic error = only_error(resolved);

    EXPECT_EQ(error.code, "NotImplementedError");
    EXPECT_EQ(error.message, "variadic tuple annotations are not supported");
}

TEST(AnnotationResolver, ReportsSubscriptingANonGenericType) {
    const FakeClassLookup classes({{"Widget", {}}});

    for (const std::string& annotation : {"int[str]", "str[int]", "Widget[int]"}) {
        const Resolved resolved = resolve_annotation("x: " + annotation + " = y\n", classes);
        const diagnostics::Diagnostic error = only_error(resolved);

        EXPECT_EQ(error.code, "TypeError") << annotation;
        EXPECT_NE(error.message.find("is not subscriptable"), std::string::npos) << annotation;
    }
}

// None of these can be imported, so the base is simply undefined. This is
// where the import gap becomes visible, and it is the correct outcome.
TEST(AnnotationResolver, ReportsTheUnimportableTypingNamesAsUndefined) {
    const FakeClassLookup no_classes;
    const std::map<std::string, std::string> annotations = {
        {"Optional[int]", "Optional"},
        {"Union[int, str]", "Union"},
        {"Callable[[int], str]", "Callable"},
        {"Generic[T]", "Generic"},
    };

    for (const auto& entry : annotations) {
        const Resolved resolved = resolve_annotation("x: " + entry.first + " = y\n", no_classes);
        const diagnostics::Diagnostic error = only_error(resolved);

        EXPECT_EQ(error.code, "NameError") << entry.first;
        EXPECT_EQ(error.message, "name '" + entry.second + "' is not defined") << entry.first;
    }
}

TEST(AnnotationResolver, ReportsANonNameSubscriptBase) {
    const FakeClassLookup no_classes;
    const Resolved resolved = resolve_annotation("x: list[int][str] = y\n", no_classes);
    const diagnostics::Diagnostic error = only_error(resolved);

    EXPECT_EQ(error.code, "TypeError");
    EXPECT_EQ(error.message, "not a valid type annotation");
}

TEST(AnnotationResolver, ReportsABinaryOperatorThatIsNotAUnion) {
    const FakeClassLookup no_classes;
    for (const std::string& annotation : {"int + str", "int & str", "int * 2"}) {
        const Resolved resolved = resolve_annotation("x: " + annotation + " = y\n", no_classes);
        const diagnostics::Diagnostic error = only_error(resolved);

        EXPECT_EQ(error.code, "TypeError") << annotation;
        EXPECT_EQ(error.message, "not a valid type annotation") << annotation;
    }
}

// One diagnostic per root cause, and NO cascade: the enclosing dict does not
// add a third. This is the only thing that pins the absorbing behaviour.
TEST(AnnotationResolver, ReportsEachUndefinedArgumentOnceAndDoesNotCascade) {
    const FakeClassLookup no_classes;
    const Resolved resolved = resolve_annotation("x: dict[Foo, Bar] = {}\n", no_classes);

    ASSERT_EQ(resolved.diagnostics.size(), 2u);
    EXPECT_EQ(resolved.diagnostics[0].message, "name 'Foo' is not defined");
    EXPECT_EQ(resolved.diagnostics[1].message, "name 'Bar' is not defined");
    EXPECT_EQ(resolved.type, Type::unknown());
}

TEST(AnnotationResolver, AFailedUnionMemberMakesTheWholeUnionUnknownWithoutCascading) {
    const FakeClassLookup no_classes;
    const Resolved resolved = resolve_annotation("x: Foo | None = None\n", no_classes);
    const diagnostics::Diagnostic error = only_error(resolved);

    EXPECT_EQ(error.code, "NameError");
    EXPECT_EQ(error.message, "name 'Foo' is not defined");
}

TEST(AnnotationResolver, AFailedGenericArgumentMakesTheWholeAnnotationUnknown) {
    const FakeClassLookup no_classes;
    const Resolved resolved = resolve_annotation("x: list[Foo] = []\n", no_classes);
    const diagnostics::Diagnostic error = only_error(resolved);

    EXPECT_EQ(error.message, "name 'Foo' is not defined");
}

} // namespace
} // namespace cythonpp::domain::semantic
