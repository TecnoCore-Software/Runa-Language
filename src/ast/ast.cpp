#include "ast/ast.hpp"

#include "runtime/runtime.hpp"  // formatFloatDisplay (float literal dumps)

namespace runa::ast {

std::string CimportStatement::joinedPath() const {
    std::string out;
    for (std::size_t i = 0; i < pathSegments.size(); ++i) {
        if (i > 0) out += '/';
        out += pathSegments[i];
    }
    return out;
}

const char* binaryOpSymbol(BinaryOp op) {
    switch (op) {
        case BinaryOp::Add: return "+";
        case BinaryOp::Sub: return "-";
        case BinaryOp::Mul: return "*";
        case BinaryOp::Div: return "/";
        case BinaryOp::Mod: return "%";
        case BinaryOp::Eq:  return "==";
        case BinaryOp::Ne:  return "!=";
        case BinaryOp::Gt:  return ">";
        case BinaryOp::Lt:  return "<";
        case BinaryOp::Ge:  return ">=";
        case BinaryOp::Le:  return "<=";
        case BinaryOp::IdentEq: return "===";
        case BinaryOp::LogicalAnd: return "and";
        case BinaryOp::LogicalOr:  return "or";
    }
    return "?";
}

const char* unaryOpSymbol(UnaryOp op) {
    switch (op) {
        case UnaryOp::Negate: return "-";
        case UnaryOp::Not:    return "not";
    }
    return "?";
}

namespace {

// Small local helper for readable dumps.
std::string escapeForDump(const std::string& text) {
    std::string out;
    for (char c : text) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            case '\r': out += "\\r";  break;
            default:   out += c;
        }
    }
    return out;
}

// Compact source-like rendering used inside BinaryExpr descriptions, e.g.
//   (2 + (3 * 4))   (a + 1)   ((2 + 3) * 4)   add(10, (20 * 2))
std::string toSourceLike(const Expression& expression) {
    switch (expression.kind) {
        case NodeKind::StringLiteral:
            return "\"" + escapeForDump(static_cast<const StringLiteral&>(expression).value) + "\"";
        case NodeKind::NumberLiteral:
            return std::to_string(static_cast<const NumberLiteral&>(expression).value);
        case NodeKind::FloatLiteral:
            return runtime::formatFloatDisplay(
                static_cast<const FloatLiteral&>(expression).value);
        case NodeKind::BoolLiteral:
            return static_cast<const BoolLiteral&>(expression).value ? "true" : "false";
        case NodeKind::Identifier:
            return static_cast<const Identifier&>(expression).name;
        case NodeKind::FunctionCallExpr: {
            const auto& call = static_cast<const FunctionCallExpr&>(expression);
            std::string out = call.callee + "(";
            for (std::size_t i = 0; i < call.arguments.size(); ++i) {
                if (i > 0) out += ", ";
                out += toSourceLike(*call.arguments[i]);
            }
            return out + ")";
        }
        case NodeKind::BinaryExpr: {
            const auto& binary = static_cast<const BinaryExpr&>(expression);
            return "(" + toSourceLike(*binary.lhs) + " " + binaryOpSymbol(binary.op) +
                   " " + toSourceLike(*binary.rhs) + ")";
        }
        case NodeKind::UnaryExpr: {
            const auto& unary = static_cast<const UnaryExpr&>(expression);
            const std::string symbol = unaryOpSymbol(unary.op);
            const std::string operand = toSourceLike(*unary.operand);
            // '-' reads naturally tight; the keyword form keeps a space.
            if (unary.op == UnaryOp::Negate) return "(-" + operand + ")";
            return "(" + symbol + " " + operand + ")";
        }
        case NodeKind::NullLiteral:
            return "null";
        case NodeKind::SelfExpr:
            return "self";
        case NodeKind::MemberAccessExpr: {
            const auto& access = static_cast<const MemberAccessExpr&>(expression);
            return toSourceLike(*access.object) + "." + access.memberName;
        }
        case NodeKind::MethodCallExpr: {
            const auto& call = static_cast<const MethodCallExpr&>(expression);
            std::string out = toSourceLike(*call.object) + "." + call.methodName + "(";
            for (std::size_t i = 0; i < call.arguments.size(); ++i) {
                if (i > 0) out += ", ";
                out += toSourceLike(*call.arguments[i]);
            }
            return out + ")";
        }
        case NodeKind::NewExpr: {
            const auto& creation = static_cast<const NewExpr&>(expression);
            std::string out = "new " + creation.className + "(";
            for (std::size_t i = 0; i < creation.arguments.size(); ++i) {
                if (i > 0) out += ", ";
                out += toSourceLike(*creation.arguments[i]);
            }
            return out + ")";
        }
        case NodeKind::SuperCallExpr: {
            const auto& call = static_cast<const SuperCallExpr&>(expression);
            std::string out = "super." + call.methodName + "(";
            for (std::size_t i = 0; i < call.arguments.size(); ++i) {
                if (i > 0) out += ", ";
                out += toSourceLike(*call.arguments[i]);
            }
            return out + ")";
        }
        case NodeKind::IsExpr: {
            const auto& check = static_cast<const IsExpr&>(expression);
            return "(" + toSourceLike(*check.value) + " is " + check.className + ")";
        }
        case NodeKind::CollectionLiteral: {
            const auto& literal = static_cast<const CollectionLiteral&>(expression);
            std::string out = "{";
            for (std::size_t i = 0; i < literal.elements.size(); ++i) {
                if (i > 0) out += ", ";
                out += toSourceLike(*literal.elements[i]);
            }
            return out + "}";
        }
        default:
            return "<?expression>";
    }
}

// One-line description of any expression node, e.g.
//   StringLiteral "Hello"   NumberLiteral 123   BoolLiteral true   Identifier 'name'
//   BinaryExpr (2 + (3 * 4))   FunctionCallExpr add(10, 20)
std::string describeExpression(const Expression& expression) {
    switch (expression.kind) {
        case NodeKind::BinaryExpr:
            return "BinaryExpr " + toSourceLike(expression);
        case NodeKind::UnaryExpr:
            return "UnaryExpr " + toSourceLike(expression);
        case NodeKind::FunctionCallExpr:
            return "FunctionCallExpr " + toSourceLike(expression);
        case NodeKind::StringLiteral: {
            const auto& literal = static_cast<const StringLiteral&>(expression);
            return "StringLiteral \"" + escapeForDump(literal.value) + "\"";
        }
        case NodeKind::NumberLiteral: {
            const auto& literal = static_cast<const NumberLiteral&>(expression);
            return "NumberLiteral " + std::to_string(literal.value);
        }
        case NodeKind::FloatLiteral: {
            const auto& literal = static_cast<const FloatLiteral&>(expression);
            return "FloatLiteral " + runtime::formatFloatDisplay(literal.value);
        }
        case NodeKind::BoolLiteral: {
            const auto& literal = static_cast<const BoolLiteral&>(expression);
            return std::string("BoolLiteral ") + (literal.value ? "true" : "false");
        }
        case NodeKind::Identifier: {
            const auto& identifier = static_cast<const Identifier&>(expression);
            return "Identifier '" + identifier.name + "'";
        }
        case NodeKind::NullLiteral:
            return "NullLiteral";
        case NodeKind::SelfExpr:
            return "SelfExpr";
        case NodeKind::MemberAccessExpr:
            return "MemberAccessExpr " + toSourceLike(expression);
        case NodeKind::MethodCallExpr:
            return "MethodCallExpr " + toSourceLike(expression);
        case NodeKind::NewExpr:
            return "NewExpr " + toSourceLike(expression);
        case NodeKind::SuperCallExpr:
            return "SuperCallExpr " + toSourceLike(expression);
        case NodeKind::IsExpr:
            return "IsExpr " + toSourceLike(expression);
        default:
            return "<unsupported expression>";
    }
}

}  // namespace

namespace {

// Forward declarations for the mutually recursive statement dump.
void dumpStatement(const Statement& statement, const std::string& prefix,
                   bool last, std::ostream& out);

// Prints a statement list under `prefix`, one box-drawing branch per item.
void dumpStatementList(const std::vector<std::unique_ptr<Statement>>& statements,
                       const std::string& prefix, std::ostream& out) {
    for (std::size_t i = 0; i < statements.size(); ++i) {
        dumpStatement(*statements[i], prefix, i + 1 == statements.size(), out);
    }
}

void dumpStatement(const Statement& statement, const std::string& prefix,
                   bool last, std::ostream& out) {
    const std::string branch =
        last ? "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 " : "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 ";
    const std::string childPrefix = prefix + (last ? "    " : "\xe2\x94\x82   ");

    if (statement.kind == NodeKind::AssignmentStatement) {
        const auto& assignment = static_cast<const AssignmentStatement&>(statement);
        out << prefix << branch << "AssignmentStatement\n";
        out << childPrefix << "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 target "
            << describeExpression(*assignment.target) << "\n";
        out << childPrefix << "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 value   "
            << describeExpression(*assignment.value) << '\n';
    } else if (statement.kind == NodeKind::ExpressionStatement) {
        const auto& expressionStatement = static_cast<const ExpressionStatement&>(statement);
        out << prefix << branch << "ExpressionStatement\n";
        out << childPrefix << "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 expression "
            << describeExpression(*expressionStatement.expression) << '\n';
    } else if (statement.kind == NodeKind::FunctionDeclaration) {
        const auto& function = static_cast<const FunctionDeclaration&>(statement);
        out << prefix << branch << "FunctionDeclaration '" << function.name << "'\n";
        if (function.parameters.empty()) {
            out << childPrefix << "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 params: (none)\n";
        } else {
            out << childPrefix << "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 params: ";
            for (std::size_t p = 0; p < function.parameters.size(); ++p) {
                if (p > 0) out << ", ";
                out << function.parameters[p].name;
            }
            out << '\n';
        }
        if (function.body.empty()) {
            out << childPrefix << "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 body (empty)\n";
        } else {
            out << childPrefix << "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 body\n";
            dumpStatementList(function.body, childPrefix + "    ", out);
        }
    } else if (statement.kind == NodeKind::ReturnStatement) {
        const auto& returnStatement = static_cast<const ReturnStatement&>(statement);
        if (returnStatement.value == nullptr) {
            out << prefix << branch << "ReturnStatement (no value)\n";
        } else {
            out << prefix << branch << "ReturnStatement\n";
            out << childPrefix << "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 value "
                << describeExpression(*returnStatement.value) << '\n';
        }
    } else if (statement.kind == NodeKind::IfStatement) {
        const auto& ifStatement = static_cast<const IfStatement&>(statement);
        out << prefix << branch << "IfStatement\n";
        for (std::size_t b = 0; b < ifStatement.branches.size(); ++b) {
            const auto& branchNode = ifStatement.branches[b];
            const bool branchLast = (b + 1 == ifStatement.branches.size());
            const std::string branchConnect =
                branchLast ? "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 "
                           : "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 ";
            const std::string branchPrefix =
                childPrefix + (branchLast ? "    " : "\xe2\x94\x82   ");
            // Branch header: 'if'/'elif' with its condition, or a bare 'else'.
            if (branchNode.condition != nullptr) {
                out << childPrefix << branchConnect
                    << (b == 0 ? "if condition " : "elif condition ")
                    << describeExpression(*branchNode.condition) << '\n';
            } else {
                out << childPrefix << branchConnect << "else\n";
            }
            // The branch body hangs below its header.
            if (branchNode.body.empty()) {
                out << branchPrefix << "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 body (empty)\n";
            } else {
                out << branchPrefix << "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 body\n";
                dumpStatementList(branchNode.body, branchPrefix + "    ", out);
            }
        }
    } else if (statement.kind == NodeKind::LoopStatement) {
        const auto& loopStatement = static_cast<const LoopStatement&>(statement);
        out << prefix << branch << "LoopStatement\n";
        out << childPrefix << "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 condition "
            << describeExpression(*loopStatement.condition) << "\n";
        if (loopStatement.body.empty()) {
            out << childPrefix << "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 body (empty)\n";
        } else {
            out << childPrefix << "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 body\n";
            dumpStatementList(loopStatement.body, childPrefix + "    ", out);
        }
    } else if (statement.kind == NodeKind::ForStatement) {
        const auto& forStatement = static_cast<const ForStatement&>(statement);
        out << prefix << branch << "ForStatement '" << forStatement.variableName << "'\n";
        out << childPrefix << "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 start "
            << describeExpression(*forStatement.start) << "\n";
        out << childPrefix << "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 end   "
            << describeExpression(*forStatement.end) << "\n";
        if (forStatement.body.empty()) {
            out << childPrefix << "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 body (empty)\n";
        } else {
            out << childPrefix << "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 body\n";
            dumpStatementList(forStatement.body, childPrefix + "    ", out);
        }
    } else if (statement.kind == NodeKind::BreakStatement) {
        out << prefix << branch << "BreakStatement\n";
    } else if (statement.kind == NodeKind::ContinueStatement) {
        out << prefix << branch << "ContinueStatement\n";
    } else if (statement.kind == NodeKind::ImportStatement) {
        const auto& importStatement = static_cast<const ImportStatement&>(statement);
        out << prefix << branch << "ImportStatement '" << importStatement.packageName
            << "'\n";
    } else if (statement.kind == NodeKind::SelectiveImportStatement) {
        const auto& selective = static_cast<const SelectiveImportStatement&>(statement);
        out << prefix << branch << "SelectiveImportStatement '" << selective.packageName
            << "'";
        if (!selective.symbols.empty()) {
            out << " {";
            for (std::size_t i = 0; i < selective.symbols.size(); ++i) {
                out << (i == 0 ? " " : ", ") << selective.symbols[i].name;
            }
            out << " }";
        }
        out << '\n';
    } else if (statement.kind == NodeKind::CimportStatement) {
        const auto& cimport = static_cast<const CimportStatement&>(statement);
        out << prefix << branch << "CimportStatement '" << cimport.joinedPath() << "'\n";
    } else if (statement.kind == NodeKind::ClassDeclaration) {
        const auto& declaration = static_cast<const ClassDeclaration&>(statement);
        std::string header = declaration.isInterface ? "InterfaceDeclaration '"
                                                     : "ClassDeclaration '";
        header += declaration.name + "'";
        if (declaration.isAbstract) header += " (abstract)";
        if (!declaration.parentName.empty()) {
            header += " (parent: " + declaration.parentName + ")";
        }
        if (!declaration.interfaces.empty()) {
            header += " implements ";
            for (std::size_t i = 0; i < declaration.interfaces.size(); ++i) {
                if (i > 0) header += ", ";
                header += declaration.interfaces[i];
            }
        }
        out << prefix << branch << header << '\n';
        if (declaration.members.empty()) {
            out << childPrefix << "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 members (none)\n";
        } else {
            out << childPrefix << "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 members\n";
            const std::string memberPrefix = childPrefix + "    ";
            for (std::size_t m = 0; m < declaration.members.size(); ++m) {
                const auto& member = declaration.members[m];
                const bool memberLast = (m + 1 == declaration.members.size());
                const std::string memberBranch =
                    memberLast ? "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 " : "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 ";
                std::string flags;
                if (member.isPublic) flags += "public ";
                if (member.isPrivate) flags += "private ";
                if (member.isStatic) flags += "static ";
                if (member.isAbstract) flags += "abstract ";
                if (member.isOverride) flags += "override ";
                if (member.kind == ClassMember::Kind::Field) {
                    out << memberPrefix << memberBranch << "field " << flags
                        << member.name << " = "
                        << (member.initializer ? describeExpression(*member.initializer)
                                               : "<?>")
                        << '\n';
                } else if (member.kind == ClassMember::Kind::Constructor) {
                    out << memberPrefix << memberBranch << "constructor (";
                    for (std::size_t p = 0; p < member.parameters.size(); ++p) {
                        if (p > 0) out << ", ";
                        out << member.parameters[p].name;
                    }
                    out << ")\n";
                } else if (member.kind == ClassMember::Kind::Method) {
                    out << memberPrefix << memberBranch << "method " << flags
                        << member.name << "(";
                    for (std::size_t p = 0; p < member.parameters.size(); ++p) {
                        if (p > 0) out << ", ";
                        out << member.parameters[p].name;
                    }
                    out << ")";
                    if (member.body.empty()) out << " (no body)";
                    out << '\n';
                } else {
                    out << memberPrefix << memberBranch << "nested class\n";
                }
            }
        }
    } else {
        out << prefix << branch << "<unknown statement>\n";
    }
}

}  // namespace

void dump(const Program& program, std::ostream& out) {
    out << "Program\n";
    if (program.statements.empty()) {
        out << "(empty program)\n";
        return;
    }
    dumpStatementList(program.statements, "", out);
}

}  // namespace runa::ast
