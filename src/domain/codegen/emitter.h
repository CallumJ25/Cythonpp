#ifndef CYTHONPP_DOMAIN_CODEGEN_EMITTER_H
#define CYTHONPP_DOMAIN_CODEGEN_EMITTER_H

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// stmt.h rather than a forward declaration of ast::Stmt: emit_suite's
// signature names StmtPtr, which is that header's alias, and a forward
// declaration cannot supply it.
#include "domain/ast/stmt.h"
#include "domain/ast/visitor.h"
#include "domain/diagnostics/diagnostic_sink.h"
#include "domain/semantic/type.h"
#include "domain/semantic/type_map.h"

namespace cythonpp::domain::ast {
class Expr;
class Module;
} // namespace cythonpp::domain::ast

namespace cythonpp::domain::codegen {

// Turns a type-checked ast::Module into standalone C++ source.
//
// AN ast::Visitor, NOT AN ast::RecursiveVisitor, and the choice is the
// inverse of TypeChecker's for the inverse reason. RecursiveVisitor's default
// walks an unhandled node's children anyway; for a type checker that is a
// missed rule, but for an emitter it is a SILENTLY DROPPED STATEMENT. The
// twenty-six pure-virtual visit methods force a new AST node to be considered
// at the exact spot needing attention.
//
// ONE CLASS ACROSS THREE TRANSLATION UNITS (emitter_expressions.cpp,
// emitter_statements.cpp, emitter_module.cpp), the same arrangement
// expression_typer.cpp and expression_typer_calls.cpp already use, and for
// the same reason: size. Two Visitor SUBCLASSES would mean fifty-two
// overrides for twenty-six nodes, half of them stubs with nothing honest to
// report.
//
// OPERATOR PRECEDENCE IS NOT A PROBLEM THIS CLASS HAS. Every Python operation
// emits as a runtime function CALL -- py::mul(py::add(a, b), c) -- so there is
// no precedence to reconstruct, no associativity to preserve, and no
// parenthesisation logic to get wrong. That is also why a single append-only
// buffer suffices: sub-expressions compose in source order.
//
// FAILURE IS ALL-OR-NOTHING. Any refusal sets failed_, and every public entry
// point yields nullopt when it is set, so a partially-emitted file never
// reaches disk. A half-written .cpp whose failure surfaces only when the user
// runs clang++ on it is precisely the silent miscompile this stage exists to
// prevent.
class Emitter : public ast::Visitor {
public:
    Emitter(const semantic::TypeMap& types, diagnostics::DiagnosticSink& sink);

    // The whole module as one .cpp, or nullopt if anything was refused.
    // Implemented in emitter_module.cpp (Task 8).
    std::optional<std::string> emit_module(const ast::Module& module);

    // Test-only seam: emit one expression in isolation. Production code goes
    // through emit_module. Exposed because an expression's emitted text is
    // the unit worth pinning, and reaching it through a whole module would
    // make every expression test assert on boilerplate too.
    std::optional<std::string> emit_expression_for_test(const ast::Expr& expr);

    // Test-only seam: emit one statement in isolation, the statement-side
    // counterpart of emit_expression_for_test above, for exactly the same
    // reason -- pinning one statement's emitted text without dragging in a
    // whole module's boilerplate.
    std::optional<std::string> emit_statement_for_test(const ast::Stmt& statement);

    // Expressions (emitter_expressions.cpp, Tasks 5-6).
    void visit(const ast::Attribute& node) override;
    void visit(const ast::BinOp& node) override;
    void visit(const ast::BoolOp& node) override;
    void visit(const ast::Call& node) override;
    void visit(const ast::Compare& node) override;
    void visit(const ast::Constant& node) override;
    void visit(const ast::DictExpr& node) override;
    void visit(const ast::ListComp& node) override;
    void visit(const ast::ListExpr& node) override;
    void visit(const ast::Name& node) override;
    void visit(const ast::Subscript& node) override;
    void visit(const ast::TupleExpr& node) override;
    void visit(const ast::UnaryOp& node) override;

    // Statements (emitter_statements.cpp, Task 7).
    void visit(const ast::AnnAssign& node) override;
    void visit(const ast::Assign& node) override;
    void visit(const ast::Break& node) override;
    void visit(const ast::ClassDef& node) override;
    void visit(const ast::Continue& node) override;
    void visit(const ast::ExprStmt& node) override;
    void visit(const ast::For& node) override;
    void visit(const ast::FunctionDef& node) override;
    void visit(const ast::If& node) override;
    void visit(const ast::Pass& node) override;
    void visit(const ast::Return& node) override;
    void visit(const ast::While& node) override;

    // The root (emitter_module.cpp, Task 8).
    void visit(const ast::Module& node) override;

private:
    // Appends to the buffer. The single choke point, so a future change to
    // how text accumulates has exactly one place to go.
    void write(std::string_view text);

    // Emits a sub-expression in place.
    void emit_expr(const ast::Expr& expr);

    // Reports one NotImplementedError naming `what` and marks the emission
    // failed. Takes the node for its span, so the diagnostic points at the
    // construct rather than at the file.
    void refuse(const ast::Node& node, const std::string& what);

    // The static type the checker gave this expression, or nullptr. A null
    // answer is itself a refusal condition at every call site: an expression
    // with no recorded type has no C++ type to emit.
    const semantic::Type* type_of(const ast::Expr& expr) const;

    // Task 6 helpers, declared here so Task 5's file compiles unchanged.
    void emit_binary(const ast::BinOp& node);
    void emit_power(const ast::BinOp& node);
    void emit_call(const ast::Call& node);
    void emit_operand_widened(const ast::Expr& operand, const semantic::Type& result);

    // Task 7-8 helpers.
    void emit_suite(const std::vector<ast::StmtPtr>& body);
    void write_indent();
    void write_line(std::string_view text);
    void emit_assignment(const ast::Expr& target, const ast::Expr& value,
                         const semantic::Type* declared);

    // A function/parameter/AnnAssign annotation Expr resolved to a Type via a
    // throwaway AnnotationResolver -- see emitter_statements.cpp's own
    // "HOLE (B)" comment for why a scratch ClassLookup and a discarded sink
    // are safe for the scalar slice this stage admits. A MEMBER function
    // (rather than a free function local to one translation unit) so
    // emitter_module.cpp's file-scope variable prelude can resolve a
    // module-level AnnAssign's annotation identically to emit_assignment,
    // instead of re-deriving the same throwaway-resolver dance a second time.
    semantic::Type resolve_annotation_type(const ast::Expr& annotation) const;

    // The type an assignment TARGET's declaration should carry: `declared`
    // when given (an AnnAssign's own resolved annotation), else
    // type_of(target) (always nullptr for a Name target -- TypeMap holds no
    // assignment-target entries, see type_map.h's own comment), else
    // type_of(value). Factored out of emit_assignment so emit_module's
    // module-level variable prelude computes the identical answer for the
    // identical statement rather than a second, potentially-drifting copy of
    // this fallback chain.
    const semantic::Type* declared_type_for_assignment(const ast::Expr& target,
                                                       const ast::Expr& value,
                                                       const semantic::Type* declared) const;

    // Writes a FunctionDef's return type, mangled name and parenthesised
    // parameter list -- e.g. "py::int_ cy_f(py::int_ cy_n)" -- with no
    // trailing ';' or '{'. Returns false (having already called refuse()) if
    // any part of the signature is outside this slice. Shared by the
    // forward-declaration pass and the real definition in emitter_module.cpp
    // so the two can never disagree: a forward declaration that disagrees
    // with its definition is a link error at best and a silently wrong call
    // at worst.
    bool write_signature(const ast::FunctionDef& node);

    // Step 3 of emit_module: writes a file-scope C++ declaration for every
    // module-level variable (collect_scope_variables decides the set),
    // recording each type in module_declared_ and each SOURCE identifier in
    // `identifiers`, which emit_module then feeds to the module body's own
    // definite-assignment check. Implemented in emitter_module.cpp; declared
    // here (rather than kept file-local) purely because it is a member -- it
    // has no callers outside that one file.
    void emit_module_variable_declarations(const ast::Module& module,
                                           std::set<std::string>& identifiers);

    // Emits `value`, wrapped in the runtime's explicit py::to_int/py::to_float
    // conversion when `value`'s own static type is a PROPER subtype of
    // `target` within Python's numeric tower (bool <: int <: float) -- a real
    // subtyping relationship is_subtype enforces, so `x: float = 1` and
    // `def f(x: bool) -> int: return x` both type-check clean under mypy, yet
    // py::int_/py::bool_ have no implicit conversion to py::float_/py::int_
    // (see runtime/cythonpp/int_.h's own comment: that is deliberate, so the
    // add/sub/mul overload sets stay unambiguous). Backs every site that puts
    // a value where a DECLARED C++ type is expected rather than an inferred
    // one: emit_assignment's initializer and visit(Return)'s value.
    void emit_value_widened(const ast::Expr& value, const semantic::Type& target);

    // One variable a scope (a module body or one function body) must declare,
    // in the source order its first assignment appears.
    struct ScopeVariable {
        std::string identifier; // as written in the Python source
        std::string mangled;    // as written in the emitted C++
        semantic::Type type;    // the type its C++ declaration carries
    };

    // FINAL-REVIEW CRITICAL 3, half one: every name a scope assigns ANYWHERE
    // -- including inside an `if`/`while` body or its `else` clause, at any
    // depth -- in the source order of its FIRST assignment. Python scoping is
    // per-FUNCTION (and per-module), not per-block, so a name first assigned
    // inside a block is still an ordinary local of the enclosing scope and its
    // C++ declaration must sit at that scope's top rather than inside the
    // block's braces. Emitting it at the first assignment gave it C++ BLOCK
    // scope, so a read (or another branch's assignment) outside that block
    // referenced a name no longer in scope: `use of undeclared identifier`.
    //
    // Never descends into a nested def or class -- those are different scopes,
    // and this stage refuses both anyway. A target that is not a plain
    // manglable Name is skipped rather than refused, so the ordinary
    // per-statement walk reports it once with the right wording.
    //
    // Returns false (having already refused) if a collected name has no
    // representable C++ type.
    bool collect_scope_variables(const std::vector<ast::StmtPtr>& body,
                                 const std::map<std::string, semantic::Type>& already_declared,
                                 std::vector<ScopeVariable>& variables);

    // FINAL-REVIEW CRITICAL 3, half two: a hoisted C++ declaration
    // DEFAULT-CONSTRUCTS, so a name whose only assignment sits in a branch
    // that does not run would read as 0/""/false in C++ where Python raises
    // UnboundLocalError. That is silently wrong OUTPUT, the one failure this
    // whole stage exists to prevent, so the shape is REFUSED rather than
    // accepted -- see this wave's report for the full argument.
    //
    // A conservative, purely syntactic definite-assignment walk: statements in
    // order, a name becomes bound at an assignment to it, an `if` contributes
    // only the names BOTH arms bind (an arm that always leaves contributes
    // nothing to intersect), and a loop body contributes nothing at all since
    // it may run zero times. Every read of a scope-local name not yet
    // definitely bound is refused by name. Only false refusals are possible:
    // the walk never concludes "bound" where Python would not have bound.
    //
    // `bound` is in/out (seeded with a function's parameter names, empty for a
    // module). Returns whether the suite always leaves via return/break/
    // continue, which is what lets an `if` arm be excluded from the merge.
    //
    // `unbound_phrase` is the clause every refusal this walk produces names
    // the problem with -- see check_reads. It is a parameter rather than a
    // fixed string because the SAME walk answers two different questions: a
    // scope's reads of its own not-yet-assigned locals, and (over a function
    // body, seeded with module_unbound_) that function's reads of a
    // module-level global the module body may never assign.
    bool check_definite_assignment(const std::vector<ast::StmtPtr>& body,
                                   const std::set<std::string>& locals,
                                   std::set<std::string>& bound,
                                   std::string_view unbound_phrase);

    // The two clauses check_reads words a refusal with. Separate strings, not
    // one generic wording, because the two shapes have different CAUSES and a
    // user reading the diagnostic needs to know which. The first is a read
    // within one scope's own body -- a function's, or the module's -- of a
    // name that scope may not have assigned yet; the second is the
    // cross-boundary one: a function reading a MODULE-level global whose
    // assignment sits in a block that may not run, which is a fact about the
    // module body and nothing the function itself can fix.
    static constexpr std::string_view kScopeUnboundPhrase =
        "this scope may not have assigned yet";
    static constexpr std::string_view kModuleUnboundPhrase =
        "the module body may never assign, so it may be unbound here";

    // Refuses every read, anywhere in `expr`, of a `locals` name not in
    // `bound`, wording the refusal as "a read of 'x', which <unbound_phrase>,".
    // Walks the whole sub-tree, so a read nested in a call argument or an
    // operand is seen.
    void check_reads(const ast::Expr& expr, const std::set<std::string>& locals,
                     const std::set<std::string>& bound, std::string_view unbound_phrase);

    // Names already declared in the CURRENT function body, mapped to the C++
    // type each was declared with. Empty at module level, where declarations
    // live in the file-scope prelude instead. A MAP rather than a set of
    // names: a REASSIGNMENT (`x = 2.5` with no annotation of its own) must
    // widen against the type the name was first declared with, not against
    // whatever this particular value's own type happens to be -- the two can
    // legitimately differ (`x: float = 1.0` then `x = 2`), and a mere set
    // cannot answer "declared as what".
    std::map<std::string, semantic::Type> function_declared_;
    bool at_module_level_ = true;

    // The expression the enclosing ExprStmt is currently emitting, or nullptr
    // when no statement-level expression is in flight. Exists for exactly one
    // question: is this `print(...)` call a whole STATEMENT, or is its value
    // being consumed? py::print returns `void` while the TypeMap types the
    // call as NoneType, so a consumed print call type-checks, matches its
    // slot's C++ spelling, slips past emit_value_widened's Decision 0
    // backstop, and then fails at clang++ -- see emit_call's own comment for
    // the three measured shapes.
    //
    // A NODE POINTER rather than a bool, deliberately: a bool set for the
    // duration of the statement would also authorise every print call
    // NESTED inside that statement (`print(print("a"))`, `f(print("a"))`),
    // which is exactly the set that must be refused. Only the identical node
    // is the statement.
    const ast::Expr* statement_expression_ = nullptr;

    // The module-scope counterpart of function_declared_: the declared type
    // of every module-level variable, populated once by emit_module's
    // file-scope prelude before main() is emitted. function_declared_ is
    // swapped out and empty at module level (it exists only for the
    // CURRENTLY-EMITTING function's body), so a module-level reassignment
    // (`x = 2` after `x: float = 1`) needs its OWN record to widen against --
    // without it, emit_assignment's existing-declaration lookup finds
    // nothing at module level and falls back to the value's own type,
    // producing e.g. a bare `py::int_(2)` assigned into a `py::float_`
    // global, which does not compile.
    std::map<std::string, semantic::Type> module_declared_;

    // POST-WAVE CRITICAL: every module-level variable NOT definitely bound by
    // the END of the module body, as the module's own definite-assignment walk
    // left it. A file-scope C++ declaration DEFAULT-CONSTRUCTS, so reading one
    // of these names yields 0/""/false where CPython raises NameError -- the
    // identical silently-wrong-OUTPUT failure check_definite_assignment
    // already refuses WITHIN a scope, simply not carried across the function
    // boundary when that check landed. visit(FunctionDef) seeds a second walk
    // of its own body with this set, so `if c: s: str = "x"` at module level
    // followed by a `def` that reads `s` is a named refusal rather than a
    // program that prints the wrong thing.
    //
    // Keyed on "not bound by the END of the module body", deliberately, so it
    // says nothing about CALL ORDER: a global assigned at module level only
    // AFTER the call that reads it (`def g(): return t` / `print(g())` /
    // `t: int = 5`) is definitely bound by the end of the body and stays out
    // of this set. That shape is a separate, pre-existing gap needing
    // call-order reasoning this stage does not have; it is deliberately left
    // exactly as it was rather than half-closed here.
    std::set<std::string> module_unbound_;

    // The enclosing function's declared return type, consulted by
    // visit(Return) to widen its value exactly as emit_assignment does for an
    // initializer. Meaningless at module level (Python itself rejects a
    // `return` there, so nothing reads this while at_module_level_ is true).
    semantic::Type return_type_;

    // Whether the loop currently being emitted declared a `_cy_broke_<depth>`
    // flag, i.e. whether it has an `else` clause. Saved and restored around a
    // loop's own body exactly as at_module_level_ is around a function's,
    // because loops nest: an inner loop with no else, inside an outer one
    // that has one, must still emit a bare `break;` rather than reference a
    // flag only the OUTER loop declared.
    bool loop_has_else_ = false;

    // FINAL-REVIEW IMPORTANT 4: whether the statement currently being emitted
    // sits inside an `if`/`while` suite rather than directly in its scope's
    // own statement list. at_module_level_ alone cannot answer this -- it
    // stays TRUE throughout a module-level `if` body, since that body is still
    // emitted into main() -- so visit(FunctionDef)'s nested-def refusal never
    // fired for `if True:` + a `def`, and C++ (which has no function
    // definitions inside a block at all) rejected the result. Saved and
    // restored around each suite exactly as loop_has_else_ is.
    bool in_conditional_block_ = false;

    const semantic::TypeMap& types_;
    diagnostics::DiagnosticSink& sink_;
    std::string out_;
    bool failed_ = false;
    int indent_ = 0;
    int loop_depth_ = 0;
};

} // namespace cythonpp::domain::codegen

#endif // CYTHONPP_DOMAIN_CODEGEN_EMITTER_H
