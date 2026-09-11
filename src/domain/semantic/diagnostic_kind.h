#ifndef CYTHONPP_DOMAIN_SEMANTIC_DIAGNOSTIC_KIND_H
#define CYTHONPP_DOMAIN_SEMANTIC_DIAGNOSTIC_KIND_H

#include "domain/diagnostics/suppressibility.h"

namespace cythonpp::domain::semantic {

// The closed set of diagnostics this pass can produce. Every report site in
// domain/semantic/ names one of these instead of spelling a code string, and
// the (code, suppressibility) pair is derived from it below.
//
// WHY THIS EXISTS AT ALL, since the project's diagnostic vocabulary is only
// four codes and a string would carry all four: the code string is a LOSSY
// proxy for the one question an unreachable region has to ask. Measured
// against mypy 1.18.1 -- mypy's SEMANTIC ANALYZER runs in unreachable code
// and its TYPE CHECKER does not, and cythonpp spells "TypeError" for
// judgements on BOTH sides of that line. So "suppress the code TypeError"
// silences, among others, `name "y" already defined on line 3` (mypy
// [no-redef], reported in unreachable code) and `duplicate argument "x" in
// function definition` (mypy exit 2 AND a CPython compile-time SyntaxError,
// so the file never runs at any reachability). Nine measured classes of
// program that an oracle rejects were being silently accepted.
//
// Splitting TypeError into two ENUMERATORS -- rather than adding a second
// parameter beside the code string -- is what makes the classification
// unforgettable AND unfalsifiable: there is no way to spell "TypeError" at a
// report site without choosing a side, and no way to pair one code with the
// other's answer. Note that NEITHER enumerator is called plain `TypeError`:
// if one were, it would read as the obvious choice and a new site would take
// it by default, which is the exact failure this table exists to prevent.
// Both are named for the mypy phase that owns the judgement, so the name IS
// the question.
//
// WHAT THIS DOES AND DOES NOT GUARANTEE, stated at the strength the code
// actually supports, because it has been overstated at this mechanism three
// rounds running. Measured 2026-09-11, by inserting the line and building:
//
//     sink_.report_error("TypeError", "ESCAPE HATCH probe", 1, 1,
//                        diagnostics::Suppressibility::Suppressible);
//
// inside type_checker.cpp COMPILES CLEAN and the diagnostic appears at run
// time. All three semantic classes hold `DiagnosticSink& sink_` directly --
// that is how their seams reach it -- so a new site CAN bypass this enum
// entirely. What is impossible is OMITTING the answer: DiagnosticSink::report
// and ::report_error have no default `Suppressibility`, so a bypassing site
// still has to state one. The true guarantee is therefore:
//
//   * a code STRING cannot be written at any of the three SEAMS (they take a
//     DiagnosticKind), and
//   * a new ENUMERATOR cannot compile until both tables below classify it,
//     and
//   * no producer anywhere can leave suppressibility unstated,
//
// but NOT "a code string cannot be written at a semantic report site at all".
// The auditing consequence is the part that bites: `grep -n
// "DiagnosticKind::" src/domain/semantic/*.cpp` lists every classified site
// and would not see such a bypass, so a complete audit needs a SECOND grep,
//
//     grep -n "sink_" src/domain/semantic/*.cpp
//
// which must show only the three seams (`annotation_resolver.cpp`,
// `expression_typer.cpp`, `type_checker.cpp`, one `report_error` each), plus
// `check_suite`'s `unreachable.emplace(sink_)` and the AnnotationResolver
// constructions that pass the sink along. Anything else is a producer that
// the first grep cannot see.
enum class DiagnosticKind {
    // "TypeError" for a judgement mypy's TYPE CHECKER owns: operand types,
    // argument types and arity, assignment compatibility, return types,
    // missing/unannotated signatures, `need type annotation`, list and dict
    // item types, `method must have at least one argument`, `not callable`,
    // missing return. Measured: mypy reports none of these in unreachable
    // code (probe E02 against its reachable control E04, plus the per-class
    // table in the round-4 report). SUPPRESSIBLE.
    TypeCheckerTypeError,

    // "TypeError" for a judgement mypy's SEMANTIC ANALYZER owns: a
    // redefinition ([no-redef]), a duplicate parameter name (a blocking
    // mypy error and a CPython SyntaxError), and every verdict on whether an
    // annotation is a well-formed type at all ([valid-type], [type-arg]).
    // Measured: mypy reports all of these in unreachable code. NEVER
    // SUPPRESSIBLE.
    //
    // The test for a new site: would mypy still say this if the statement sat
    // after a `return`? If it is a claim about the SHAPE of a definition or
    // an annotation, yes; if it is a claim about the TYPES flowing through an
    // expression, no.
    //
    // AND THE SECOND QUESTION, which round 5 exists because nobody asked:
    // does mypy say this AT ALL, on every sub-form the site fires on? The
    // first question is the right one for "may this be suppressed?" and the
    // WRONG one for "should this fire?". A class can be genuine
    // semantic-analyzer output and still over-fire: `'X' is not
    // subscriptable` was correct for `int[str]` and a false positive for
    // `type[int]`, and `name "g" already defined` was correct for two flat
    // defs and a false positive for a conditional one. Classifying such a
    // site NotSuppressible -- correctly -- widens the reach of a judgement
    // that was already wrong. Both questions, or neither answer is worth
    // anything.
    SemanticAnalyzerTypeError,

    // "NameError". Also semantic-analyzer output -- mypy reports
    // [name-defined] and [used-before-def] in unreachable code (probes E01,
    // E06, E07). NEVER SUPPRESSIBLE.
    NameError,

    // "NotImplementedError". This compiler's own capability claim, with no
    // mypy analogue by construction: it says nothing about the program's
    // types, so it cannot contradict either oracle, and the project's rule
    // states outright that it is not silent acceptance. It must survive
    // unreachability because there is no dead-code elimination here -- an
    // unreachable subtree still has to be handed to codegen, and a construct
    // this compiler cannot model does not become modellable by being
    // unreachable. NEVER SUPPRESSIBLE.
    NotImplementedError,

    // "OverflowError". A capability claim too, for the same reason: a
    // literal that does not fit 64 bits does not start fitting because the
    // statement holding it never runs. NEVER SUPPRESSIBLE.
    OverflowError,
};

// The Python exception name this kind is reported under. Two kinds share
// "TypeError", deliberately -- the four-code vocabulary is fixed by the
// project's rule, and re-spelling a semantic-analyzer judgement as a fifth
// code would change labelled corpus output to encode something only this
// compiler's internals care about.
// BOTH FUNCTIONS BELOW ARE DELIBERATELY EXHAUSTIVE SWITCHES WITH NO `default`
// LABEL, and CMakeLists.txt passes -Werror=switch, so adding an enumerator to
// DiagnosticKind without answering BOTH questions for it is a COMPILE ERROR
// at the exact two spots needing attention. That is the point: a `default`
// label, or a `==` comparison against one enumerator, would quietly give a new
// kind someone else's answer -- and "quietly gave it the wrong answer" is this
// defect's documented failure mode four rounds running. Do not add a
// `default:` here to silence anything.
inline const char* diagnostic_code(DiagnosticKind kind) {
    switch (kind) {
    case DiagnosticKind::TypeCheckerTypeError:
    case DiagnosticKind::SemanticAnalyzerTypeError:
        return "TypeError";
    case DiagnosticKind::NameError:
        return "NameError";
    case DiagnosticKind::NotImplementedError:
        return "NotImplementedError";
    case DiagnosticKind::OverflowError:
        return "OverflowError";
    }
    // Unreachable for any valid enumerator; present only because a function
    // returning a value must, to the compiler, appear to always do so.
    return "TypeError";
}

// Whether TypeChecker::check_suite's unreachable-region suppression applies.
// Exactly one kind is suppressible; see each enumerator's own comment for the
// measurement behind it.
inline diagnostics::Suppressibility suppressibility_of(DiagnosticKind kind) {
    switch (kind) {
    case DiagnosticKind::TypeCheckerTypeError:
        return diagnostics::Suppressibility::Suppressible;
    case DiagnosticKind::SemanticAnalyzerTypeError:
    case DiagnosticKind::NameError:
    case DiagnosticKind::NotImplementedError:
    case DiagnosticKind::OverflowError:
        return diagnostics::Suppressibility::NotSuppressible;
    }
    // Unreachable for any valid enumerator. NotSuppressible rather than
    // Suppressible: if this line is ever somehow reached, reporting is the
    // recoverable mistake and silence is not.
    return diagnostics::Suppressibility::NotSuppressible;
}

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_DIAGNOSTIC_KIND_H
