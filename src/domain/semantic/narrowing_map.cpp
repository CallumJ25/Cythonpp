#include "domain/semantic/narrowing_map.h"

#include <set>
#include <utility>

#include "domain/ast/attribute.h"
#include "domain/ast/name.h"

namespace cythonpp::domain::semantic {
namespace {

// True when `path` is a PROPER dotted prefix of `candidate`. The dot boundary
// check is what stops `self.b` from matching `self.bc`.
bool is_proper_prefix(const NarrowedPath& path, const NarrowedPath& candidate) {
    if (candidate.size() <= path.size()) {
        return false;
    }
    if (candidate.compare(0, path.size(), path) != 0) {
        return false;
    }
    return candidate[path.size()] == '.';
}

// The Object collapse on top of an already-computed union -- see
// join_narrowings' own comment for why it is an identity and not a
// heuristic. Applied only to decide the STORED value, never to decide
// whether an entry is trivial (see the "equal to declared" check below,
// which must compare the union BEFORE this collapse): a union that only
// collapses to the declared type because Object absorbed a genuinely
// different contribution (e.g. `int | object` collapsing to `object`) is
// still real information from at least one edge, and must be kept, while a
// union with no distinct contribution at all (every edge already at the
// declared type) collapses to that same declared type without ever forming
// a Union, and that one is the trivial case meant to be dropped.
Type collapse_object_union(Type joined) {
    if (joined.kind != TypeKind::Union) {
        return joined;
    }
    for (const Type& member : joined.args) {
        if (member.kind == TypeKind::Object) {
            return Type::object();
        }
    }
    return joined;
}

} // namespace

std::optional<NarrowedPath> narrowing_path_of(const ast::Expr& expr) {
    if (const auto* name = dynamic_cast<const ast::Name*>(&expr)) {
        return name->identifier();
    }
    if (const auto* attribute = dynamic_cast<const ast::Attribute*>(&expr)) {
        if (const std::optional<NarrowedPath> receiver = narrowing_path_of(attribute->value())) {
            return *receiver + "." + attribute->attribute();
        }
    }
    // Every other shape -- a call, a subscript, an operator, a literal -- is
    // not a path. See the header for why subscripts are excluded by scope.
    return std::nullopt;
}

std::optional<Type> NarrowingMap::get(const NarrowedPath& path) const {
    const auto it = entries_.find(path);
    return it == entries_.end() ? std::nullopt : std::optional<Type>(it->second);
}

void NarrowingMap::set(const NarrowedPath& path, Type type) {
    entries_[path] = std::move(type);
}

void NarrowingMap::kill(const NarrowedPath& path) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->first == path || is_proper_prefix(path, it->first)) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

void NarrowingMap::clear() { entries_.clear(); }

NarrowingState NarrowingMap::snapshot() const { return entries_; }

void NarrowingMap::restore(NarrowingState state) { entries_ = std::move(state); }

NarrowingState join_narrowings(
    const std::vector<NarrowingState>& edges,
    const std::function<std::optional<Type>(const NarrowedPath&)>& declared_type_of) {
    // Every path any edge narrowed. A path no edge touched needs no entry:
    // its declared type is what a missing entry already means.
    std::set<NarrowedPath> paths;
    for (const NarrowingState& edge : edges) {
        for (const auto& entry : edge) {
            paths.insert(entry.first);
        }
    }

    NarrowingState joined;
    for (const NarrowedPath& path : paths) {
        const std::optional<Type> declared = declared_type_of(path);
        if (!declared.has_value()) {
            // Nothing resolvable to layer a narrowing over.
            continue;
        }
        std::vector<Type> contributions;
        contributions.reserve(edges.size());
        for (const NarrowingState& edge : edges) {
            const auto it = edge.find(path);
            contributions.push_back(it == edge.end() ? *declared : it->second);
        }
        // Compared BEFORE the Object collapse: this is what tells "no edge
        // contributed anything but the declared type" (drop) apart from "a
        // real per-edge difference happened to collapse to the declared
        // type anyway" (keep, storing the collapsed value).
        Type raw = Type::union_of(std::move(contributions));
        if (raw == *declared) {
            // Equal to the declared type, so an entry would say nothing a
            // missing one does not.
            continue;
        }
        joined.emplace(path, collapse_object_union(std::move(raw)));
    }
    return joined;
}

} // namespace cythonpp::domain::semantic
