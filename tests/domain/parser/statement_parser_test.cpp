#include <gtest/gtest.h>

#include "domain/ast/ann_assign.h"
#include "domain/ast/constant.h"
#include "domain/ast/expr_stmt.h"
#include "domain/ast/for.h"
#include "domain/ast/function_def.h"
#include "domain/ast/name.h"
#include "domain/ast/return.h"
#include "domain/lexer/token_type.h"
#include "statement_parse_test_helpers.h"

namespace cythonpp::domain::parser {
namespace {

using statement_test_support::parse_module;
using statement_test_support::printed;

TEST(StatementParser, AnEmptyFileIsAnEmptyModule) {
    EXPECT_EQ(printed(""), "(Module)");
}

TEST(StatementParser, ACommentOnlyFileIsAnEmptyModule) {
    // A comment-only line emits no NEWLINE and no indentation, so it is
    // invisible as a line boundary. The module must still come back empty
    // rather than the parser tripping over the COMMENT_SINGLE token.
    EXPECT_EQ(printed("# nothing here\n"), "(Module)");
}

TEST(StatementParser, ParsesPass) {
    EXPECT_EQ(printed("pass\n"), "(Module\n  (Pass))");
}

TEST(StatementParser, ParsesBreakAndContinue) {
    EXPECT_EQ(printed("break\n"), "(Module\n  (Break))");
    EXPECT_EQ(printed("continue\n"), "(Module\n  (Continue))");
}

TEST(StatementParser, ParsesSeveralStatementsInSourceOrder) {
    EXPECT_EQ(printed("pass\nbreak\ncontinue\n"),
              "(Module\n  (Pass)\n  (Break)\n  (Continue))");
}

TEST(StatementParser, AFileWithNoTrailingNewlineStillParses) {
    EXPECT_EQ(printed("pass"), "(Module\n  (Pass))");
}

TEST(StatementParser, SemicolonsSeparateStatementsOnOneLine) {
    EXPECT_EQ(printed("pass; break; continue\n"),
              "(Module\n  (Pass)\n  (Break)\n  (Continue))");
}

TEST(StatementParser, ATrailingSemicolonIsAllowed) {
    EXPECT_EQ(printed("pass;\n"), "(Module\n  (Pass))");
}

TEST(StatementParser, BlankLinesBetweenStatementsAreInvisible) {
    EXPECT_EQ(printed("pass\n\n\nbreak\n"), "(Module\n  (Pass)\n  (Break))");
}

TEST(StatementParser, ACommentBetweenStatementsIsInvisible) {
    EXPECT_EQ(printed("pass\n# why\nbreak\n"), "(Module\n  (Pass)\n  (Break))");
}

TEST(StatementParser, ParsesABareReturn) {
    EXPECT_EQ(printed("return\n"), "(Module\n  (Return))");
}

TEST(StatementParser, ParsesAReturnWithAValue) {
    EXPECT_EQ(printed("return 1\n"), "(Module\n  (Return (Constant 1)))");
}

TEST(StatementParser, ParsesAReturnOfATuple) {
    // parse_expression_list rather than parse_expression: `return a, b`
    // returns one tuple, not the first of two values.
    EXPECT_EQ(printed("return a, b\n"),
              "(Module\n  (Return (TupleExpr (Name a) (Name b))))");
}

TEST(StatementParser, ParsesAReturnOfAnOperatorExpression) {
    EXPECT_EQ(printed("return a + b * 2\n"),
              "(Module\n  (Return (BinOp + (Name a) (BinOp * (Name b) (Constant 2)))))");
}

TEST(StatementParser, AReturnCanShareALineViaASemicolon) {
    EXPECT_EQ(printed("pass; return 1\n"),
              "(Module\n  (Pass)\n  (Return (Constant 1)))");
}

TEST(StatementParser, ABareReturnHasNoValue) {
    const statement_test_support::ModuleResult result = parse_module("return\n");
    ASSERT_NE(result.module, nullptr);
    ASSERT_EQ(result.module->body().size(), 1u);
    const auto* returned = dynamic_cast<const ast::Return*>(result.module->body().front().get());
    ASSERT_NE(returned, nullptr);
    // value() dereferences unconditionally, so has_value() is the only
    // supported way to ask -- calling value() here would dereference null.
    EXPECT_FALSE(returned->has_value());
}

TEST(StatementParser, ACallOnItsOwnIsAnExprStmt) {
    EXPECT_EQ(printed("print(x)\n"),
              "(Module\n  (ExprStmt (Call (Name print) (Name x))))");
}

TEST(StatementParser, AMethodCallOnItsOwnIsAnExprStmt) {
    EXPECT_EQ(printed("items.append(x)\n"),
              "(Module\n  (ExprStmt (Call (Attribute (Name items) append) (Name x))))");
}

TEST(StatementParser, ADocstringIsAnExprStmtOfAStringConstant) {
    EXPECT_EQ(printed("'a docstring'\n"),
              "(Module\n  (ExprStmt (Constant 'a docstring')))");
}

TEST(StatementParser, ParsesASimpleAssignment) {
    EXPECT_EQ(printed("x = 1\n"), "(Module\n  (Assign (Name x) (Constant 1)))");
}

TEST(StatementParser, ParsesAssignmentToAnAttributeAndASubscript) {
    EXPECT_EQ(printed("obj.field = 1\n"),
              "(Module\n  (Assign (Attribute (Name obj) field) (Constant 1)))");
    EXPECT_EQ(printed("items[0] = 1\n"),
              "(Module\n  (Assign (Subscript (Name items) (Constant 0)) (Constant 1)))");
}

TEST(StatementParser, ParsesTupleUnpacking) {
    EXPECT_EQ(printed("x, y = 1, 2\n"),
              "(Module\n  (Assign (TupleExpr (Name x) (Name y))"
              " (TupleExpr (Constant 1) (Constant 2))))");
}

TEST(StatementParser, AssigningATupleValueToOneNameKeepsTheTuple) {
    EXPECT_EQ(printed("x = 1, 2\n"),
              "(Module\n  (Assign (Name x) (TupleExpr (Constant 1) (Constant 2))))");
}

TEST(StatementParser, ParsesABareAnnotation) {
    EXPECT_EQ(printed("x: int\n"), "(Module\n  (AnnAssign (Name x) (Name int)))");
}

TEST(StatementParser, ParsesAnAnnotatedAssignment) {
    // `int` here lexes as TYPE_INT, not IDENTIFIER -- annotation position is
    // exactly what ScanContext switches on. The expression parser's atom rule
    // accepts both spellings via has_category(IDENTIFIER), so it becomes a
    // Name either way.
    EXPECT_EQ(printed("x: int = 5\n"),
              "(Module\n  (AnnAssign (Name x) (Name int) (Constant 5)))");
}

TEST(StatementParser, ParsesASubscriptedAnnotation) {
    EXPECT_EQ(printed("values: list[int] = []\n"),
              "(Module\n  (AnnAssign (Name values) (Subscript (Name list) (Name int))"
              " (ListExpr)))");
}

TEST(StatementParser, AnAnnotatedAssignmentValueCanBeATuple) {
    EXPECT_EQ(printed("x: tuple = 1, 2\n"),
              "(Module\n  (AnnAssign (Name x) (Name tuple)"
              " (TupleExpr (Constant 1) (Constant 2))))");
}

TEST(StatementParser, ABareAnnotationHasNoValue) {
    const statement_test_support::ModuleResult result = parse_module("x: int\n");
    ASSERT_NE(result.module, nullptr);
    ASSERT_EQ(result.module->body().size(), 1u);
    const auto* annotated =
        dynamic_cast<const ast::AnnAssign*>(result.module->body().front().get());
    ASSERT_NE(annotated, nullptr);
    EXPECT_FALSE(annotated->has_value());
}

TEST(StatementParser, AnExprStmtLiteralKeepsItsLexerTokenType) {
    // AstPrinter erases Constant::type(): LITERAL_INT "1" and LITERAL_FLOAT
    // "1" both render (Constant 1), so the printed form cannot catch a
    // literal-kind misclassification on its own.
    const statement_test_support::ModuleResult result = parse_module("3.5\n");
    ASSERT_NE(result.module, nullptr);
    ASSERT_EQ(result.module->body().size(), 1u);
    const auto* statement = dynamic_cast<const ast::ExprStmt*>(result.module->body().front().get());
    ASSERT_NE(statement, nullptr);
    const auto* constant = dynamic_cast<const ast::Constant*>(&statement->value());
    ASSERT_NE(constant, nullptr);
    EXPECT_EQ(constant->type(), lexer::token_type::LITERAL_FLOAT);
}

TEST(StatementParser, ParsesAnIfWithABlockBody) {
    EXPECT_EQ(printed("if x:\n    pass\n"),
              "(Module\n  (If (Name x)\n    (Pass)))");
}

TEST(StatementParser, ParsesAnIfWithAOneLineBody) {
    // The guard-clause form. Same production as a semicolon line.
    EXPECT_EQ(printed("if x: return 1\n"),
              "(Module\n  (If (Name x)\n    (Return (Constant 1))))");
}

TEST(StatementParser, ParsesAOneLineBodyWithSeveralStatements) {
    EXPECT_EQ(printed("if x: pass; break\n"),
              "(Module\n  (If (Name x)\n    (Pass)\n    (Break)))");
}

TEST(StatementParser, ParsesAnIfElse) {
    EXPECT_EQ(printed("if x:\n    pass\nelse:\n    break\n"),
              "(Module\n  (If (Name x)\n    (Pass)\n    (Else\n      (Break))))");
}

TEST(StatementParser, ElifNestsAsAnIfInsideTheOuterOrelse) {
    EXPECT_EQ(printed("if a:\n    pass\nelif b:\n    break\n"),
              "(Module\n  (If (Name a)\n    (Pass)\n    (Else\n      (If (Name b)\n"
              "        (Break)))))");
}

TEST(StatementParser, ParsesAFullElifChain) {
    EXPECT_EQ(printed("if a:\n    pass\nelif b:\n    break\nelse:\n    continue\n"),
              "(Module\n  (If (Name a)\n    (Pass)\n    (Else\n      (If (Name b)\n"
              "        (Break)\n        (Else\n          (Continue))))))");
}

TEST(StatementParser, ParsesNestedIfs) {
    EXPECT_EQ(printed("if a:\n    if b:\n        pass\n"),
              "(Module\n  (If (Name a)\n    (If (Name b)\n      (Pass))))");
}

TEST(StatementParser, AnIfConditionCanBeAComparison) {
    EXPECT_EQ(printed("if a != b:\n    pass\n"),
              "(Module\n  (If (Compare (Name a) != (Name b))\n    (Pass)))");
}

TEST(StatementParser, StatementsAfterABlockReturnToTheOuterLevel) {
    EXPECT_EQ(printed("if a:\n    pass\nbreak\n"),
              "(Module\n  (If (Name a)\n    (Pass))\n  (Break))");
}

TEST(StatementParser, ParsesAWhileLoop) {
    EXPECT_EQ(printed("while x:\n    pass\n"),
              "(Module\n  (While (Name x)\n    (Pass)))");
}

TEST(StatementParser, ParsesAWhileElse) {
    // Python runs the else when the loop finishes without hitting a break.
    EXPECT_EQ(printed("while x:\n    break\nelse:\n    pass\n"),
              "(Module\n  (While (Name x)\n    (Break)\n    (Else\n      (Pass))))");
}

TEST(StatementParser, ParsesAForLoop) {
    EXPECT_EQ(printed("for x in items:\n    pass\n"),
              "(Module\n  (For (Name x) (Name items)\n    (Pass)))");
}

TEST(StatementParser, ParsesAForWithATupleTarget) {
    EXPECT_EQ(printed("for i, item in pairs:\n    pass\n"),
              "(Module\n  (For (TupleExpr (Name i) (Name item)) (Name pairs)\n    (Pass)))");
}

TEST(StatementParser, ParsesAForOverACall) {
    EXPECT_EQ(printed("for i in range(10):\n    pass\n"),
              "(Module\n  (For (Name i) (Call (Name range) (Constant 10))\n    (Pass)))");
}

TEST(StatementParser, ParsesAForElse) {
    EXPECT_EQ(printed("for x in items:\n    pass\nelse:\n    break\n"),
              "(Module\n  (For (Name x) (Name items)\n    (Pass)\n    (Else\n      (Break))))");
}

TEST(StatementParser, AForTargetIsNotSwallowedAsAComparison) {
    // The reason parse_target exists. With the full expression grammar this
    // would parse `x in items` as one Compare and then find no `in`.
    const statement_test_support::ModuleResult result =
        parse_module("for x in items:\n    pass\n");
    ASSERT_NE(result.module, nullptr);
    ASSERT_EQ(result.module->body().size(), 1u);
    const auto* loop = dynamic_cast<const ast::For*>(result.module->body().front().get());
    ASSERT_NE(loop, nullptr);
    EXPECT_NE(dynamic_cast<const ast::Name*>(&loop->target()), nullptr);
}

TEST(StatementParser, ParsesALoopBodyWithSeveralStatements) {
    EXPECT_EQ(printed("while x:\n    a = 1\n    break\n"),
              "(Module\n  (While (Name x)\n    (Assign (Name a) (Constant 1))\n    (Break)))");
}

TEST(StatementParser, ParsesADefWithNoParameters) {
    EXPECT_EQ(printed("def f():\n    pass\n"),
              "(Module\n  (FunctionDef f\n    (Pass)))");
}

TEST(StatementParser, ParsesADefWithOneParameter) {
    EXPECT_EQ(printed("def f(a):\n    pass\n"),
              "(Module\n  (FunctionDef f (Params (Parameter a))\n    (Pass)))");
}

TEST(StatementParser, ParsesADefWithSeveralParameters) {
    // AstPrinter's (Parameter ...) sequence has only ever been exercised at
    // n=1; this is the first time anything builds it at n>1.
    EXPECT_EQ(printed("def f(a, b, c):\n    pass\n"),
              "(Module\n  (FunctionDef f (Params (Parameter a) (Parameter b) (Parameter c))\n"
              "    (Pass)))");
}

TEST(StatementParser, ParsesAnnotatedParameters) {
    EXPECT_EQ(printed("def f(a: int, b: str):\n    pass\n"),
              "(Module\n  (FunctionDef f (Params (Parameter a (Name int))"
              " (Parameter b (Name str)))\n    (Pass)))");
}

TEST(StatementParser, ParsesDefaultedParameters) {
    EXPECT_EQ(printed("def f(a=1, b=2):\n    pass\n"),
              "(Module\n  (FunctionDef f (Params (Parameter a (Default (Constant 1)))"
              " (Parameter b (Default (Constant 2))))\n    (Pass)))");
}

TEST(StatementParser, ParsesAnnotatedAndDefaultedParameters) {
    EXPECT_EQ(printed("def f(a: int = 1):\n    pass\n"),
              "(Module\n  (FunctionDef f (Params (Parameter a (Name int) (Default (Constant 1))))"
              "\n    (Pass)))");
}

TEST(StatementParser, ParsesAReturnAnnotation) {
    EXPECT_EQ(printed("def f() -> int:\n    pass\n"),
              "(Module\n  (FunctionDef f (Returns (Name int))\n    (Pass)))");
}

TEST(StatementParser, ParsesADefWithEverything) {
    EXPECT_EQ(printed("def add(a: int, b: int = 0) -> int:\n    return a + b\n"),
              "(Module\n  (FunctionDef add (Params (Parameter a (Name int))"
              " (Parameter b (Name int) (Default (Constant 0)))) (Returns (Name int))\n"
              "    (Return (BinOp + (Name a) (Name b)))))");
}

TEST(StatementParser, ParsesADefWithAOneLineBody) {
    EXPECT_EQ(printed("def f(): pass\n"), "(Module\n  (FunctionDef f\n    (Pass)))");
}

TEST(StatementParser, ParsesATrailingCommaInAParameterList) {
    EXPECT_EQ(printed("def f(a, b,):\n    pass\n"),
              "(Module\n  (FunctionDef f (Params (Parameter a) (Parameter b))\n    (Pass)))");
}

TEST(StatementParser, ADefWithNoReturnAnnotationSaysSo) {
    const statement_test_support::ModuleResult result = parse_module("def f():\n    pass\n");
    ASSERT_NE(result.module, nullptr);
    ASSERT_EQ(result.module->body().size(), 1u);
    const auto* function =
        dynamic_cast<const ast::FunctionDef*>(result.module->body().front().get());
    ASSERT_NE(function, nullptr);
    // return_annotation() dereferences unconditionally; has_return_annotation
    // is the only supported way to ask.
    EXPECT_FALSE(function->has_return_annotation());
    EXPECT_TRUE(function->params().empty());
}

TEST(StatementParser, ParsesNestedDefs) {
    EXPECT_EQ(printed("def outer():\n    def inner():\n        pass\n"),
              "(Module\n  (FunctionDef outer\n    (FunctionDef inner\n      (Pass))))");
}

TEST(StatementParser, ParsesAClassWithNoBases) {
    EXPECT_EQ(printed("class C:\n    pass\n"), "(Module\n  (ClassDef C\n    (Pass)))");
}

TEST(StatementParser, ParsesAClassWithOneBase) {
    // ScanContext keeps `int` an IDENTIFIER here rather than TYPE_INT, which
    // is exactly the case class-base handling exists for.
    EXPECT_EQ(printed("class C(int):\n    pass\n"),
              "(Module\n  (ClassDef C (Bases (Name int))\n    (Pass)))");
}

TEST(StatementParser, ParsesAClassWithSeveralBases) {
    EXPECT_EQ(printed("class C(A, B):\n    pass\n"),
              "(Module\n  (ClassDef C (Bases (Name A) (Name B))\n    (Pass)))");
}

TEST(StatementParser, ParsesASubscriptedBase) {
    EXPECT_EQ(printed("class C(Generic[T]):\n    pass\n"),
              "(Module\n  (ClassDef C (Bases (Subscript (Name Generic) (Name T)))\n    (Pass)))");
}

TEST(StatementParser, ParsesAnEmptyBaseList) {
    EXPECT_EQ(printed("class C():\n    pass\n"), "(Module\n  (ClassDef C\n    (Pass)))");
}

TEST(StatementParser, ParsesAClassWithMethods) {
    EXPECT_EQ(printed("class C:\n    def m(self):\n        return 1\n"),
              "(Module\n  (ClassDef C\n    (FunctionDef m (Params (Parameter self))\n"
              "      (Return (Constant 1)))))");
}

TEST(StatementParser, ParsesAnAnnotatedClassAttribute) {
    EXPECT_EQ(printed("class C:\n    x: int = 0\n"),
              "(Module\n  (ClassDef C\n    (AnnAssign (Name x) (Name int) (Constant 0))))");
}

TEST(StatementParser, ParsesTheRepositorysOwnHelloWorldFile) {
    // The acceptance criterion for this spec. Two of these four statements
    // are bare expression statements, which is why ExprStmt had to exist
    // before any of this could parse. The file has no trailing newline, so
    // the last statement ends at TOKEN_EOF rather than at a NEWLINE.
    const std::string source =
        "print(\"Hello World\")\n"
        "\n"
        "string: str = \"Hello\"\n"
        "\n"
        "if string != \"hello\":\n"
        "    print(\"Something has happened to string\")";

    EXPECT_EQ(printed(source),
              "(Module\n"
              "  (ExprStmt (Call (Name print) (Constant \"Hello World\")))\n"
              "  (AnnAssign (Name string) (Name str) (Constant \"Hello\"))\n"
              "  (If (Compare (Name string) != (Constant \"hello\"))\n"
              "    (ExprStmt (Call (Name print)"
              " (Constant \"Something has happened to string\")))))");
}

} // namespace
} // namespace cythonpp::domain::parser
