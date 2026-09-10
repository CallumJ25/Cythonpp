#ifndef CYTHONPP_DOMAIN_DIAGNOSTICS_DIAGNOSTIC_SINK_H
#define CYTHONPP_DOMAIN_DIAGNOSTICS_DIAGNOSTIC_SINK_H

#include <string>
#include <vector>

#include "diagnostic.h"
#include "suppressibility.h"

namespace cythonpp::domain::diagnostics {

// Collects the diagnostics a pass produces, in report order.
//
// A concrete class rather than an interface: domain code must not depend on
// ports/, and a pass has no business knowing whether its output is printed,
// buffered, or discarded. The application layer drains a sink into
// ports::DiagnosticsReporter, which is where that choice belongs.
class DiagnosticSink {
public:
    // EVERY call must state a Suppressibility, and there is deliberately NO
    // default argument for it. Consider the two possible defaults: defaulting
    // to Suppressible sends a forgotten new diagnostic silent inside a
    // suppressed region, and defaulting to NotSuppressible makes a forgotten
    // new one report where the region says it must not. Which of those is the
    // damaging one depends on the diagnostic, so neither is safe as a default
    // and omission must be a compile error instead -- the same reason
    // ast::Visitor gives every node a pure-virtual visit and RuleResult
    // defaults to NotApplicable.
    void report(Diagnostic diagnostic, Suppressibility suppressibility);

    // The common case, spelled so call sites stay off aggregate initialization
    // -- adding a field to Diagnostic should not break every reporting pass.
    void report_error(std::string code, std::string message, int line, int column,
                      Suppressibility suppressibility);

    const std::vector<Diagnostic>& diagnostics() const;

    // True if any diagnostic has Severity::Error. Distinct from !empty(),
    // which a warning also satisfies.
    bool has_errors() const;
    bool empty() const;

    // SCOPED SUPPRESSION. While a suppression is pushed, every diagnostic
    // reported as Suppressibility::Suppressible is DROPPED at report() and
    // never reaches diagnostics_ -- so has_errors()/empty() cannot see it
    // either. A NotSuppressible diagnostic is unaffected.
    //
    // Why the filter lives on the sink rather than on the pass that wants it:
    // the semantic pass reports from FOUR classes across 62 call sites
    // (counted 2026-09-10) -- 26 in TypeChecker, which do route through its
    // own report() helper, plus 15 in AnnotationResolver, 12 in
    // ExpressionTyper and 9 in expression_typer_calls.cpp, each of which
    // calls its OWN class's error() helper and reaches this sink with no
    // TypeChecker method in between. So a flag consulted in TypeChecker::
    // report covers 26 of the 62 and silently misses 36, and "forgot a call
    // site" is this defect's documented recurring failure mode. Filtering
    // at the single point every diagnostic must pass through is complete by
    // construction: no producer, present or future, can route around it.
    //
    // A DEPTH COUNTER, NOT A BOOL, because the regions that use it NEST --
    // an unreachable suite may contain another one, and the inner region's
    // end must not un-suppress the outer.
    //
    // WHAT IS SUPPRESSED IS A PROPERTY OF THE DIAGNOSTIC, NOT ITS CODE.
    // Round 3 of this fix keyed the filter on the code string "TypeError"
    // instead, and that string is a LOSSY proxy: cythonpp spells "TypeError"
    // both for judgements mypy's type checker owns (which mypy does not make
    // in unreachable code) and for judgements mypy's semantic analyzer owns
    // (which it does -- [no-redef], [valid-type], [type-arg], Duplicate
    // argument). Suppressing the string silently accepted nine measured
    // classes of program that mypy rejects and one that CPython refuses to
    // compile. The axis is now carried explicitly from the report site, so
    // this class still learns nothing about which codes exist or what any of
    // them mean -- only "drop the ones marked Suppressible".
    //
    // Call these through DiagnosticSuppression below, never by hand.
    void push_suppression();
    void pop_suppression();

private:
    std::vector<Diagnostic> diagnostics_;
    int suppression_depth_ = 0;
};

// RAII for DiagnosticSink's push/pop pair.
//
// RAII rather than paired calls for the same reason NarrowingScopeGuard is:
// a missed pop suppresses for the whole REST of the file, which
// shows up as silence rather than as a crash -- the failure mode a test suite
// is least likely to notice. Non-copyable and non-movable so the pair cannot
// be duplicated or orphaned; construct it in place (std::optional::emplace)
// when the region's start is decided at run time.
class DiagnosticSuppression {
public:
    explicit DiagnosticSuppression(DiagnosticSink& sink) : sink_(sink) {
        sink_.push_suppression();
    }
    ~DiagnosticSuppression() { sink_.pop_suppression(); }
    DiagnosticSuppression(const DiagnosticSuppression&) = delete;
    DiagnosticSuppression& operator=(const DiagnosticSuppression&) = delete;

private:
    DiagnosticSink& sink_;
};

} // namespace cythonpp::domain::diagnostics

#endif // CYTHONPP_DOMAIN_DIAGNOSTICS_DIAGNOSTIC_SINK_H
