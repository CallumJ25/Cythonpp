#include "typed_printer.h"

#include <cstddef>
#include <string>
#include <vector>

#include "domain/ast/ann_assign.h"
#include "domain/ast/assign.h"
#include "domain/ast/attribute.h"
#include "domain/ast/bin_op.h"
#include "domain/ast/bool_op.h"
#include "domain/ast/break.h"
#include "domain/ast/call.h"
#include "domain/ast/class_def.h"
#include "domain/ast/compare.h"
#include "domain/ast/constant.h"
#include "domain/ast/continue.h"
#include "domain/ast/dict_expr.h"
#include "domain/ast/expr_stmt.h"
#include "domain/ast/for.h"
#include "domain/ast/function_def.h"
#include "domain/ast/if.h"
#include "domain/ast/list_comp.h"
#include "domain/ast/list_expr.h"
#include "domain/ast/module.h"
#include "domain/ast/name.h"
#include "domain/ast/parameter.h"
#include "domain/ast/pass.h"
#include "domain/ast/return.h"
#include "domain/ast/stmt.h"
#include "domain/ast/subscript.h"
#include "domain/ast/tuple_expr.h"
#include "domain/ast/unary_op.h"
#include "domain/ast/visitor.h"
#include "domain/ast/while.h"
#include "domain/lexer/keyword_table.h"
#include "domain/lexer/operator_table.h"
#include "domain/lexer/token_type_name.h"
#include "type_name.h"

namespace cythonpp::domain::semantic {

namespace {

// Identical to ast_printer.cpp's helper of the same name -- see the header
// comment on why this file mirrors AstPrinter rather than sharing it.
std::string op_spelling(lexer::token_type type) {
    std::string_view spelling = lexer::operator_lexeme_of(type);
    if (!spelling.empty()) {
        return std::string(spelling);
    }
    spelling = lexer::keyword_lexeme_of(type);
    if (!spelling.empty()) {
        return std::string(spelling);
    }
    return std::string(lexer::token_type_name(type));
}

// A sibling of ast::AstPrinter (see typed_printer.h for why this is a
// sibling rather than a subclass): every statement-shaped visit is a
// verbatim copy of AstPrinter's. Every expression-shaped visit renders the
// identical bare form and then, exactly once, appends ":type" if `types_`
// has an entry for that node.
class Renderer : public ast::Visitor {
public:
    explicit Renderer(const TypeMap& types) : types_(types) {}

    std::string render(const ast::Node& node) {
        out_.clear();
        depth_ = 0;
        node.accept(*this);
        return out_;
    }

    void visit(const ast::AnnAssign& node) override {
        out_ += "(AnnAssign ";
        node.target().accept(*this);
        out_ += " ";
        node.annotation().accept(*this);
        if (node.has_value()) {
            out_ += " ";
            node.value().accept(*this);
        }
        out_ += ")";
    }

    void visit(const ast::Assign& node) override {
        out_ += "(Assign ";
        node.target().accept(*this);
        out_ += " ";
        node.value().accept(*this);
        out_ += ")";
    }

    void visit(const ast::Attribute& node) override {
        out_ += "(Attribute ";
        node.value().accept(*this);
        out_ += " " + node.attribute() + ")";
        append_suffix(node);
    }

    void visit(const ast::BinOp& node) override {
        out_ += "(BinOp " + op_spelling(node.op()) + " ";
        node.left().accept(*this);
        out_ += " ";
        node.right().accept(*this);
        out_ += ")";
        append_suffix(node);
    }

    void visit(const ast::BoolOp& node) override {
        out_ += "(BoolOp " + op_spelling(node.op());
        for (const ast::ExprPtr& value : node.values()) {
            out_ += " ";
            value->accept(*this);
        }
        out_ += ")";
        append_suffix(node);
    }

    void visit(const ast::Break&) override { out_ += "(Break)"; }

    void visit(const ast::Call& node) override {
        out_ += "(Call ";
        node.callee().accept(*this);
        for (const ast::ExprPtr& arg : node.args()) {
            out_ += " ";
            arg->accept(*this);
        }
        out_ += ")";
        append_suffix(node);
    }

    void visit(const ast::ClassDef& node) override {
        out_ += "(ClassDef " + node.name();
        if (!node.bases().empty()) {
            out_ += " (Bases";
            for (const ast::ExprPtr& base : node.bases()) {
                out_ += " ";
                base->accept(*this);
            }
            out_ += ")";
        }
        print_body(node.body());
        out_ += ")";
    }

    void visit(const ast::Compare& node) override {
        out_ += "(Compare ";
        node.left().accept(*this);
        for (const ast::Compare::Rest& rest : node.rest()) {
            out_ += " " + op_spelling(rest.op) + " ";
            rest.operand->accept(*this);
        }
        out_ += ")";
        append_suffix(node);
    }

    void visit(const ast::Constant& node) override {
        out_ += "(Constant " + node.lexeme() + ")";
        append_suffix(node);
    }

    void visit(const ast::Continue&) override { out_ += "(Continue)"; }

    void visit(const ast::DictExpr& node) override {
        out_ += "(DictExpr";
        for (const ast::DictExpr::Entry& entry : node.entries()) {
            out_ += " (";
            entry.key->accept(*this);
            out_ += " ";
            entry.value->accept(*this);
            out_ += ")";
        }
        out_ += ")";
        append_suffix(node);
    }

    void visit(const ast::ExprStmt& node) override {
        out_ += "(ExprStmt ";
        node.value().accept(*this);
        out_ += ")";
    }

    void visit(const ast::For& node) override {
        out_ += "(For ";
        node.target().accept(*this);
        out_ += " ";
        node.iterable().accept(*this);
        print_body(node.body());
        print_else(node.orelse());
        out_ += ")";
    }

    void visit(const ast::FunctionDef& node) override {
        out_ += "(FunctionDef " + node.name();
        if (!node.params().empty()) {
            out_ += " (Params";
            for (const ast::Parameter& param : node.params()) {
                out_ += " (Parameter " + param.name;
                if (param.annotation != nullptr) {
                    out_ += " ";
                    param.annotation->accept(*this);
                }
                if (param.default_value != nullptr) {
                    out_ += " (Default ";
                    param.default_value->accept(*this);
                    out_ += ")";
                }
                out_ += ")";
            }
            out_ += ")";
        }
        if (node.has_return_annotation()) {
            out_ += " (Returns ";
            node.return_annotation().accept(*this);
            out_ += ")";
        }
        print_body(node.body());
        out_ += ")";
    }

    void visit(const ast::If& node) override {
        out_ += "(If ";
        node.condition().accept(*this);
        print_body(node.body());
        print_else(node.orelse());
        out_ += ")";
    }

    void visit(const ast::ListComp& node) override {
        out_ += "(ListComp ";
        node.element().accept(*this);
        for (const ast::ComprehensionClause& clause : node.clauses()) {
            out_ += " (Clause ";
            clause.target->accept(*this);
            out_ += " ";
            clause.iterable->accept(*this);
            for (const ast::ExprPtr& condition : clause.conditions) {
                out_ += " ";
                condition->accept(*this);
            }
            out_ += ")";
        }
        out_ += ")";
        append_suffix(node);
    }

    void visit(const ast::ListExpr& node) override {
        out_ += "(ListExpr";
        for (const ast::ExprPtr& element : node.elements()) {
            out_ += " ";
            element->accept(*this);
        }
        out_ += ")";
        append_suffix(node);
    }

    void visit(const ast::Module& node) override {
        out_ += "(Module";
        print_body(node.body());
        out_ += ")";
    }

    void visit(const ast::Name& node) override {
        out_ += "(Name " + node.identifier() + ")";
        append_suffix(node);
    }

    void visit(const ast::Pass&) override { out_ += "(Pass)"; }

    void visit(const ast::Return& node) override {
        out_ += "(Return";
        if (node.has_value()) {
            out_ += " ";
            node.value().accept(*this);
        }
        out_ += ")";
    }

    void visit(const ast::Subscript& node) override {
        out_ += "(Subscript ";
        node.value().accept(*this);
        out_ += " ";
        node.index().accept(*this);
        out_ += ")";
        append_suffix(node);
    }

    void visit(const ast::TupleExpr& node) override {
        out_ += "(TupleExpr";
        for (const ast::ExprPtr& element : node.elements()) {
            out_ += " ";
            element->accept(*this);
        }
        out_ += ")";
        append_suffix(node);
    }

    void visit(const ast::UnaryOp& node) override {
        out_ += "(UnaryOp " + op_spelling(node.op()) + " ";
        node.operand().accept(*this);
        out_ += ")";
        append_suffix(node);
    }

    void visit(const ast::While& node) override {
        out_ += "(While ";
        node.condition().accept(*this);
        print_body(node.body());
        print_else(node.orelse());
        out_ += ")";
    }

private:
    const TypeMap& types_;
    std::string out_;
    int depth_ = 0;

    void newline_indent() {
        out_ += "\n";
        out_.append(static_cast<std::size_t>(2 * depth_), ' ');
    }

    void print_body(const std::vector<ast::StmtPtr>& body) {
        ++depth_;
        for (const ast::StmtPtr& statement : body) {
            newline_indent();
            statement->accept(*this);
        }
        --depth_;
    }

    void print_else(const std::vector<ast::StmtPtr>& orelse) {
        if (orelse.empty()) {
            return;
        }
        ++depth_;
        newline_indent();
        out_ += "(Else";
        print_body(orelse);
        out_ += ")";
        --depth_;
    }

    // The one place the ":type" suffix is written. Appends nothing when
    // `types_` has no entry for `node` -- which is every annotation
    // subtree, and any expression the checker has not typed yet.
    void append_suffix(const ast::Expr& node) {
        if (const Type* type = types_.find(&node)) {
            out_ += ":" + type_name(*type);
        }
    }
};

} // namespace

std::string TypedPrinter::print(const ast::Module& module, const TypeMap& types) {
    return Renderer(types).render(module);
}

} // namespace cythonpp::domain::semantic
