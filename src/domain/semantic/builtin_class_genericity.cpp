#include "builtin_class_genericity.h"

#include <cstddef>

#include "builtin_class_table.h"

namespace cythonpp::domain::semantic {

std::optional<bool> builtin_class_accepts_type_arguments(const std::string& name) {
    // The alias table is deliberately NOT consulted here. Each alias
    // spelling has its own row in kBuiltinClasses (IOError, EnvironmentError
    // and WindowsError are all present), and the generator probed each
    // spelling separately -- so `IOError[int]`, which mypy answers as
    // `"OSError" expects no type arguments`, is already recorded false on
    // IOError's own row. Routing through canonical_name would need
    // ClassLookup, which this function deliberately does not take: the
    // question is about the BUILTIN of that spelling, and a user class
    // shadowing the spelling is the caller's business, not this table's.
    for (std::size_t index = 0; index < kBuiltinClassCount; ++index) {
        if (name == kBuiltinClasses[index].name) {
            return kBuiltinClasses[index].accepts_type_arguments;
        }
    }
    return std::nullopt;
}

} // namespace cythonpp::domain::semantic
