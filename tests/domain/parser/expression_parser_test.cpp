#include <gtest/gtest.h>

#include <string>

#include "domain/ast/constant.h"
#include "domain/ast/source_span.h"
#include "domain/lexer/token_type.h"
#include "parse_test_helpers.h"

namespace cythonpp::domain::parser {
namespace {

using test_support::parse;
using test_support::printed;
using test_support::printed_list;
using test_support::printed_target;

// The token_type of a root Constant. AstPrinter erases it, so a literal-kind
// misclassification is invisible in the printed form alone.
lexer::token_type constant_type(const std::string& source) {
    const test_support::ParseResult result = parse(source);
    EXPECT_TRUE(result.succeeded()) << "parse failed: " << source;
    const auto* constant = dynamic_cast<const ast::Constant*>(result.expression.get());
    EXPECT_NE(constant, nullptr) << "root was not a Constant: " << source;
    return constant == nullptr ? lexer::token_type::TOKEN_ERROR : constant->type();
}

TEST(ExpressionParser, IdentifierBecomesAName) {
    EXPECT_EQ(printed("total"), "(Name total)");
}

TEST(ExpressionParser, LiteralsKeepTheirRawLexeme) {
    EXPECT_EQ(printed("42"), "(Constant 42)");
    EXPECT_EQ(printed("0xFF"), "(Constant 0xFF)");
    EXPECT_EQ(printed("1_000"), "(Constant 1_000)");
    EXPECT_EQ(printed("3.5"), "(Constant 3.5)");
    EXPECT_EQ(printed("'hi'"), "(Constant 'hi')");
}

TEST(ExpressionParser, LiteralsKeepTheirLexerTokenType) {
    EXPECT_EQ(constant_type("42"), lexer::token_type::LITERAL_INT);
    EXPECT_EQ(constant_type("3.5"), lexer::token_type::LITERAL_FLOAT);
    EXPECT_EQ(constant_type("1j"), lexer::token_type::LITERAL_COMPLEX);
    EXPECT_EQ(constant_type("'hi'"), lexer::token_type::LITERAL_STRING);
    EXPECT_EQ(constant_type("b'hi'"), lexer::token_type::LITERAL_BYTES);
    EXPECT_EQ(constant_type("True"), lexer::token_type::BOOL_TRUE);
    EXPECT_EQ(constant_type("False"), lexer::token_type::BOOL_FALSE);
    EXPECT_EQ(constant_type("None"), lexer::token_type::KEYWORD_NONE);
    EXPECT_EQ(constant_type("..."), lexer::token_type::ELLIPSIS);
}

TEST(ExpressionParser, SingletonsPrintTheirSpelling) {
    EXPECT_EQ(printed("True"), "(Constant True)");
    EXPECT_EQ(printed("None"), "(Constant None)");
    EXPECT_EQ(printed("..."), "(Constant ...)");
}

TEST(ExpressionParser, SoftKeywordsAreOrdinaryNames) {
    // match/case/_ are reserved only inside a match statement, which is not
    // in the supported subset. In expression position they are just names,
    // and the scanner already emits IDENTIFIER for all three.
    EXPECT_EQ(printed("match"), "(Name match)");
    EXPECT_EQ(printed("case"), "(Name case)");
    EXPECT_EQ(printed("_"), "(Name _)");
}

TEST(ExpressionParser, BuiltinTypeNamesOutsideAnnotationsAreNames) {
    EXPECT_EQ(printed("int"), "(Name int)");
    EXPECT_EQ(printed("str"), "(Name str)");
}

TEST(ExpressionParser, PrefixOperatorsBuildUnaryOps) {
    EXPECT_EQ(printed("-x"), "(UnaryOp - (Name x))");
    EXPECT_EQ(printed("+x"), "(UnaryOp + (Name x))");
    EXPECT_EQ(printed("~mask"), "(UnaryOp ~ (Name mask))");
}

TEST(ExpressionParser, PrefixOperatorsNest) {
    EXPECT_EQ(printed("- -x"), "(UnaryOp - (UnaryOp - (Name x)))");
}

TEST(ExpressionParser, PowerIsRightAssociative) {
    EXPECT_EQ(printed("2 ** 3 ** 2"),
              "(BinOp ** (Constant 2) (BinOp ** (Constant 3) (Constant 2)))");
}

TEST(ExpressionParser, PowerBindsTighterThanUnaryOnItsLeft) {
    // -2**2 is -4 in Python, not 4.
    EXPECT_EQ(printed("-2 ** 2"), "(UnaryOp - (BinOp ** (Constant 2) (Constant 2)))");
}

TEST(ExpressionParser, PowerAllowsAUnaryOperatorOnItsRight) {
    EXPECT_EQ(printed("2 ** -1"), "(BinOp ** (Constant 2) (UnaryOp - (Constant 1)))");
}

TEST(ExpressionParser, MultiplicationBindsTighterThanAddition) {
    EXPECT_EQ(printed("a + b * c"), "(BinOp + (Name a) (BinOp * (Name b) (Name c)))");
    EXPECT_EQ(printed("a * b + c"), "(BinOp + (BinOp * (Name a) (Name b)) (Name c))");
}

TEST(ExpressionParser, SameLevelOperatorsAreLeftAssociative) {
    EXPECT_EQ(printed("a - b - c"), "(BinOp - (BinOp - (Name a) (Name b)) (Name c))");
    EXPECT_EQ(printed("a / b // c"), "(BinOp // (BinOp / (Name a) (Name b)) (Name c))");
}

TEST(ExpressionParser, EveryLevelBoundaryGroupsCorrectly) {
    EXPECT_EQ(printed("a | b ^ c"), "(BinOp | (Name a) (BinOp ^ (Name b) (Name c)))");
    EXPECT_EQ(printed("a ^ b & c"), "(BinOp ^ (Name a) (BinOp & (Name b) (Name c)))");
    EXPECT_EQ(printed("a & b << c"), "(BinOp & (Name a) (BinOp << (Name b) (Name c)))");
    EXPECT_EQ(printed("a << b + c"), "(BinOp << (Name a) (BinOp + (Name b) (Name c)))");
    EXPECT_EQ(printed("a + b % c"), "(BinOp + (Name a) (BinOp % (Name b) (Name c)))");
}

TEST(ExpressionParser, AtIsMatrixMultiplyInExpressionPosition) {
    EXPECT_EQ(printed("a @ b"), "(BinOp @ (Name a) (Name b))");
}

TEST(ExpressionParser, PowerBindsTighterThanEveryTableLevel) {
    EXPECT_EQ(printed("a * b ** c"), "(BinOp * (Name a) (BinOp ** (Name b) (Name c)))");
}

TEST(ExpressionParser, UnaryBindsTighterThanBinary) {
    EXPECT_EQ(printed("-a + b"), "(BinOp + (UnaryOp - (Name a)) (Name b))");
}

TEST(ExpressionParser, AssignmentOperatorsEndAnExpressionRatherThanJoiningIt) {
    // OP_ASSIGN and the aug-assigns carry token_category::OPERATOR, so a
    // parser that trusted the flag instead of the table would build a BinOp
    // here. The expression is just `a`, and `=` is left for Spec 4.
    EXPECT_EQ(printed("a = b"), "(Name a)");
    EXPECT_EQ(printed("a += b"), "(Name a)");
    EXPECT_EQ(printed("a -> b"), "(Name a)");
}

TEST(ExpressionParser, ASingleComparisonBuildsACompare) {
    EXPECT_EQ(printed("a < b"), "(Compare (Name a) < (Name b))");
    EXPECT_EQ(printed("a != b"), "(Compare (Name a) != (Name b))");
}

TEST(ExpressionParser, ChainedComparisonsStayInOneNode) {
    // Kept intact rather than desugared to `a < b and b <= c`, so a later
    // stage can evaluate the middle operand once.
    EXPECT_EQ(printed("a < b <= c"), "(Compare (Name a) < (Name b) <= (Name c))");
    EXPECT_EQ(printed("a < b < c < d"),
              "(Compare (Name a) < (Name b) < (Name c) < (Name d))");
}

TEST(ExpressionParser, WordShapedComparisonOperators) {
    EXPECT_EQ(printed("a is b"), "(Compare (Name a) is (Name b))");
    EXPECT_EQ(printed("a in b"), "(Compare (Name a) in (Name b))");
}

TEST(ExpressionParser, TwoWordComparisonOperatorsFoldIntoOneOperator) {
    EXPECT_EQ(printed("a is not b"), "(Compare (Name a) is not (Name b))");
    EXPECT_EQ(printed("a not in b"), "(Compare (Name a) not in (Name b))");
}

TEST(ExpressionParser, TwoWordOperatorsChainToo) {
    EXPECT_EQ(printed("a not in b not in c"),
              "(Compare (Name a) not in (Name b) not in (Name c))");
}

TEST(ExpressionParser, ComparisonBindsLooserThanArithmetic) {
    EXPECT_EQ(printed("a + 1 < b * 2"),
              "(Compare (BinOp + (Name a) (Constant 1)) < (BinOp * (Name b) (Constant 2)))");
}

TEST(ExpressionParser, NotBindsLooserThanComparison) {
    // `not a == b` is `not (a == b)`, which is why `not` is its own level and
    // not grouped with the `-` and `~` prefix operators.
    EXPECT_EQ(printed("not a == b"), "(UnaryOp not (Compare (Name a) == (Name b)))");
}

TEST(ExpressionParser, NotNests) {
    EXPECT_EQ(printed("not not a"), "(UnaryOp not (UnaryOp not (Name a)))");
}

TEST(ExpressionParser, ARunOfAndIsOneFlattenedNode) {
    // One node rather than nested pairs, because `a and b and c` short-
    // circuits as a single left-to-right sequence and codegen wants that.
    EXPECT_EQ(printed("a and b and c"), "(BoolOp and (Name a) (Name b) (Name c))");
    EXPECT_EQ(printed("a or b or c"), "(BoolOp or (Name a) (Name b) (Name c))");
}

TEST(ExpressionParser, AndBindsTighterThanOr) {
    EXPECT_EQ(printed("a or b and c"), "(BoolOp or (Name a) (BoolOp and (Name b) (Name c)))");
    EXPECT_EQ(printed("a and b or c"), "(BoolOp or (BoolOp and (Name a) (Name b)) (Name c))");
}

TEST(ExpressionParser, NotBindsTighterThanAnd) {
    EXPECT_EQ(printed("not a and b"), "(BoolOp and (UnaryOp not (Name a)) (Name b))");
}

TEST(ExpressionParser, BooleanOperatorsBindLooserThanComparison) {
    EXPECT_EQ(printed("a < b and c"),
              "(BoolOp and (Compare (Name a) < (Name b)) (Name c))");
}

TEST(ExpressionParser, ParenthesesGroupWithoutBuildingANode) {
    EXPECT_EQ(printed("(a)"), "(Name a)");
    EXPECT_EQ(printed("(a + b) * c"),
              "(BinOp * (BinOp + (Name a) (Name b)) (Name c))");
}

TEST(ExpressionParser, GroupingLeavesTheInnerSpanAlone) {
    // Nodes are immutable, so widening the span to cover the parentheses
    // would mean rebuilding the subtree for a distinction nothing downstream
    // can use. `a` in `(a)` is at column 2, ending at column 3.
    const test_support::ParseResult result = parse("(a)");
    ASSERT_TRUE(result.succeeded());
    EXPECT_EQ(result.span(), (ast::SourceSpan{1, 2, 1, 3}));
}

TEST(ExpressionParser, ATrailingCommaMakesAOneElementTuple) {
    EXPECT_EQ(printed("(a,)"), "(TupleExpr (Name a))");
    // Unlike grouping, the parentheses genuinely create this node, so it
    // spans them: columns 1 through 5.
    const test_support::ParseResult result = parse("(a,)");
    ASSERT_TRUE(result.succeeded());
    EXPECT_EQ(result.span(), (ast::SourceSpan{1, 1, 1, 5}));
}

TEST(ExpressionParser, EmptyParenthesesAreTheEmptyTuple) {
    EXPECT_EQ(printed("()"), "(TupleExpr)");
}

TEST(ExpressionParser, ParenthesisedTuples) {
    EXPECT_EQ(printed("(a, b)"), "(TupleExpr (Name a) (Name b))");
    EXPECT_EQ(printed("(a, b, c)"), "(TupleExpr (Name a) (Name b) (Name c))");
    EXPECT_EQ(printed("(a, b,)"), "(TupleExpr (Name a) (Name b))");
}

TEST(ExpressionParser, ABareCommaListIsATuple) {
    EXPECT_EQ(printed_list("a, b"), "(TupleExpr (Name a) (Name b))");
    EXPECT_EQ(printed_list("a,"), "(TupleExpr (Name a))");
}

TEST(ExpressionParser, AListOfOneWithNoCommaIsNotATuple) {
    EXPECT_EQ(printed_list("a"), "(Name a)");
}

TEST(ExpressionParser, AttributeAccess) {
    EXPECT_EQ(printed("self.count"), "(Attribute (Name self) count)");
    EXPECT_EQ(printed("a.b.c"), "(Attribute (Attribute (Name a) b) c)");
}

TEST(ExpressionParser, Subscripting) {
    EXPECT_EQ(printed("items[0]"), "(Subscript (Name items) (Constant 0))");
    EXPECT_EQ(printed("grid[a, b]"),
              "(Subscript (Name grid) (TupleExpr (Name a) (Name b)))");
}

TEST(ExpressionParser, Calls) {
    EXPECT_EQ(printed("f()"), "(Call (Name f))");
    EXPECT_EQ(printed("f(x)"), "(Call (Name f) (Name x))");
    EXPECT_EQ(printed("f(x, 2)"), "(Call (Name f) (Name x) (Constant 2))");
    EXPECT_EQ(printed("f(x,)"), "(Call (Name f) (Name x))");
}

TEST(ExpressionParser, TrailersChainInSourceOrder) {
    EXPECT_EQ(printed("a.b[0](c).d"),
              "(Attribute (Call (Subscript (Attribute (Name a) b) (Constant 0)) (Name c)) d)");
}

TEST(ExpressionParser, TrailersBindTighterThanEveryOperator) {
    EXPECT_EQ(printed("-f(x)"), "(UnaryOp - (Call (Name f) (Name x)))");
    EXPECT_EQ(printed("a.b + c"), "(BinOp + (Attribute (Name a) b) (Name c))");
    EXPECT_EQ(printed("f(x) ** 2"),
              "(BinOp ** (Call (Name f) (Name x)) (Constant 2))");
}

TEST(ExpressionParser, SoftKeywordsWorkAsNamesUnderTrailersToo) {
    // The shapes that would break if `match` or `case` were ever reserved at
    // the token level rather than at statement level.
    EXPECT_EQ(printed("match(x)"), "(Call (Name match) (Name x))");
    EXPECT_EQ(printed("case.value"), "(Attribute (Name case) value)");
    EXPECT_EQ(printed("_[0]"), "(Subscript (Name _) (Constant 0))");
}

TEST(ExpressionParser, ArgumentsAreFullExpressions) {
    EXPECT_EQ(printed("f(a + b, c or d)"),
              "(Call (Name f) (BinOp + (Name a) (Name b)) (BoolOp or (Name c) (Name d)))");
}

TEST(ExpressionParser, BuiltinTypeNamesInAnnotationPositionAlsoBecomeNames) {
    // The scanner spells `int` as TYPE_INT only in annotation position, which
    // needs a statement the expression parser cannot consume. Parsing from
    // the token after the ':' proves both spellings reach the same node.
    // Tokens: x(0) :(1) list(2) [(3) int(4) ](5) =(6) [(7) ](8)
    const test_support::ParseResult result = test_support::parse_from("x: list[int] = []", 2);
    ASSERT_TRUE(result.succeeded());
    EXPECT_EQ(result.printed(), "(Subscript (Name list) (Name int))");
}

TEST(ExpressionParser, ListDisplays) {
    EXPECT_EQ(printed("[]"), "(ListExpr)");
    EXPECT_EQ(printed("[1]"), "(ListExpr (Constant 1))");
    EXPECT_EQ(printed("[1, 2, 3]"),
              "(ListExpr (Constant 1) (Constant 2) (Constant 3))");
    EXPECT_EQ(printed("[1, 2,]"), "(ListExpr (Constant 1) (Constant 2))");
}

TEST(ExpressionParser, ASimpleListComprehension) {
    EXPECT_EQ(printed("[x for x in items]"),
              "(ListComp (Name x) (Clause (Name x) (Name items)))");
}

TEST(ExpressionParser, AComprehensionWithAConditon) {
    EXPECT_EQ(printed("[x * 2 for x in items if x > 0]"),
              "(ListComp (BinOp * (Name x) (Constant 2))"
              " (Clause (Name x) (Name items) (Compare (Name x) > (Constant 0))))");
}

TEST(ExpressionParser, AComprehensionWithSeveralConditions) {
    EXPECT_EQ(printed("[x for x in items if a if b]"),
              "(ListComp (Name x) (Clause (Name x) (Name items) (Name a) (Name b)))");
}

TEST(ExpressionParser, AComprehensionWithSeveralForClauses) {
    // The first n>1 rendering of ListComp's (Clause ...) sequence.
    EXPECT_EQ(printed("[x for row in grid for x in row]"),
              "(ListComp (Name x) (Clause (Name row) (Name grid))"
              " (Clause (Name x) (Name row)))");
}

TEST(ExpressionParser, AComprehensionTargetMayBeATuple) {
    EXPECT_EQ(printed("[k for k, v in pairs]"),
              "(ListComp (Name k) (Clause (TupleExpr (Name k) (Name v)) (Name pairs)))");
}

TEST(ExpressionParser, TargetsAreRestrictedToPostfixExpressions) {
    // `in` is a comparison operator, so parsing a target with the full
    // grammar would swallow `x in y` as a Compare. This grammar has no `in`
    // in it and halts before the keyword.
    EXPECT_EQ(printed_target("x"), "(Name x)");
    EXPECT_EQ(printed_target("a.b"), "(Attribute (Name a) b)");
    EXPECT_EQ(printed_target("a[0]"), "(Subscript (Name a) (Constant 0))");
    EXPECT_EQ(printed_target("a, b"), "(TupleExpr (Name a) (Name b))");
}

TEST(ExpressionParser, TargetParsingStopsBeforeIn) {
    const test_support::ParseResult result =
        test_support::parse_from("x in y", 0, test_support::Entry::Target);
    ASSERT_TRUE(result.succeeded());
    EXPECT_EQ(result.printed(), "(Name x)");
}

} // namespace
} // namespace cythonpp::domain::parser
