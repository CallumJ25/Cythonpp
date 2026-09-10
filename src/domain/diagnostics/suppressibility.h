#ifndef CYTHONPP_DOMAIN_DIAGNOSTICS_SUPPRESSIBILITY_H
#define CYTHONPP_DOMAIN_DIAGNOSTICS_SUPPRESSIBILITY_H

namespace cythonpp::domain::diagnostics {

// Whether an active DiagnosticSuppression on the sink applies to one
// diagnostic. Stated at every report site; there is no default (see
// DiagnosticSink::report).
//
// This is the sink's WHOLE knowledge of the subject. It does not know what a
// suppression means, which pass pushes one, or which diagnostics ought to
// carry which value -- only "while a suppression is live, drop the ones
// marked Suppressible". The judgement itself belongs to the pass that owns
// the diagnostic; for domain/semantic/ it lives in that directory's own
// DiagnosticKind table.
//
// A named two-valued enum rather than a bool because a bool at a call site
// reads as `true`/`false` with nothing to say which way round it goes, and
// this is exactly the parameter a reader must not have to guess at: getting
// it backwards either silences an error the oracles report or invents one
// they do not.
enum class Suppressibility {
    // Reported no matter what. The right answer for anything a suppression's
    // reason does not cover -- and the value every pass outside
    // domain/semantic/ uses, since nothing outside it pushes a suppression.
    NotSuppressible,

    // Dropped while a suppression is live.
    Suppressible,
};

} // namespace cythonpp::domain::diagnostics

#endif // CYTHONPP_DOMAIN_DIAGNOSTICS_SUPPRESSIBILITY_H
