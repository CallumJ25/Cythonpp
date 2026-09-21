#ifndef CYTHONPP_DOMAIN_SEMANTIC_LITERAL_GUARD_H
#define CYTHONPP_DOMAIN_SEMANTIC_LITERAL_GUARD_H

#include "domain/ast/expr.h"

namespace cythonpp::domain::semantic {

// `is_literal_true` USED TO LIVE in type_checker.cpp, and was DELETED
// 2026-09-17 rather than kept as a forwarder. It answered "is this a Constant
// of BOOL_TRUE" for always_returns' and statement_always_leaves' While arms --
// a second, independent notion of "literally true" alongside
// literal_guard_verdict's, free to drift from it. It had already drifted: it
// answered false for `while 1:` and `while 2:`, which mypy treats as
// always-true loop conditions exactly as it treats `while True:`. Both call
// sites now ask `literal_guard_verdict(...) == GuardVerdict::AlwaysTrue`.
//
// THIS IS A HEADER, AND THAT IS THE WHOLE POINT OF IT (2026-09-21). The fold
// used to be a file-local function inside type_checker.cpp's anonymous
// namespace, forward-declared near the top of that file so visit(If)/
// visit(While) could reach it. codegen then needed the SAME question --
// Emitter::collect_scope_variables must not let a statically-dead arm's
// assignment decide a name's C++ slot type, which is the emitter half of the
// dead-arm-binding fix -- and copying the fold into domain/codegen/ would
// have recreated, verbatim, the two-rival-notions situation that got
// `is_literal_true` deleted. So the implementation MOVED here unchanged
// (byte-identical logic, and every measurement comment with it) rather than
// being duplicated. THESE ARE THE ONE FOLD SET. There must never be a second.
//
// It lives under domain/semantic/ and not somewhere neutral because the
// question it answers is a SEMANTIC one -- "would mypy prune this branch" --
// measured against mypy's own is_true_literal/is_false_literal. codegen is a
// consumer of that answer, not a co-owner of it.

// What a guard evaluates to. `Unknown` means BOTH arms stay live, which is
// the pre-existing behaviour and the safe default -- a verdict is only ever
// an invitation to prune, never a requirement.
enum class GuardVerdict {
    Unknown,      // not decidable -- BOTH arms live, the pre-existing behaviour
    AlwaysTrue,   // `if True:` / `if <narrowed>:`     -- the ELSE arm is dead
    AlwaysFalse,  // `if False:` / `if not <narrowed>:` -- the BODY is dead
};

// The measurements, the deliberate omissions and the three traps this fold
// must not fall into are all on the DEFINITION, in literal_guard.cpp. Read
// them before widening the set: the gate is the AST SHAPE and never the type,
// the int-literal test is a per-character decimal WHITELIST, and underscore
// placement plus a leading zero are validated because the lexer hands over
// spellings Python's own grammar rejects.
GuardVerdict literal_guard_verdict(const ast::Expr& condition);

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_LITERAL_GUARD_H
