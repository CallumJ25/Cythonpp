#ifndef CYTHONPP_DOMAIN_DIAGNOSTICS_DIAGNOSTIC_H
#define CYTHONPP_DOMAIN_DIAGNOSTICS_DIAGNOSTIC_H

#include <string>

namespace cythonpp::domain::diagnostics {

// Warning/Error rather than the SCREAMING_CASE used by token_category and
// token_type, deliberately: ERROR is an object-like macro in <wingdi.h>, and
// macro substitution happens before scoping, so Severity::ERROR would fail to
// compile in any translation unit that ever pulls in <windows.h>. The
// SCREAMING enums elsewhere are bit-flag and packed-value constants where the
// C-style spelling carries meaning; this is an ordinary scoped enum.
enum class Severity { Warning, Error };

// One problem found in one source file.
//
// `code` holds the Python exception name -- "TabError", "IndentationError" --
// so a later front end can match CPython's wording, and so a caller can branch
// on the kind of problem without parsing `message`.
//
// The source path is deliberately absent. A domain pass only ever sees one
// file's tokens and has no way to know its path; the caller that does know
// attaches it on the way out, at ports::DiagnosticsReporter.
struct Diagnostic {
    Severity severity;
    std::string code;
    std::string message;
    int line;
    int column;
};

} // namespace cythonpp::domain::diagnostics

#endif // CYTHONPP_DOMAIN_DIAGNOSTICS_DIAGNOSTIC_H
