#ifndef CYTHONPP_DOMAIN_LEXER_INDENTATION_PASS_H
#define CYTHONPP_DOMAIN_LEXER_INDENTATION_PASS_H

#include "domain/diagnostics/diagnostic_sink.h"
#include "token_stream.h"

namespace cythonpp::domain::lexer {

// Replaces the scanner's per-character SPACE/TAB runs with INDENT/DEDENT
// tokens, recovering the block structure Python's grammar is built on.
//
// A separate pass rather than logic inside Lexer, which is what CPython does:
// the scanner deliberately preserves which whitespace character was used so
// that Python's tab rules can be applied here, and keeping the two apart means
// this can be tested by feeding it a token vector instead of round-tripping
// through source strings.
//
// Guarantees, both of which the parser will lean on:
//
//  - Total. An unbalanced dedent or an ambiguous tab is a diagnostic, never an
//    exception, so a caller needs no try/catch and still gets a usable stream.
//  - Balanced. The output always contains exactly as many DEDENTs as INDENTs,
//    even for input that is malformed in every way at once.
//
// Input is expected to come straight from Lexer::tokenize(). The contract it
// relies on: SPACE/TAB appear only as a contiguous run at the start of a
// logical line that has content, NEWLINE appears only at the end of a logical
// line, and blank and comment-only lines contribute no indentation.
class IndentationPass {
public:
    // Diagnostics are appended to `sink` in source order. The sink is a
    // parameter rather than a member so that running the pass twice cannot
    // accumulate duplicate diagnostics.
    TokenStream run(const TokenStream& tokens, diagnostics::DiagnosticSink& sink) const;
};

} // namespace cythonpp::domain::lexer

#endif // CYTHONPP_DOMAIN_LEXER_INDENTATION_PASS_H
