#include <gtest/gtest.h>

#include <string>

#include "statement_parse_test_helpers.h"

namespace cythonpp::domain::parser {
namespace {

using statement_test_support::only_error;
using statement_test_support::parse_module;

void expect_error(const std::string& source, const std::string& message, int line, int column) {
    const diagnostics::Diagnostic diagnostic =
        only_error(statement_test_support::parse_module(source));
    EXPECT_EQ(diagnostic.code, "SyntaxError") << source;
    EXPECT_EQ(diagnostic.message, message) << source;
    EXPECT_EQ(diagnostic.line, line) << source;
    EXPECT_EQ(diagnostic.column, column) << source;
    EXPECT_EQ(diagnostic.severity, diagnostics::Severity::Error) << source;
}

TEST(StatementParserError, JunkAfterASimpleStatementIsReportedOnce) {
    expect_error("pass pass\n", "expected a newline after the statement", 1, 6);
}

TEST(StatementParserError, ABadStatementDoesNotStopTheOnesAfterIt) {
    const statement_test_support::ModuleResult result = parse_module("pass pass\nbreak\n");

    EXPECT_EQ(result.diagnostics.size(), 1u);
    ASSERT_NE(result.module, nullptr);
    // The bad statement is dropped; the good one after it survives. This is
    // what panic-mode recovery is for, and asserting the tree rather than
    // only the diagnostic count is what actually pins it.
    EXPECT_EQ(result.printed(), "(Module\n  (Break))");
}

TEST(StatementParserError, ParseModuleIsNeverNullEvenWhenEverythingFailed) {
    const statement_test_support::ModuleResult result = parse_module("pass pass\n");

    ASSERT_NE(result.module, nullptr);
    EXPECT_EQ(result.printed(), "(Module)");
    EXPECT_TRUE(result.module->body().empty());
}

TEST(StatementParserError, ABadReturnValueReportsExactlyOneDiagnostic) {
    // ExpressionParser already reported "lambda expressions are not
    // supported". The statement parser must propagate the failure silently
    // rather than adding a second message.
    expect_error("return lambda: 1\n", "lambda expressions are not supported", 1, 8);
}

TEST(StatementParserError, AssigningToALiteralIsRejected) {
    expect_error("1 = x\n", "cannot assign to literal", 1, 1);
}

TEST(StatementParserError, AssigningToACallIsRejected) {
    expect_error("f() = x\n", "cannot assign to function call", 1, 1);
}

TEST(StatementParserError, AnnotatingATupleIsRejected) {
    // Python does not allow annotating a tuple, and AnnAssign holds one
    // target, so this is rejected in the parser rather than deferred.
    expect_error("x, y: int = 1\n", "only single targets can be annotated", 1, 1);
}

TEST(StatementParserError, ChainedAssignmentGetsTheGenericMessage) {
    // Deliberately not a named diagnostic: import and augmented assignment
    // appear in essentially every real file and earn specific messages, while
    // chained assignment is rare enough that the generic one is adequate.
    expect_error("a = b = 1\n", "expected a newline after the statement", 1, 7);
}

TEST(StatementParserError, AFailedExpressionStatementReportsOnlyItsOwnDiagnostic) {
    expect_error("f(*a)\n", "starred expressions are not supported", 1, 3);
}

TEST(StatementParserError, AnIndentWithNoBlockHeaderIsReported) {
    // IndentationPass emits a balanced INDENT/DEDENT pair here and reports
    // nothing, so this diagnostic is the parser's to produce.
    const diagnostics::Diagnostic diagnostic =
        only_error(statement_test_support::parse_module("a = 1\n    b = 2\nc = 3\n"));
    EXPECT_EQ(diagnostic.code, "IndentationError");
    EXPECT_EQ(diagnostic.message, "unexpected indent");
    EXPECT_EQ(diagnostic.line, 2);
    EXPECT_EQ(diagnostic.column, 5);
}

TEST(StatementParserError, AnUnexpectedIndentDoesNotEatTheRestOfTheFile) {
    const statement_test_support::ModuleResult result =
        parse_module("a = 1\n    b = 2\nc = 3\n");

    ASSERT_NE(result.module, nullptr);
    // The indented block is discarded; the statements around it survive.
    EXPECT_EQ(result.printed(),
              "(Module\n  (Assign (Name a) (Constant 1))\n  (Assign (Name c) (Constant 3)))");
}

TEST(StatementParserError, ANestedUnexpectedIndentIsStillOneDiagnostic) {
    // skip_unexpected_block tracks nesting depth, so a block containing its
    // own deeper block is skipped whole rather than re-reported per level.
    const statement_test_support::ModuleResult result =
        parse_module("a = 1\n    b = 2\n        c = 3\nd = 4\n");

    EXPECT_EQ(result.diagnostics.size(), 1u);
    ASSERT_NE(result.module, nullptr);
    EXPECT_EQ(result.printed(),
              "(Module\n  (Assign (Name a) (Constant 1))\n  (Assign (Name d) (Constant 4)))");
}

TEST(StatementParserError, AMissingColonAfterAnIfConditionIsReported) {
    expect_error("if x\n    pass\n", "expected ':'", 1, 5);
}

TEST(StatementParserError, AnIfHeaderWithNoIndentedBodyIsReported) {
    expect_error("if x:\npass\n", "expected an indented block", 2, 1);
}

TEST(StatementParserError, ABadStatementInsideASuiteDoesNotDiscardTheSuite) {
    // The reason recovery stops at -- and never consumes -- INDENT/DEDENT.
    const statement_test_support::ModuleResult result =
        parse_module("if a:\n    pass pass\n    break\n");

    EXPECT_EQ(result.diagnostics.size(), 1u);
    ASSERT_NE(result.module, nullptr);
    EXPECT_EQ(result.printed(), "(Module\n  (If (Name a)\n    (Break)))");
}

TEST(StatementParserError, ABadSuiteDoesNotSwallowTheStatementsAfterIt) {
    const statement_test_support::ModuleResult result =
        parse_module("if a:\n    pass pass\nb = 1\n");

    EXPECT_EQ(result.diagnostics.size(), 1u);
    ASSERT_NE(result.module, nullptr);
    EXPECT_EQ(result.printed(), "(Module\n  (Assign (Name b) (Constant 1)))");
}

TEST(StatementParserError, AFailedElseHeaderIsStillOneDiagnostic) {
    // parse_else_clause must distinguish "no else" from "broken else". If it
    // cannot, parse_if builds a valid If out of a failed parse, its recovery
    // path never runs, and the orphaned block is reported a second time.
    expect_error("if a:\n    pass\nelse\n    pass\n", "expected ':'", 3, 5);
}

TEST(StatementParserError, ACommentBeforeARecoveredStatementDoesNotHideTheBoundary) {
    // A comment-only line emits no NEWLINE and survives IndentationPass, so
    // the token physically before the cursor is not the one that logically
    // precedes it. Without the backward comment skip, at_statement_boundary()
    // answers false here, synchronize() eats the `pass` line, and the tree
    // silently loses it.
    const statement_test_support::ModuleResult result =
        parse_module("if x:\n# note\npass\n");

    EXPECT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().message, "expected an indented block");
    ASSERT_NE(result.module, nullptr);
    EXPECT_EQ(result.printed(), "(Module\n  (Pass))");
}

TEST(StatementParserError, AFailedIfConditionSwallowsItsOrphanedBlock) {
    expect_error("if lambda: 1\n    pass\n", "lambda expressions are not supported", 1, 4);
}

TEST(StatementParserError, AForWithNoInKeywordIsReported) {
    expect_error("for x:\n    pass\n", "expected 'in' after the for target", 1, 6);
}

TEST(StatementParserError, AForTargetThatCannotBeAssignedIsReported) {
    expect_error("for f() in items:\n    pass\n", "cannot assign to function call", 1, 5);
}

TEST(StatementParserError, ADefWithNoNameIsReported) {
    expect_error("def ():\n    pass\n", "expected a function name", 1, 5);
}

TEST(StatementParserError, ADefWithNoParenthesesIsReported) {
    expect_error("def f:\n    pass\n", "expected '(' after the function name", 1, 6);
}

TEST(StatementParserError, AnUnclosedParameterListIsReportedAgainstItsOpener) {
    // The lexer emits a NEWLINE at EOF even inside an unclosed bracket
    // (lexer.cpp:544), so there is no NEWLINE-free run to detect -- the close
    // must be checked explicitly rather than inferred from a missing newline.
    expect_error("def f(a\n", "expected ')' to close the parameter list", 1, 6);
}

TEST(StatementParserError, AStarredParameterIsReported) {
    expect_error("def f(*args):\n    pass\n", "expected a parameter name", 1, 7);
}

TEST(StatementParserError, AClassWithNoNameIsReported) {
    expect_error("class :\n    pass\n", "expected a class name", 1, 7);
}

TEST(StatementParserError, AnUnclosedBaseListIsReportedAgainstItsOpener) {
    expect_error("class C(A\n", "expected ')' to close the base list", 1, 8);
}

TEST(StatementParserError, AKeywordArgumentInABaseListIsReported) {
    // `metaclass` parses fine as a bare Name; parse_class_def then sees the
    // '=' immediately after and reports the same message ExpressionParser
    // uses for keyword arguments in a call, positioned at `metaclass` (the
    // base just parsed) rather than the unclosed-list fallback.
    expect_error("class C(metaclass=M):\n    pass\n",
                 "keyword arguments are not supported", 1, 9);
}

TEST(StatementParserError, UnsupportedStatementsNameThemselves) {
    // `with` is deliberately absent here: it has an indented body, so it
    // produces two diagnostics like try: and async def, and only_error would
    // fail. It gets its own test below, alongside except/finally/else/elif,
    // which have the same shape.
    expect_error("import os\n", "import statements are not supported", 1, 1);
    expect_error("from os import path\n", "import statements are not supported", 1, 1);
    expect_error("raise ValueError()\n", "raise statements are not supported", 1, 1);
    expect_error("assert x\n", "assert statements are not supported", 1, 1);
    expect_error("del x\n", "del statements are not supported", 1, 1);
    expect_error("global x\n", "global statements are not supported", 1, 1);
    expect_error("nonlocal x\n", "nonlocal statements are not supported", 1, 1);
}

TEST(StatementParserError, WithStatementDoesNotCascadeIntoItsBody) {
    // Deviation from the brief: `with` has an indented body, so -- like try:
    // and async def -- it produces two diagnostics (the rejection, then the
    // orphaned block) and cannot use expect_error's only_error. Moved out of
    // UnsupportedStatementsNameThemselves rather than loosening only_error.
    const statement_test_support::ModuleResult result =
        parse_module("with open(p) as f:\n    pass\n");

    EXPECT_TRUE(result.indentation_diagnostics.empty());
    ASSERT_EQ(result.diagnostics.size(), 2u);
    EXPECT_EQ(result.diagnostics.at(0).message, "with statements are not supported");
    EXPECT_EQ(result.diagnostics.at(1).message, "unexpected indent");
    ASSERT_NE(result.module, nullptr);
    EXPECT_EQ(result.printed(), "(Module)");
}

TEST(StatementParserError, OrphanedExceptAndFinallyNameTryRatherThanThemselves) {
    // try is unsupported, so a bare except: can only ever be orphaned. The
    // message naming the construct is more useful than "unexpected 'except'".
    //
    // Deviation from the brief: both except: and finally: have an indented
    // body, so -- like try: itself -- each produces two diagnostics and
    // cannot use expect_error's only_error.
    {
        const statement_test_support::ModuleResult result = parse_module("except:\n    pass\n");
        EXPECT_TRUE(result.indentation_diagnostics.empty());
        ASSERT_EQ(result.diagnostics.size(), 2u);
        EXPECT_EQ(result.diagnostics.at(0).message, "try statements are not supported");
        EXPECT_EQ(result.diagnostics.at(1).message, "unexpected indent");
        ASSERT_NE(result.module, nullptr);
        EXPECT_EQ(result.printed(), "(Module)");
    }
    {
        const statement_test_support::ModuleResult result = parse_module("finally:\n    pass\n");
        EXPECT_TRUE(result.indentation_diagnostics.empty());
        ASSERT_EQ(result.diagnostics.size(), 2u);
        EXPECT_EQ(result.diagnostics.at(0).message, "try statements are not supported");
        EXPECT_EQ(result.diagnostics.at(1).message, "unexpected indent");
        ASSERT_NE(result.module, nullptr);
        EXPECT_EQ(result.printed(), "(Module)");
    }
}

TEST(StatementParserError, DecoratorsAreRejectedAtStatementPosition) {
    // Settles OP_AT's second reading: decorator at statement position,
    // matrix-multiply everywhere else.
    expect_error("@deco\ndef f():\n    pass\n", "decorators are not supported", 1, 1);
}

TEST(StatementParserError, MatrixMultiplyStillParsesInsideAnExpression) {
    // The other half of the OP_AT split. This must NOT be read as a decorator.
    const statement_test_support::ModuleResult result = parse_module("c = a @ b\n");
    EXPECT_TRUE(result.diagnostics.empty());
    ASSERT_NE(result.module, nullptr);
    EXPECT_EQ(result.printed(),
              "(Module\n  (Assign (Name c) (BinOp @ (Name a) (Name b))))");
}

TEST(StatementParserError, AugmentedAssignmentIsRejected) {
    expect_error("x += 1\n", "augmented assignment is not supported", 1, 3);
    expect_error("x //= 2\n", "augmented assignment is not supported", 1, 3);
    expect_error("x >>= 2\n", "augmented assignment is not supported", 1, 3);
}

TEST(StatementParserError, AnOrphanedElseOrElifIsReported) {
    // Deviation from the brief: both else: and elif x: have an indented
    // body, so -- like try: and with -- each produces two diagnostics (the
    // misplaced-keyword rejection, then the orphaned block) and cannot use
    // expect_error's only_error.
    {
        const statement_test_support::ModuleResult result = parse_module("else:\n    pass\n");
        EXPECT_TRUE(result.indentation_diagnostics.empty());
        ASSERT_EQ(result.diagnostics.size(), 2u);
        EXPECT_EQ(result.diagnostics.at(0).message, "unexpected 'else'");
        EXPECT_EQ(result.diagnostics.at(1).message, "unexpected indent");
        ASSERT_NE(result.module, nullptr);
        EXPECT_EQ(result.printed(), "(Module)");
    }
    {
        const statement_test_support::ModuleResult result = parse_module("elif x:\n    pass\n");
        EXPECT_TRUE(result.indentation_diagnostics.empty());
        ASSERT_EQ(result.diagnostics.size(), 2u);
        EXPECT_EQ(result.diagnostics.at(0).message, "unexpected 'elif'");
        EXPECT_EQ(result.diagnostics.at(1).message, "unexpected indent");
        ASSERT_NE(result.module, nullptr);
        EXPECT_EQ(result.printed(), "(Module)");
    }
}

TEST(StatementParserError, AFailedIfBodyTakesItsElseWithItRatherThanCascading) {
    // When the suite fails, parse_if returns null before parse_else_clause
    // ever runs. Without the fix, the orphaned `else` then gets dispatched
    // through parse_simple_statement as a misplaced keyword, and its own body
    // becomes a second, unrelated "unexpected indent" -- three diagnostics
    // instead of one, and two of them pointing at the wrong place.
    const statement_test_support::ModuleResult result =
        parse_module("if a:\n    pass pass\nelse:\n    pass\nz = 1\n");

    EXPECT_TRUE(result.indentation_diagnostics.empty());
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().message,
              "expected a newline after the statement");
    EXPECT_EQ(result.diagnostics.front().line, 2);
    EXPECT_EQ(result.diagnostics.front().column, 10);
    ASSERT_NE(result.module, nullptr);
    EXPECT_EQ(result.printed(), "(Module\n  (Assign (Name z) (Constant 1)))");
}

TEST(StatementParserError, AFailedIfBodyTakesItsElifWithItRatherThanCascading) {
    const statement_test_support::ModuleResult result =
        parse_module("if a:\n    pass pass\nelif b:\n    pass\nz = 1\n");

    EXPECT_TRUE(result.indentation_diagnostics.empty());
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().message,
              "expected a newline after the statement");
    EXPECT_EQ(result.diagnostics.front().line, 2);
    EXPECT_EQ(result.diagnostics.front().column, 10);
    ASSERT_NE(result.module, nullptr);
    EXPECT_EQ(result.printed(), "(Module\n  (Assign (Name z) (Constant 1)))");
}

TEST(StatementParserError, AFailedWhileBodyTakesItsElseWithItRatherThanCascading) {
    const statement_test_support::ModuleResult result =
        parse_module("while a:\n    pass pass\nelse:\n    pass\nz = 1\n");

    EXPECT_TRUE(result.indentation_diagnostics.empty());
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().message,
              "expected a newline after the statement");
    EXPECT_EQ(result.diagnostics.front().line, 2);
    EXPECT_EQ(result.diagnostics.front().column, 10);
    ASSERT_NE(result.module, nullptr);
    EXPECT_EQ(result.printed(), "(Module\n  (Assign (Name z) (Constant 1)))");
}

TEST(StatementParserError, AFailedForBodyTakesItsElseWithItRatherThanCascading) {
    const statement_test_support::ModuleResult result =
        parse_module("for a in b:\n    pass pass\nelse:\n    pass\nz = 1\n");

    EXPECT_TRUE(result.indentation_diagnostics.empty());
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().message,
              "expected a newline after the statement");
    EXPECT_EQ(result.diagnostics.front().line, 2);
    EXPECT_EQ(result.diagnostics.front().column, 10);
    ASSERT_NE(result.module, nullptr);
    EXPECT_EQ(result.printed(), "(Module\n  (Assign (Name z) (Constant 1)))");
}

TEST(StatementParserError, AnUnsupportedStatementDoesNotCascade) {
    // reject() itself only reports; parse_statement_list's synchronize() call
    // on the false return is what consumes the rest of the logical line, so
    // the tokens the construct would have owned cannot each produce their own
    // diagnostic -- and, just as importantly, cannot bleed into the next one.
    const statement_test_support::ModuleResult result = parse_module("import os\nx = 1\n");

    EXPECT_EQ(result.diagnostics.size(), 1u);
    ASSERT_NE(result.module, nullptr);
    EXPECT_EQ(result.printed(), "(Module\n  (Assign (Name x) (Constant 1)))");
}

TEST(StatementParserError, AnUnsupportedBlockStatementDoesNotCascadeIntoItsBody) {
    // `try:` is rejected, and its indented body then hits the stray-INDENT
    // path -- a second, different diagnostic. Two is correct here; what must
    // not happen is one per statement in the body.
    const statement_test_support::ModuleResult result =
        parse_module("try:\n    a = 1\n    b = 2\nc = 3\n");

    EXPECT_TRUE(result.indentation_diagnostics.empty());
    ASSERT_EQ(result.diagnostics.size(), 2u);
    EXPECT_EQ(result.diagnostics.at(0).message, "try statements are not supported");
    EXPECT_EQ(result.diagnostics.at(1).message, "unexpected indent");
    ASSERT_NE(result.module, nullptr);
    EXPECT_EQ(result.printed(), "(Module\n  (Assign (Name c) (Constant 3)))");
}

TEST(StatementParserError, SoftKeywordsStayOrdinaryNames) {
    // Spec 3 declined to build the soft_keyword_of seam because there is no
    // node to re-classify into, and Spec 4 does not change that. `match` and
    // `case` arrive as IDENTIFIER, so `match x:` is two adjacent atoms and
    // fails generically. Recognising it properly needs a bracket-depth scan
    // for a top-level colon, whose only consumer would be an error message.
    const statement_test_support::ModuleResult statement =
        parse_module("match x:\n    case 1:\n        pass\n");
    ASSERT_EQ(statement.diagnostics.size(), 2u);
    EXPECT_EQ(statement.diagnostics.at(0).message,
              "expected a newline after the statement");
    EXPECT_EQ(statement.diagnostics.at(1).message, "unexpected indent");

    // The other half: as ordinary names they must still parse as names.
    EXPECT_EQ(statement_test_support::parse_module("match = 1\n").diagnostics.size(), 0u);
    EXPECT_EQ(statement_test_support::parse_module("case = 1\n").diagnostics.size(), 0u);
    EXPECT_EQ(statement_test_support::parse_module("_ = 1\n").diagnostics.size(), 0u);
}

TEST(StatementParserError, AsyncIsRejected) {
    // Its body is an indented block, so this produces the same two
    // diagnostics as try: the rejection, then the orphaned block.
    const statement_test_support::ModuleResult result =
        parse_module("async def f():\n    pass\n");

    EXPECT_TRUE(result.indentation_diagnostics.empty());
    ASSERT_EQ(result.diagnostics.size(), 2u);
    EXPECT_EQ(result.diagnostics.at(0).message, "async statements are not supported");
    EXPECT_EQ(result.diagnostics.at(0).line, 1);
    EXPECT_EQ(result.diagnostics.at(0).column, 1);
    EXPECT_EQ(result.diagnostics.at(1).message, "unexpected indent");
}

} // namespace
} // namespace cythonpp::domain::parser
