#include <string>

#include <gtest/gtest.h>

#include "domain/codegen/emitter.h"
#include "emitter_fixture.h"

namespace cythonpp::domain::codegen {
namespace {

std::optional<std::string> emit_module(Fixture& fixture) {
    Emitter emitter(fixture.types, fixture.emit_sink);
    return emitter.emit_module(*fixture.module);
}

TEST(EmitterModule, IncludesTheRuntimeAndWrapsTopLevelCodeInMain) {
    Fixture fixture = build("print(1)\n");
    const std::string text = emit_module(fixture).value();

    EXPECT_NE(text.find("#include \"cythonpp/cythonpp.h\""), std::string::npos);
    EXPECT_NE(text.find("int main()"), std::string::npos);
    EXPECT_NE(text.find("py::print(py::int_(1));"), std::string::npos);
    EXPECT_NE(text.find("return 0;"), std::string::npos);
}

// A module-level variable must be a FILE-SCOPE global, not a main() local, or
// a def could not read it.
TEST(EmitterModule, ModuleVariablesAreFileScopeGlobalsAssignedInMain) {
    Fixture fixture = build("total: int = 0\n\n\ndef show() -> None:\n    print(total)\n\n\nshow()\n");
    const std::string text = emit_module(fixture).value();

    const std::size_t declaration = text.find("py::int_ cy_total;");
    const std::size_t main_start = text.find("int main()");
    ASSERT_NE(declaration, std::string::npos);
    ASSERT_NE(main_start, std::string::npos);
    EXPECT_LT(declaration, main_start) << "the declaration must precede main";
    EXPECT_GT(text.find("cy_total = py::int_(0);"), main_start)
        << "the assignment must happen inside main";
}

// A body may call a function defined LATER in the file; Python resolves that
// at call time and C++ does not.
TEST(EmitterModule, EveryFunctionIsForwardDeclaredBeforeAnyDefinition) {
    Fixture fixture = build(
        "def a(n: int) -> int:\n    return b(n)\n\n\ndef b(n: int) -> int:\n    return n\n");
    const std::string text = emit_module(fixture).value();

    const std::size_t forward = text.find("py::int_ cy_b(py::int_ cy_n);");
    const std::size_t definition_of_a = text.find("py::int_ cy_a(py::int_ cy_n) {");
    ASSERT_NE(forward, std::string::npos) << "b must be forward declared";
    ASSERT_NE(definition_of_a, std::string::npos);
    EXPECT_LT(forward, definition_of_a);
}

// All-or-nothing: one refusal anywhere yields no text at all.
TEST(EmitterModule, OneRefusalYieldsNoModuleText) {
    Fixture fixture = build("print(1)\nxs: list[int] = []\n");
    EXPECT_FALSE(emit_module(fixture).has_value());
    EXPECT_FALSE(fixture.emit_sink.empty());
}

// A bare declaration binds nothing in Python (no executable statement), but
// the C++ variable still needs to exist at file scope so a later assignment
// or a def reading it has something to refer to.
//
// POST-WAVE CRITICAL: this test used to omit the `total = 5` line, and that
// omission made it pin a DEFECT rather than a property. `total: int` alone
// binds nothing at all (bound_name_of says exactly this), so `print(total)`
// inside `show()` is `NameError: name 'total' is not defined` under CPython
// (mypy --strict: Success) while the emitted program printed `0` and exited
// 0 -- silently wrong output on a program the union rule rejects. The
// MODULE-level analogue (`total: int` then a top-level `print(total)`) was
// already refused by the wave's own in-scope check; only the read from
// inside a function slipped through, which is the hole this round closes.
// The assignment is added so the test pins the file-scope-declaration
// property on a program both oracles accept; the refusal it was masking is
// pinned separately, below.
TEST(EmitterModule, BareAnnotationStillGetsAFileScopeDeclaration) {
    Fixture fixture =
        build("total: int\ntotal = 5\n\n\ndef show() -> None:\n    print(total)\n\n\nshow()\n");
    const std::string text = emit_module(fixture).value();

    EXPECT_NE(text.find("py::int_ cy_total;"), std::string::npos);
}

// TASK 8, point 2: function_declared_ is empty at module level (it exists
// only for the CURRENTLY-EMITTING function's body), so a module-level
// reassignment needs its OWN record -- module_declared_ -- to widen against
// the DECLARED type rather than the value's own type. Without it, `x = 2`
// would emit a bare `py::int_(2)` assigned into a `py::float_` global, which
// does not compile.
TEST(EmitterModule, ModuleLevelReassignmentWidensAgainstTheDeclaredType) {
    Fixture fixture = build("x: float = 1\nx = 2\n");
    const std::string text = emit_module(fixture).value();

    EXPECT_NE(text.find("py::float_ cy_x;"), std::string::npos);
    EXPECT_NE(text.find("cy_x = py::to_float(py::int_(1));"), std::string::npos)
        << "the very first assignment must also widen, since the file-scope\n"
           "declaration -- not this statement -- is what makes cy_x a float";
    EXPECT_NE(text.find("cy_x = py::to_float(py::int_(2));"), std::string::npos)
        << "the reassignment must widen against the DECLARED float, not its\n"
           "own value's int type";
}

// The declaration itself must appear exactly once even though the name is
// assigned twice at module level -- only the FIRST occurrence declares.
TEST(EmitterModule, ModuleLevelReassignmentDoesNotRedeclare) {
    Fixture fixture = build("x: float = 1\nx = 2\n");
    const std::string text = emit_module(fixture).value();

    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = text.find("py::float_ cy_x;", pos)) != std::string::npos) {
        ++count;
        pos += 1;
    }
    EXPECT_EQ(count, 1u);
}

// FINAL-REVIEW CRITICAL 3, module half: a module-level `if`/`while` body is
// the SAME Python scope as the module, so a name first assigned there is an
// ordinary module variable and belongs in the file-scope prelude. This pass
// used to walk module.body() alone, so the assignment inside the block
// referenced a name declared nowhere at all.
TEST(EmitterModule, AVariableFirstAssignedInsideAModuleLevelBlockIsStillFileScope) {
    Fixture fixture = build("x: int = 1\nif x > 0:\n    y: int = 2\nelse:\n    y = 3\nprint(y)\n");
    const std::string text = emit_module(fixture).value();

    const std::size_t declaration = text.find("py::int_ cy_y;");
    const std::size_t main_start = text.find("int main()");
    ASSERT_NE(declaration, std::string::npos) << "cy_y must be declared at all";
    ASSERT_NE(main_start, std::string::npos);
    EXPECT_LT(declaration, main_start) << "and at FILE scope, not inside the if";
}

// FINAL-REVIEW CRITICAL 3, the decision half at MODULE scope. Hoisting alone
// would happily emit this: `cy_y` default-constructs to 0 and `print(y)`
// would say 0 where CPython raises NameError and exits 1. Both are programs
// CPython REJECTS, so silently printing something is the never-acceptable
// direction; the definite-assignment check refuses instead. (The exact shape
// the review reported -- `if x > 0: y: int = 2` with no else -- is the first
// entry here.)
TEST(EmitterModule, AModuleVariableAssignedOnlyInOneBranchIsRefusedRatherThanDefaulted) {
    for (const std::string source :
         {std::string("x: int = 1\nif x > 0:\n    y: int = 2\nprint(y)\n"),
          std::string("x: int = 1\nwhile x > 0:\n    y: int = 2\n    x = 0\nprint(y)\n"),
          std::string("x: int = 1\nif x > 0:\n    y: int = 2\nelse:\n    print(y)\n")}) {
        Fixture fixture = build(source);
        EXPECT_FALSE(emit_module(fixture).has_value()) << source;
        EXPECT_FALSE(fixture.emit_sink.empty()) << source;
    }
}

// The same for a `while` body and for an `else` clause, which are the other
// two suites the collection has to recurse into.
TEST(EmitterModule, AVariableFirstAssignedInsideAModuleLevelLoopIsStillFileScope) {
    Fixture fixture =
        build("n: int = 0\nwhile n < 1:\n    n = n + 1\n    z: int = 5\nelse:\n    w: int = 6\n");
    const std::string text = emit_module(fixture).value();

    EXPECT_NE(text.find("py::int_ cy_z;"), std::string::npos);
    EXPECT_NE(text.find("py::int_ cy_w;"), std::string::npos);
}

// POST-WAVE CRITICAL: the module-scope definite-assignment refusal above,
// carried across the FUNCTION boundary. The wave that added that refusal
// checked reads WITHIN the module body and WITHIN each function body, but
// never a function's read of a module-level global -- so
// `if c: s: str = "cfg"` plus a `def` reading `s` emitted, compiled, and
// printed "!" where CPython raises NameError and exits 1. Silently wrong
// OUTPUT on a program the union rule rejects.
//
// Every source here was measured against CPython 3.14 and mypy 1.18.1: all
// four are `mypy --strict` Success, and all four raise
// `NameError: name 's' is not defined` under CPython.
TEST(EmitterModule, AFunctionReadingAConditionallyAssignedGlobalIsRefused) {
    for (const std::string source :
         {std::string("c: bool = False\nif c:\n    s: str = \"cfg\"\n\n\n"
                      "def label() -> str:\n    return s + \"!\"\n\n\nprint(label())\n"),
          // The read nested in a call argument rather than a return value.
          std::string("c: bool = False\nif c:\n    s: str = \"cfg\"\n\n\n"
                      "def show() -> None:\n    print(s)\n\n\nshow()\n"),
          // Assigned only in a `while` body, which may run zero times.
          std::string("c: bool = False\nwhile c:\n    s: str = \"cfg\"\n    c = False\n\n\n"
                      "def label() -> str:\n    return s + \"!\"\n\n\nprint(label())\n"),
          // The read in the function's own `if` condition.
          std::string("c: bool = False\nif c:\n    s: str = \"cfg\"\n\n\n"
                      "def label() -> str:\n    if s == \"cfg\":\n        return \"y\"\n"
                      "    return \"n\"\n\n\nprint(label())\n"),
          // A bare `total: int` binds NOTHING in Python, so a function
          // reading it is the same hole with no `if` involved at all -- the
          // shape BareAnnotationStillGetsAFileScopeDeclaration above used to
          // assert emitted. CPython: NameError, exit 1. mypy: Success.
          std::string("total: int\n\n\ndef show() -> None:\n    print(total)\n\n\nshow()\n")}) {
        Fixture fixture = build(source);
        EXPECT_FALSE(emit_module(fixture).has_value()) << source;
        EXPECT_FALSE(fixture.emit_sink.empty()) << source;
    }
}

// The two shadowing controls, at unit level: a name the function BINDS
// itself, and a name a PARAMETER carries, are the function's own and never
// reach the module-level global at all -- so neither may be refused by the
// check above. Both are CPython exit 0 (measured); the execution test of the
// same name in codegen_execution_test.cpp runs them for real.
TEST(EmitterModule, AShadowedGlobalNameIsNotRefusedByTheCrossBoundaryCheck) {
    for (const std::string source :
         {// A parameter of the same name.
          std::string("c: bool = False\nif c:\n    s: str = \"cfg\"\n\n\n"
                      "def label(s: str) -> str:\n    return s + \"!\"\n\n\n"
                      "print(label(\"arg\"))\n"),
          // A local the function assigns before reading.
          std::string("c: bool = False\nif c:\n    s: str = \"cfg\"\n\n\n"
                      "def label() -> str:\n    s = \"own\"\n    return s + \"!\"\n\n\n"
                      "print(label())\n")}) {
        Fixture fixture = build(source);
        EXPECT_TRUE(emit_module(fixture).has_value()) << source;
        EXPECT_TRUE(fixture.emit_sink.empty()) << source;
    }
}

// The deliberate BOUNDARY of the check, kept as a pinned control so a future
// widening is a conscious decision rather than an accident: the set is keyed
// on "not bound by the END of the module body", which says nothing about
// CALL ORDER. `t` here IS assigned at module level, just after the call that
// reads it, so it stays out of module_unbound_ and this program is emitted.
// CPython rejects it (`NameError: name 't' is not defined`) -- a
// PRE-EXISTING gap that predates the cross-boundary check and needs
// call-order reasoning this stage does not have. Not closed here; pinned so
// its status is visible.
TEST(EmitterModule, AGlobalAssignedAfterTheCallThatReadsItIsStillEmitted) {
    Fixture fixture = build("def g() -> int:\n    return t\n\n\nprint(g())\nt: int = 5\n");

    EXPECT_TRUE(emit_module(fixture).has_value());
    EXPECT_TRUE(fixture.emit_sink.empty());
}

// DECISION 0 BACKSTOP, 2026-09-16: emit_value_widened refuses a value whose
// C++ spelling neither matches the declared slot's nor widens into it, rather
// than emitting text clang++ rejects. Both shapes below are ones the SEMANTIC
// layer misses -- mypy reports `Name "x" already defined on line N
// [no-redef]` for an annotated redefinition of an already-assigned module
// name, which this compiler does not model (Phase 2 binds every module-level
// annotation before any assignment is walked, so the collision is never
// seen). Measured at 58b5ef5: cythonpp exited 0, wrote the file, and
// clang++ said `no viable overloaded '='`. The missed diagnostic stays open,
// deliberately -- it is a missed error, the safe direction -- but the third
// state does not.
TEST(EmitterModule, AValueThatCannotFitItsDeclaredSlotIsRefusedNotEmitted) {
    for (const std::string source :
         {// Flat: the annotated redefinition needs no block at all.
          std::string("x = 1\nx: float = 2.5\nprint(x)\n"),
          // The block-nested sibling, where the hoisted declaration's type
          // comes from the `if` body's assignment.
          std::string("c: bool = True\nif c:\n    x = 1\nx: float = 2.5\nprint(x)\n")}) {
        Fixture fixture = build(source);
        EXPECT_FALSE(emit_module(fixture).has_value()) << source;
        ASSERT_FALSE(fixture.emit_sink.empty()) << source;
        EXPECT_NE(fixture.emit_sink.diagnostics().front().message.find(
                      "a value of type \"float\" where \"int\" is required"),
                  std::string::npos)
            << source << " -> " << fixture.emit_sink.diagnostics().front().message;
    }
}

// ADVERSARIAL REVIEW, 2026-09-16, CRITICAL, pre-existing. py::print returns
// `void` -- the only void-returning function in the runtime, since py::len
// returns an int_ -- while the TypeMap types a `print(...)` CALL as NoneType,
// spelled "py::none_t". So a print call used as a VALUE matched its slot's C++
// spelling exactly, which means emit_value_widened's own Decision 0 backstop
// (added in the very commit this review covers) compared the two, found them
// equal, and let it through. Measured 2026-09-16: all four shapes were mypy
// `Success`, CPython exit 0, cythonpp exit 0 WITH the .cpp written, and
// clang++ rejecting it -- `no viable overloaded '='`, `no viable conversion
// from ... 'void'`, and `cannot convert argument of incomplete type 'void'`.
//
// The refusal is POSITIONAL: a print call is legal as a whole statement and
// illegal wherever its value is consumed. That is why the last shape matters
// -- the inner call is an argument to print itself, which is deliberately
// widening-EXEMPT and so is reached by no type-based guard at all.
TEST(EmitterModule, APrintCallUsedAsAValueIsRefusedNotEmitted) {
    for (const std::string source :
         {// An assignment initializer.
          std::string("x: None = print(\"a\")\nprint(x)\n"),
          // A `return` value.
          std::string("def f() -> None:\n    return print(\"a\")\n\n\nf()\n"),
          // A user function's call argument.
          std::string("def f(v: None) -> None:\n    return v\n\n\nf(print(\"a\"))\n"),
          // Nested inside print itself, the widening-exempt path.
          std::string("print(print(\"a\"))\n")}) {
        Fixture fixture = build(source);
        EXPECT_FALSE(emit_module(fixture).has_value()) << source;
        ASSERT_FALSE(fixture.emit_sink.empty()) << source;
        EXPECT_NE(fixture.emit_sink.diagnostics().front().message.find(
                      "a call to 'print' used as a value"),
                  std::string::npos)
            << source << " -> " << fixture.emit_sink.diagnostics().front().message;
    }
}

// CONTROL: a print call as a whole STATEMENT is exactly what the refusal above
// must not touch -- at module level, inside a block (reached through a
// different suite walk), and inside a function body. Also pins that `len`, the
// other modelled builtin, is unaffected: it returns an int_, not void, so it
// stays legal as a value.
TEST(EmitterModule, APrintStatementAndALenValueStayEmittable) {
    Fixture fixture = build("c: bool = True\n"
                            "n: int = len(\"abc\")\n"
                            "print(n)\n"
                            "if c:\n"
                            "    print(\"in a block\")\n"
                            "def show() -> None:\n"
                            "    print(\"in a body\")\n"
                            "\n\nshow()\n");
    EXPECT_TRUE(emit_module(fixture).has_value());
    EXPECT_TRUE(fixture.emit_sink.empty());
}

} // namespace
} // namespace cythonpp::domain::codegen
