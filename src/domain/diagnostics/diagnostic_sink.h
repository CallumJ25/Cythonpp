#ifndef CYTHONPP_DOMAIN_DIAGNOSTICS_DIAGNOSTIC_SINK_H
#define CYTHONPP_DOMAIN_DIAGNOSTICS_DIAGNOSTIC_SINK_H

#include <string>
#include <vector>

#include "diagnostic.h"

namespace cythonpp::domain::diagnostics {

// Collects the diagnostics a pass produces, in report order.
//
// A concrete class rather than an interface: domain code must not depend on
// ports/, and a pass has no business knowing whether its output is printed,
// buffered, or discarded. The application layer drains a sink into
// ports::DiagnosticsReporter, which is where that choice belongs.
class DiagnosticSink {
public:
    void report(Diagnostic diagnostic);

    // The common case, spelled so call sites stay off aggregate initialization
    // -- adding a field to Diagnostic should not break every reporting pass.
    void report_error(std::string code, std::string message, int line, int column);

    const std::vector<Diagnostic>& diagnostics() const;

    // True if any diagnostic has Severity::Error. Distinct from !empty(),
    // which a warning also satisfies.
    bool has_errors() const;
    bool empty() const;

    // SCOPED SUPPRESSION OF ONE CODE. While `code` is pushed, every
    // diagnostic carrying that exact code is DROPPED at report() and never
    // reaches diagnostics_ -- so has_errors()/empty() cannot see it either.
    //
    // Why the filter lives on the sink rather than on the pass that wants it:
    // the semantic pass reports "TypeError" from FOUR classes across 31 call
    // sites (counted 2026-09-10): 14 in TypeChecker, which do route through
    // its own report() helper, plus 9 in AnnotationResolver, 4 in
    // ExpressionTyper and 4 in expression_typer_calls.cpp, each of which
    // calls its OWN class's error() helper and reaches this sink with no
    // TypeChecker method in between. So a flag consulted in TypeChecker::
    // report covers 14 of the 31 and silently misses 17, and "forgot a call
    // site" is this defect's documented recurring failure mode. Filtering
    // at the single point every diagnostic must pass through is complete by
    // construction: no producer, present or future, can route around it.
    //
    // A STACK, NOT A BOOL, because the regions that use it NEST -- an
    // unreachable suite may contain another one, and the inner region's end
    // must not un-suppress the outer.
    //
    // THE CODE IS A PARAMETER, deliberately, so this class learns nothing
    // about which codes exist or what any of them mean. It knows only "drop
    // diagnostics whose code string equals one currently pushed"; the decision
    // that "TypeError" is the code an unreachable region suppresses stays in
    // domain/semantic/, which owns that judgement.
    //
    // Call these through DiagnosticSuppression below, never by hand.
    void push_suppressed_code(std::string code);
    void pop_suppressed_code();

private:
    bool is_suppressed(const std::string& code) const;

    std::vector<Diagnostic> diagnostics_;
    std::vector<std::string> suppressed_codes_;
};

// RAII for DiagnosticSink's push/pop pair.
//
// RAII rather than paired calls for the same reason NarrowingScopeGuard is:
// a missed pop suppresses the code for the whole REST of the file, which
// shows up as silence rather than as a crash -- the failure mode a test suite
// is least likely to notice. Non-copyable and non-movable so the pair cannot
// be duplicated or orphaned; construct it in place (std::optional::emplace)
// when the region's start is decided at run time.
class DiagnosticSuppression {
public:
    DiagnosticSuppression(DiagnosticSink& sink, std::string code) : sink_(sink) {
        sink_.push_suppressed_code(std::move(code));
    }
    ~DiagnosticSuppression() { sink_.pop_suppressed_code(); }
    DiagnosticSuppression(const DiagnosticSuppression&) = delete;
    DiagnosticSuppression& operator=(const DiagnosticSuppression&) = delete;

private:
    DiagnosticSink& sink_;
};

} // namespace cythonpp::domain::diagnostics

#endif // CYTHONPP_DOMAIN_DIAGNOSTICS_DIAGNOSTIC_SINK_H
