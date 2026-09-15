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
TEST(EmitterModule, BareAnnotationStillGetsAFileScopeDeclaration) {
    Fixture fixture = build("total: int\n\n\ndef show() -> None:\n    print(total)\n\n\nshow()\n");
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

} // namespace
} // namespace cythonpp::domain::codegen
