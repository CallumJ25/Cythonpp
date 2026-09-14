#ifndef CYTHONPP_DOMAIN_CODEGEN_EMIT_DIAGNOSTIC_KIND_H
#define CYTHONPP_DOMAIN_CODEGEN_EMIT_DIAGNOSTIC_KIND_H

#include "domain/diagnostics/suppressibility.h"

namespace cythonpp::domain::codegen {

// The closed set of diagnostics the codegen stage can produce, mirroring
// semantic/diagnostic_kind.h's idiom: every report site names a kind rather
// than spelling a code string, so a code string cannot be written at a call
// site at all.
//
// There is exactly ONE kind today, and that is not an oversight waiting to be
// corrected. Codegen makes exactly one kind of claim -- "this compiler cannot
// emit that" -- which is a capability claim, never a judgement about the
// program's types. A second kind would mean codegen had started deciding
// something the semantic layer owns, and that is the point at which someone
// should stop and check whether the check belongs here at all.
enum class EmitDiagnosticKind {
    // "NotImplementedError": a construct this stage cannot emit. The program
    // may still be one both oracles accept -- see CLAUDE.md's union rule.
    UnsupportedConstruct,
};

// BOTH SWITCHES BELOW ARE DELIBERATELY EXHAUSTIVE WITH NO `default` LABEL,
// and CMakeLists.txt passes -Werror=switch, so adding an enumerator without
// answering both questions for it is a COMPILE ERROR at the two spots needing
// attention. Do not add a `default:` here to silence anything.
inline const char* diagnostic_code(EmitDiagnosticKind kind) {
    switch (kind) {
    case EmitDiagnosticKind::UnsupportedConstruct:
        return "NotImplementedError";
    }
    // Unreachable for any valid enumerator; present only because a function
    // returning a value must, to the compiler, appear to always do so.
    return "NotImplementedError";
}

inline diagnostics::Suppressibility suppressibility_of(EmitDiagnosticKind kind) {
    switch (kind) {
    case EmitDiagnosticKind::UnsupportedConstruct:
        return diagnostics::Suppressibility::NotSuppressible;
    }
    // Unreachable. NotSuppressible rather than Suppressible: if this line is
    // ever somehow reached, reporting is the recoverable mistake and silence
    // is not.
    return diagnostics::Suppressibility::NotSuppressible;
}

} // namespace cythonpp::domain::codegen

#endif // CYTHONPP_DOMAIN_CODEGEN_EMIT_DIAGNOSTIC_KIND_H
