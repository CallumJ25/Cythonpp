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
// Two methods rather than two interfaces because is_subtype needs the base
// chain and AnnotationResolver needs membership, and 5b's class table
// implements both -- splitting them would be a distinction without a
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
};

} // namespace cythonpp::domain::semantic

#endif // CYTHONPP_DOMAIN_SEMANTIC_CLASS_LOOKUP_H
