#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "domain/ast/ast_printer.h"
#include "domain/ast/attribute.h"
#include "domain/ast/bin_op.h"
#include "domain/ast/bool_op.h"
#include "domain/ast/call.h"
#include "domain/ast/compare.h"
#include "domain/ast/comprehension_clause.h"
#include "domain/ast/constant.h"
#include "domain/ast/dict_expr.h"
#include "domain/ast/list_comp.h"
#include "domain/ast/list_expr.h"
#include "domain/ast/name.h"
#include "domain/ast/subscript.h"
#include "domain/ast/tuple_expr.h"
#include "domain/ast/unary_op.h"

namespace cythonpp::domain::ast {
namespace {

// Every node in these tests needs a span, and which span is almost never what
// the test is about, so one placeholder keeps the assertions readable.
constexpr SourceSpan kSpan{1, 1, 1, 2};

std::string print(const Node& node) { return AstPrinter().print(node); }

TEST(AstPrinter, NameRendersItsIdentifier) {
    const Name node(kSpan, "total");
    EXPECT_EQ(print(node), "(Name total)");
}

TEST(AstPrinter, ConstantRendersItsRawLexeme) {
    const Constant node(kSpan, lexer::token_type::LITERAL_INT, "0xFF");
    EXPECT_EQ(print(node), "(Constant 0xFF)");
}

TEST(AstPrinter, ConstantKeepsTheLexerTokenType) {
    const Constant node(kSpan, lexer::token_type::LITERAL_STRING, "'hi'");
    EXPECT_EQ(node.type(), lexer::token_type::LITERAL_STRING);
    EXPECT_EQ(print(node), "(Constant 'hi')");
}

TEST(AstPrinter, AttributeRendersValueThenName) {
    const Attribute node(kSpan, std::make_unique<Name>(kSpan, "self"), "count");
    EXPECT_EQ(print(node), "(Attribute (Name self) count)");
}

TEST(AstPrinter, SubscriptRendersValueThenIndex) {
    const Subscript node(kSpan, std::make_unique<Name>(kSpan, "items"),
                         std::make_unique<Constant>(kSpan, lexer::token_type::LITERAL_INT, "0"));
    EXPECT_EQ(print(node), "(Subscript (Name items) (Constant 0))");
}

TEST(AstPrinter, BinOpRendersOperatorSpellingThenOperands) {
    const BinOp node(kSpan, lexer::token_type::OP_PLUS, std::make_unique<Name>(kSpan, "x"),
                     std::make_unique<Constant>(kSpan, lexer::token_type::LITERAL_INT, "1"));
    EXPECT_EQ(print(node), "(BinOp + (Name x) (Constant 1))");
}

TEST(AstPrinter, BinOpRendersMultiCharacterOperators) {
    const BinOp node(kSpan, lexer::token_type::OP_DOUBLE_SLASH,
                     std::make_unique<Name>(kSpan, "a"), std::make_unique<Name>(kSpan, "b"));
    EXPECT_EQ(print(node), "(BinOp // (Name a) (Name b))");
}

TEST(AstPrinter, UnaryOpRendersOperatorThenOperand) {
    const UnaryOp node(kSpan, lexer::token_type::OP_MINUS, std::make_unique<Name>(kSpan, "x"));
    EXPECT_EQ(print(node), "(UnaryOp - (Name x))");
}

TEST(AstPrinter, BoolOpRendersAWordShapedOperator) {
    std::vector<ExprPtr> values;
    values.push_back(std::make_unique<Name>(kSpan, "a"));
    values.push_back(std::make_unique<Name>(kSpan, "b"));
    const BoolOp node(kSpan, lexer::token_type::OP_AND, std::move(values));
    EXPECT_EQ(print(node), "(BoolOp and (Name a) (Name b))");
}

TEST(AstPrinter, CompareRendersEachOperatorWithItsOperand) {
    std::vector<Compare::Rest> rest;
    rest.push_back({lexer::token_type::OP_LESS, std::make_unique<Name>(kSpan, "b")});
    rest.push_back({lexer::token_type::OP_LESS, std::make_unique<Name>(kSpan, "c")});
    const Compare node(kSpan, std::make_unique<Name>(kSpan, "a"), std::move(rest));
    EXPECT_EQ(print(node), "(Compare (Name a) < (Name b) < (Name c))");
}

TEST(AstPrinter, CallWithNoArgumentsRendersJustTheCallee) {
    const Call node(kSpan, std::make_unique<Name>(kSpan, "f"), {});
    EXPECT_EQ(print(node), "(Call (Name f))");
}

TEST(AstPrinter, CallRendersItsArgumentsInOrder) {
    std::vector<ExprPtr> args;
    args.push_back(std::make_unique<Name>(kSpan, "x"));
    args.push_back(std::make_unique<Constant>(kSpan, lexer::token_type::LITERAL_INT, "2"));
    const Call node(kSpan, std::make_unique<Name>(kSpan, "f"), std::move(args));
    EXPECT_EQ(print(node), "(Call (Name f) (Name x) (Constant 2))");
}

TEST(AstPrinter, EmptyListRendersWithNoElements) {
    const ListExpr node(kSpan, {});
    EXPECT_EQ(print(node), "(ListExpr)");
}

TEST(AstPrinter, ListRendersItsElements) {
    std::vector<ExprPtr> elements;
    elements.push_back(std::make_unique<Constant>(kSpan, lexer::token_type::LITERAL_INT, "1"));
    elements.push_back(std::make_unique<Constant>(kSpan, lexer::token_type::LITERAL_INT, "2"));
    const ListExpr node(kSpan, std::move(elements));
    EXPECT_EQ(print(node), "(ListExpr (Constant 1) (Constant 2))");
}

TEST(AstPrinter, TupleRendersItsElements) {
    std::vector<ExprPtr> elements;
    elements.push_back(std::make_unique<Name>(kSpan, "a"));
    const TupleExpr node(kSpan, std::move(elements));
    EXPECT_EQ(print(node), "(TupleExpr (Name a))");
}

TEST(AstPrinter, DictRendersEachKeyValuePair) {
    std::vector<DictExpr::Entry> entries;
    entries.push_back({std::make_unique<Constant>(kSpan, lexer::token_type::LITERAL_STRING, "'k'"),
                       std::make_unique<Constant>(kSpan, lexer::token_type::LITERAL_INT, "1")});
    const DictExpr node(kSpan, std::move(entries));
    EXPECT_EQ(print(node), "(DictExpr ((Constant 'k') (Constant 1)))");
}

TEST(AstPrinter, ListCompRendersElementThenClause) {
    std::vector<ComprehensionClause> clauses;
    clauses.push_back(ComprehensionClause{std::make_unique<Name>(kSpan, "x"),
                                          std::make_unique<Name>(kSpan, "items"),
                                          {}});
    const ListComp node(kSpan, std::make_unique<Name>(kSpan, "x"), std::move(clauses));
    EXPECT_EQ(print(node), "(ListComp (Name x) (Clause (Name x) (Name items)))");
}

TEST(AstPrinter, ListCompRendersClauseConditions) {
    std::vector<ExprPtr> conditions;
    conditions.push_back(std::make_unique<Name>(kSpan, "keep"));

    std::vector<ComprehensionClause> clauses;
    clauses.push_back(ComprehensionClause{std::make_unique<Name>(kSpan, "x"),
                                          std::make_unique<Name>(kSpan, "items"),
                                          std::move(conditions)});
    const ListComp node(kSpan, std::make_unique<Name>(kSpan, "x"), std::move(clauses));
    EXPECT_EQ(print(node), "(ListComp (Name x) (Clause (Name x) (Name items) (Name keep)))");
}

TEST(Node, CarriesTheSpanItWasBuiltWith) {
    const Name node(SourceSpan{4, 9, 4, 14}, "total");
    EXPECT_EQ(node.span(), (SourceSpan{4, 9, 4, 14}));
}

} // namespace
} // namespace cythonpp::domain::ast
