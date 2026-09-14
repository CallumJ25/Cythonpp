#include <gtest/gtest.h>

#include "domain/codegen/emit_diagnostic_kind.h"
#include "domain/diagnostics/suppressibility.h"

namespace cythonpp::domain::codegen {
namespace {

TEST(EmitDiagnosticKind, UnsupportedConstructIsNotImplementedError) {
    EXPECT_STREQ(diagnostic_code(EmitDiagnosticKind::UnsupportedConstruct),
                 "NotImplementedError");
}

// A capability claim, never a type judgement, so unreachability cannot drop
// it -- the same classification semantic/diagnostic_kind.h already gives
// NotImplementedError, and for the same reason: an unreachable subtree is
// still handed to codegen, and a construct this compiler cannot model does
// not become modellable by being unreachable.
TEST(EmitDiagnosticKind, UnsupportedConstructIsNeverSuppressible) {
    EXPECT_EQ(suppressibility_of(EmitDiagnosticKind::UnsupportedConstruct),
              diagnostics::Suppressibility::NotSuppressible);
}

} // namespace
} // namespace cythonpp::domain::codegen
