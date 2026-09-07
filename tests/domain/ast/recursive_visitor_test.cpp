#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "domain/ast/bin_op.h"
#include "domain/ast/name.h"
#include "domain/ast/recursive_visitor.h"
#include "domain/ast/source_span.h"
#include "domain/diagnostics/diagnostic_sink.h"
#include "domain/lexer/indentation_pass.h"
#include "domain/lexer/lexer.h"
#include "domain/parser/statement_parser.h"

namespace cythonpp::domain::ast {
namespace {

// Real source through the real chain, per Global Constraint 10. A hand-built
// tree would let this test agree with a wrong belief about what the parser
// produces.
std::unique_ptr<Module> parse(const std::string& source) {
    lexer::Lexer lexer(source);
    const lexer::TokenStream lexed(lexer.tokenize());
    diagnostics::DiagnosticSink sink;
    lexer::TokenStream tokens = lexer::IndentationPass().run(lexed, sink);
    std::unique_ptr<Module> module = parser::StatementParser(tokens, sink).parse_module();
    EXPECT_TRUE(sink.empty()) << "fixture must parse cleanly";
    return module;
}

// Counts three node kinds, overriding nothing else. If the base's defaults did
// not recurse, this would count 0.
class CountingVisitor : public RecursiveVisitor {
public:
    int count = 0;

    void visit(const Name& node) override {
        ++count;
        RecursiveVisitor::visit(node);
    }
    void visit(const Constant& node) override {
        ++count;
        RecursiveVisitor::visit(node);
    }
    void visit(const BinOp& node) override {
        ++count;
        RecursiveVisitor::visit(node);
    }
};

// THE PIN for visitor.h's corrected comment. This subclass declares exactly
// ONE visit override, which by ordinary name-hiding rules hides the other
// twenty-five from name lookup in this class's scope. The earlier version of
// visitor.h's comment claimed traversal would therefore "silently stop".
//
// It does not: accept() dispatches through Visitor&, virtual dispatch is
// unaffected by name hiding, so the twenty-five inherited defaults still fire
// and this visitor still reaches every Name in the tree.
class OnlyNamesVisitor : public RecursiveVisitor {
public:
    std::vector<std::string> names;

    void visit(const Name& node) override { names.push_back(node.identifier()); }
};

// The one thing name hiding DOES cost, and the documented remedy. Without the
// using-declaration, the unqualified `visit(...)` call below fails to compile
// with "no viable conversion". That this file COMPILES is the assertion.
//
// NOTE (deviation from the task-5 brief): the brief's original body called
// `visit(node.left())` / `visit(node.right())`. left()/right() return the
// abstract `Expr&` base, and NO `visit()` overload accepts `Expr&` -- by
// design, the double-dispatch step that turns a base reference into a
// concrete one is accept(), never visit(). That call fails to compile with
// or without the using-declaration (confirmed: identical "no viable
// conversion from 'const Expr' to ..." errors either way), so it cannot pin
// what this test claims to pin. A dummy concrete Name node is constructed
// here instead, purely to give an unqualified `visit(...)` call a
// concrete-typed argument at compile time -- this is not part of the real
// tree walk, just a compile-time probe of name lookup.
class UsingDeclarationVisitor : public RecursiveVisitor {
public:
    using RecursiveVisitor::visit;

    int binops = 0;

    void visit(const BinOp& node) override {
        ++binops;
        // Unqualified, and reaching a DIFFERENT overload (Name) than the one
        // this class declares (BinOp). Without the using-declaration above,
        // name lookup for `visit` here sees only this class's own BinOp
        // overload, and this call fails with "no viable conversion from
        // 'const Name' to 'const BinOp'".
        static const SourceSpan dummy_span{0, 0, 0, 0};
        visit(Name(dummy_span, "dummy"));
    }
};

TEST(RecursiveVisitor, DefaultsRecurseIntoEveryChild) {
    const std::unique_ptr<Module> module = parse("x = 1 + 2\n");

    CountingVisitor visitor;
    module->accept(visitor);

    // Name x, BinOp, Constant 1, Constant 2.
    EXPECT_EQ(visitor.count, 4);
}

// The corrected-comment pin: ONE override, and traversal still reaches every
// Name four nesting levels down.
TEST(RecursiveVisitor, ASubclassOverridingOneMethodStillTraversesEverything) {
    const std::unique_ptr<Module> module = parse(
        "def f(a: int) -> int:\n"
        "    if a:\n"
        "        return b\n"
        "    while c:\n"
        "        d = e\n"
        "    return g\n");

    OnlyNamesVisitor visitor;
    module->accept(visitor);

    // Names reached from inside a def, an if body, a while body and two
    // returns. The two `int`s are the parameter annotation and the return
    // annotation, both of which are Name nodes.
    //
    // NOTE (deviation from the task-5 brief): the brief expected a leading
    // "a" here for the parameter itself, but Parameter::name (parameter.h)
    // is a plain std::string field, not an ExprPtr/Name node -- there is no
    // Name to visit for a bare parameter identifier, only for its annotation
    // and default_value (which ARE ExprPtr). Confirmed by running: the
    // parameter's own name never appears in the traversal. The second "a"
    // below is the real Name node from the `if a:` condition.
    EXPECT_EQ(visitor.names, (std::vector<std::string>{"int", "int", "a", "b", "c", "d",
                                                       "e", "g"}));
}

TEST(RecursiveVisitor, AUsingDeclarationRestoresTheHiddenOverloads) {
    const std::unique_ptr<Module> module = parse("x = 1 + 2\n");

    UsingDeclarationVisitor visitor;
    module->accept(visitor);

    EXPECT_EQ(visitor.binops, 1);
}

// The guarded accessors, each of which dereferences unconditionally and would
// crash if recursed into without its guard: Return::value(),
// AnnAssign::value(), FunctionDef::return_annotation(), and both of
// Parameter's raw ExprPtr fields. If any guard is missing this segfaults
// rather than failing.
TEST(RecursiveVisitor, GuardedAccessorsAreNotDereferencedWhenAbsent) {
    const std::unique_ptr<Module> module = parse(
        "def f(a):\n"
        "    x: int\n"
        "    return\n");

    OnlyNamesVisitor visitor;
    module->accept(visitor);

    // Parameter `a` has no annotation and no default; the def has no return
    // annotation; the AnnAssign has no value; the return is bare.
    EXPECT_EQ(visitor.names, (std::vector<std::string>{"x", "int"}));
}

TEST(RecursiveVisitor, ComprehensionClausesAreFullyTraversed) {
    const std::unique_ptr<Module> module = parse("xs = [a for b in c if d if e]\n");

    OnlyNamesVisitor visitor;
    module->accept(visitor);

    // Element, target, iterable, and BOTH conditions. A ListComp arm that
    // forgets `conditions` silently leaves the filters unchecked.
    EXPECT_EQ(visitor.names, (std::vector<std::string>{"xs", "a", "b", "c", "d", "e"}));
}

TEST(RecursiveVisitor, CompareRestOperandsAndDictEntriesAreTraversed) {
    const std::unique_ptr<Module> compare = parse("r = a < b < c\n");
    OnlyNamesVisitor compare_visitor;
    compare->accept(compare_visitor);
    EXPECT_EQ(compare_visitor.names, (std::vector<std::string>{"r", "a", "b", "c"}));

    const std::unique_ptr<Module> dict = parse("d = {k1: v1, k2: v2}\n");
    OnlyNamesVisitor dict_visitor;
    dict->accept(dict_visitor);
    EXPECT_EQ(dict_visitor.names, (std::vector<std::string>{"d", "k1", "v1", "k2", "v2"}));
}

TEST(RecursiveVisitor, OrElseBranchesAreTraversed) {
    const std::unique_ptr<Module> module = parse(
        "if a:\n"
        "    b = 1\n"
        "else:\n"
        "    c = 2\n");

    OnlyNamesVisitor visitor;
    module->accept(visitor);

    // `c` appears only if orelse() is recursed into.
    EXPECT_EQ(visitor.names, (std::vector<std::string>{"a", "b", "c"}));
}

TEST(RecursiveVisitor, ClassBasesAndBodyAreTraversed) {
    const std::unique_ptr<Module> module = parse(
        "class D(B):\n"
        "    x: int = y\n");

    OnlyNamesVisitor visitor;
    module->accept(visitor);

    EXPECT_EQ(visitor.names, (std::vector<std::string>{"B", "x", "int", "y"}));
}

} // namespace
} // namespace cythonpp::domain::ast
