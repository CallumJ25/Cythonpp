#ifndef CYTHONPP_DOMAIN_SEMANTIC_CLASS_LOOKUP_H
#define CYTHONPP_DOMAIN_SEMANTIC_CLASS_LOOKUP_H

#include <string>
#include <vector>

namespace cythonpp::domain::semantic {

// What the type algebra needs to know about user-defined classes.
//
// An abstract collaborator inside the domain, deliberately NOT a ports/
// interface: it abstracts a domain concept rather than an I/O boundary, and
// putting it in ports/ would say the application layer must supply it, which
// is false. Spec 5b implements it on its class table; 5a's tests use a fake.
//
// Three methods rather than three interfaces because is_subtype needs the
// base chain (plus canonical identity, added below) and AnnotationResolver
// needs membership (plus that same canonical identity), and 5b's class table
// implements all three -- splitting them would be a distinction without a
// difference.
class ClassLookup {
public:
    virtual ~ClassLookup() = default;

    virtual bool is_class(const std::string& name) const = 0;

    // Direct bases only; the transitive walk belongs to is_subtype, where it
    // is testable against a fake. Empty for a name that is not a class.
    //
    // Returns BY VALUE, not by const reference: a reference return forces
    // every implementation, the test fake included, to own a persistent empty
    // vector to hand back for the unknown-name case -- a lifetime trap in
    // exactly the path easiest to get wrong. Base lists are two or three
    // short strings, so the copy is free.
    virtual std::vector<std::string> bases_of(const std::string& name) const = 0;

    // The name under which this class is actually known. An alias resolves to
    // its canonical spelling; every other name resolves to itself.
    //
    // A third method rather than a second interface, for the same reason the
    // other two live together: both consumers need it. is_subtype needs
    // canonical identity to compare two Class names, and AnnotationResolver
    // needs it to avoid ever BUILDING a Type::class_of under an alias
    // spelling. Duplicating the alias table into each consumer is what let
    // the two copies disagree: annotation_resolver.cpp's own file-local alias
    // table mapped IOError -> OSError unconditionally, while ClassTable let a
    // live user entry under the exact spelling win, so a program declaring
    // `class IOError: pass` got Class("OSError") from one producer and
    // Class("IOError") from the other -- a false TypeError from is_subtype on
    // an mypy-clean program. One source of truth removes the possibility of
    // that disagreement by construction.
    virtual std::string canonical_name(const std::string& name) const = 0;
};

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_CLASS_LOOKUP_H
