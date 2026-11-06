#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTMutationListener.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Lex/Lexer.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/TemplateDeduction.h"
#include "clang/UnifiedSymbolResolution/USRGeneration.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
using namespace clang;

struct SourceSpanRecord {
    std::string file;
    std::uint32_t begin = 0;
    std::uint32_t end = 0;

    bool operator==(SourceSpanRecord const&) const = default;
};

struct NamedPortLiteralRecord {
    SourceSpanRecord span;
    std::string name;
    bool event = false;
};

std::filesystem::path normalized_path(std::filesystem::path path)
{
    std::error_code error;
    auto normalized = std::filesystem::weakly_canonical(path, error);
    if (!error) return normalized;
    normalized = std::filesystem::absolute(path, error);
    return error ? path.lexically_normal() : normalized.lexically_normal();
}

bool path_is_within(
    std::filesystem::path const& child,
    std::filesystem::path const& parent)
{
    auto child_it = child.begin();
    for (auto parent_it = parent.begin(); parent_it != parent.end();
         ++parent_it, ++child_it) {
        if (child_it == child.end() || *child_it != *parent_it) return false;
    }
    return true;
}

std::string type_string(ASTContext const& context, QualType type)
{
    PrintingPolicy policy(context.getLangOpts());
    policy.SuppressScope = false;
    policy.SuppressTagKeyword = true;
    policy.FullyQualifiedName = true;
    auto result = type.getCanonicalType().getAsString(policy);
    if (result.starts_with("::")) result.erase(0, 2);
    return result;
}

bool has_concrete_template_arguments(CXXRecordDecl const* record)
{
    if (!record || !record->isDependentType()) return true;
    auto const* specialization = dyn_cast<ClassTemplateSpecializationDecl>(record);
    if (!specialization) return false;
    auto const arguments = specialization->getTemplateArgs().asArray();
    return std::none_of(arguments.begin(), arguments.end(), [](TemplateArgument const& argument) {
        return argument.isDependent();
    });
}

std::string declaration_usr(ASTContext& context, Decl const* declaration)
{
    if (!declaration) return {};
    llvm::SmallString<128> buffer;
    if (clang::index::generateUSRForDecl(declaration, buffer)) {
        auto id = context.getDiagnostics().getCustomDiagID(
            DiagnosticsEngine::Error,
            "cannot generate a Clang USR for graph-significant declaration '%0'");
        context.getDiagnostics().Report(declaration->getLocation(), id)
            << declaration->getDeclKindName();
        return {};
    }
    return buffer.str().str();
}

std::string declaration_usr_if_available(Decl const* declaration)
{
    if (!declaration) return {};
    llvm::SmallString<128> buffer;
    if (clang::index::generateUSRForDecl(declaration, buffer)) return {};
    return buffer.str().str();
}

std::string type_usr(ASTContext& context, QualType type)
{
    llvm::SmallString<128> buffer;
    if (clang::index::generateUSRForType(
            type.getCanonicalType(), context, buffer)) {
        return type_string(context, type);
    }
    return buffer.str().str();
}

std::string hex_u64(std::uint64_t value);

QualType transported_configuration_type(QualType type)
{
    return type.getNonReferenceType().getUnqualifiedType().getCanonicalType();
}

std::uint64_t fingerprint_hash(std::string_view value)
{
    std::uint64_t result = 14695981039346656037ull;
    for (auto const character : value) {
        result ^= static_cast<unsigned char>(character);
        result *= 1099511628211ull;
    }
    return result;
}

void append_type_definition_fingerprint(
    ASTContext& context,
    QualType type,
    std::set<Type const*>& visited,
    std::string& material)
{
    type = transported_configuration_type(type);
    auto const* raw = type.getTypePtrOrNull();
    if (!raw || !visited.insert(raw).second) return;
    material += type_usr(context, type);
    material += ':';

    // ODRHash walks every declaration currently attached to the record. That
    // is not a stable package boundary identity: implicit specializations of
    // a friend/template member appear only after a particular package happens
    // to use them. Two packages can therefore see the same Sample declaration
    // with different ODRHash values. Fingerprint the spelled definition
    // instead. This is both compiler-produced and invariant under unrelated
    // use-site instantiations; removing whitespace avoids formatting-only
    // churn.
    auto definition_text = [&](Decl const* declaration) {
        if (!declaration) return std::string{};
        bool invalid = false;
        auto const text = Lexer::getSourceText(
            CharSourceRange::getTokenRange(declaration->getSourceRange()),
            context.getSourceManager(), context.getLangOpts(), &invalid);
        if (invalid || text.empty()) return std::string{};
        std::string normalized;
        normalized.reserve(text.size());
        for (unsigned char character : text.bytes()) {
            if (!std::isspace(character))
                normalized.push_back(static_cast<char>(character));
        }
        return normalized;
    };
    if (auto const* record = type->getAsCXXRecordDecl()) {
        auto const* definition = record->getDefinition();
        material += definition_text(definition);
        if (definition) {
            for (auto const& base : definition->bases()) {
                append_type_definition_fingerprint(
                    context, base.getType(), visited, material);
            }
            for (auto const* field : definition->fields()) {
                append_type_definition_fingerprint(
                    context, field->getType(), visited, material);
            }
        }
    } else if (auto const* enumeration = type->getAs<EnumType>()) {
        if (auto const* definition = enumeration->getDecl()->getDefinition()) {
            material += definition_text(definition);
        }
    } else {
        material += type_string(context, type);
    }
    material += ';';

    if (auto const* pointer = type->getAs<PointerType>()) {
        append_type_definition_fingerprint(
            context, pointer->getPointeeType(), visited, material);
    } else if (auto const* reference = type->getAs<ReferenceType>()) {
        append_type_definition_fingerprint(
            context, reference->getPointeeType(), visited, material);
    } else if (auto const* array = context.getAsArrayType(type)) {
        append_type_definition_fingerprint(
            context, array->getElementType(), visited, material);
    } else if (auto const* function = type->getAs<FunctionProtoType>()) {
        append_type_definition_fingerprint(
            context, function->getReturnType(), visited, material);
        for (auto parameter : function->param_types()) {
            append_type_definition_fingerprint(
                context, parameter, visited, material);
        }
    }
}

std::string type_definition_fingerprint(ASTContext& context, QualType type)
{
    std::set<Type const*> visited;
    std::string material;
    append_type_definition_fingerprint(context, type, visited, material);
    return hex_u64(fingerprint_hash(material));
}

llvm::json::Object configuration_type_metadata(
    ASTContext& context,
    QualType type)
{
    type = transported_configuration_type(type);
    return llvm::json::Object{
        {"nominal_id", type_usr(context, type)},
        {"definition_fingerprint", type_definition_fingerprint(context, type)},
        {"display_name", type_string(context, type)},
    };
}

std::string mangled_global_name(ASTContext& context, VarDecl const* declaration)
{
    if (!declaration) return {};
    ASTNameGenerator names(context);
    return names.getName(declaration);
}

std::string mangled_function_name(
    ASTContext& context,
    FunctionDecl const* declaration)
{
    if (!declaration) return {};
    ASTNameGenerator names(context);
    return names.getName(declaration);
}

std::uint64_t node_code_key_hash(
    std::string_view value,
    std::uint64_t initial_hash)
{
    std::uint64_t hash = initial_hash;
    for (auto const ch : value) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string hex_u64(std::uint64_t value)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result(16, '0');
    for (std::size_t index = result.size(); index-- > 0; value >>= 4)
        result[index] = digits[value & 0xf];
    return result;
}

llvm::json::Object node_code_key(ASTContext const& context, QualType type)
{
    // This reproduces iv::details::make_node_code_key. The key is deliberately
    // not persistent metadata: it only joins this frontend's State record to
    // the compiler record emitted by the same module build.
    auto const spelling = type_string(context, type);
    return llvm::json::Object{
        {"low", hex_u64(node_code_key_hash(spelling, 14695981039346656037ull))},
        {"high", hex_u64(node_code_key_hash(
            spelling, 1099511628211ull ^ 0x9e3779b97f4a7c15ull))},
    };
}

std::string record_name(QualType type)
{
    type = type.getNonReferenceType().getUnqualifiedType();
    if (auto const* record = type->getAsCXXRecordDecl())
        return record->getNameAsString();
    return {};
}

enum class AnnotatableRefKind {
    none,
    node,
    sample_port,
    event_port,
    public_sample_input,
    public_event_input,
};

AnnotatableRefKind annotatable_ref_kind(QualType type)
{
    auto const name = record_name(type);
    if (name == "NodeRef" || name == "TypedNodeRef")
        return AnnotatableRefKind::node;
    if (name == "SamplePortRef"
        || name == "TypedSamplePortRef"
        || name == "TypedSamplePortChannelRef"
        || name == "TypedSamplePortTileRef"
        || name == "TypedSamplePortTileChannelRef")
        return AnnotatableRefKind::sample_port;
    if (name == "EventPortRef") return AnnotatableRefKind::event_port;
    if (name == "PublicSampleInputRef" || name == "TypedPublicSampleInputRef")
        return AnnotatableRefKind::public_sample_input;
    if (name == "PublicEventInputRef")
        return AnnotatableRefKind::public_event_input;
    return AnnotatableRefKind::none;
}

bool is_annotatable_ref(QualType type)
{
    return annotatable_ref_kind(type) != AnnotatableRefKind::none;
}

class SourceModel {
public:
    SourceModel(ASTContext& context, std::filesystem::path core_source_dir)
        : context_(context), source_manager_(context.getSourceManager()),
          core_source_dir_(normalized_path(std::move(core_source_dir)))
    {}

    bool is_user_source(SourceLocation location) const
    {
        if (location.isInvalid()) return false;
        auto const spelling = source_manager_.getSpellingLoc(location);
        if (spelling.isInvalid() || source_manager_.isInSystemHeader(spelling))
            return false;
        auto const file = source_manager_.getFilename(spelling);
        if (file.empty()) return false;
        if (core_source_dir_.empty()) return true;
        return !path_is_within(normalized_path(file.str()), core_source_dir_);
    }

    std::optional<SourceSpanRecord> source_span(SourceRange range) const
    {
        if (range.isInvalid()) return std::nullopt;
        auto const begin = source_manager_.getSpellingLoc(range.getBegin());
        auto const end = source_manager_.getSpellingLoc(range.getEnd());
        if (begin.isInvalid() || end.isInvalid()) return std::nullopt;
        if (source_manager_.getFileID(begin) != source_manager_.getFileID(end))
            return std::nullopt;
        if (!is_user_source(begin)) return std::nullopt;
        auto const file = source_manager_.getFilename(begin);
        if (file.empty()) return std::nullopt;
        auto const token_length = Lexer::MeasureTokenLength(
            end, source_manager_, context_.getLangOpts());
        auto const begin_offset = source_manager_.getFileOffset(begin);
        auto const end_offset = source_manager_.getFileOffset(end) + token_length;
        if (end_offset < begin_offset
            || end_offset > std::numeric_limits<std::uint32_t>::max())
            return std::nullopt;
        return SourceSpanRecord{
            .file = normalized_path(file.str()).generic_string(),
            .begin = static_cast<std::uint32_t>(begin_offset),
            .end = static_cast<std::uint32_t>(end_offset),
        };
    }

    std::optional<SourceSpanRecord> declaration_span(VarDecl const* declaration) const
    {
        if (!declaration || declaration->getLocation().isInvalid()
            || declaration->getName().empty()
            || !is_user_source(declaration->getLocation()))
            return std::nullopt;
        auto const location = source_manager_.getSpellingLoc(declaration->getLocation());
        auto const file = source_manager_.getFilename(location);
        if (file.empty()) return std::nullopt;
        auto const begin = source_manager_.getFileOffset(location);
        auto const length = Lexer::MeasureTokenLength(
            location, source_manager_, context_.getLangOpts());
        if (length == 0 || begin + length > std::numeric_limits<std::uint32_t>::max())
            return std::nullopt;
        return SourceSpanRecord{
            .file = normalized_path(file.str()).generic_string(),
            .begin = static_cast<std::uint32_t>(begin),
            .end = static_cast<std::uint32_t>(begin + length),
        };
    }

    std::optional<NamedPortLiteralRecord> named_port_literal(
        UserDefinedLiteral const* literal) const
    {
        if (!literal) return std::nullopt;
        auto const begin = source_manager_.getSpellingLoc(literal->getBeginLoc());
        auto const end = source_manager_.getSpellingLoc(literal->getEndLoc());
        if (begin.isInvalid() || end.isInvalid()) return std::nullopt;
        auto token_text = Lexer::getSourceText(
            CharSourceRange::getTokenRange(begin, end),
            source_manager_, context_.getLangOpts());
        if (token_text.empty()) return std::nullopt;

        std::string_view suffix;
        bool event = false;
        if (token_text.ends_with("_P"))
            suffix = "_P";
        else if (token_text.ends_with("_F")) {
            suffix = "_F";
            event = true;
        } else
            return std::nullopt;

        auto span = source_span(literal->getSourceRange());
        if (!span || span->end < span->begin + suffix.size())
            return std::nullopt;
        // Clang exposes a string UDL as one source token (for example
        // `"frequency"_P`).  Tooling provenance should cover the name
        // string only, not the literal-operator suffix.
        span->end -= static_cast<std::uint32_t>(suffix.size());
        token_text = token_text.drop_back(suffix.size());
        auto const first_quote = token_text.find('"');
        auto const last_quote = token_text.rfind('"');
        if (first_quote == llvm::StringRef::npos || last_quote == llvm::StringRef::npos
            || first_quote >= last_quote)
            return std::nullopt;

        // Port names are ordinary narrow string literal NTTPs. Preserve the
        // semantic spelling used by the DSL for ordinary names; escaped names
        // are intentionally uncommon and can be handled here if the DSL ever
        // starts relying on them.
        auto port_name = token_text.slice(first_quote + 1, last_quote).str();
        return NamedPortLiteralRecord{
            .span = *span,
            .name = std::move(port_name),
            .event = event,
        };
    }

private:
    ASTContext& context_;
    SourceManager& source_manager_;
    std::filesystem::path core_source_dir_;
};

struct RefAnnotation {
    VarDecl* declaration = nullptr;
    std::string declaration_identity;
    SourceSpanRecord span;
    bool refers_to_enclosing_variable_or_capture = false;
    Expr* node_builder = nullptr;
};

struct NodeInputPortAnnotation {
    VarDecl* declaration = nullptr;
    std::string declaration_identity;
    std::string port_name;
    bool event = false;
    SourceSpanRecord span;
    bool refers_to_enclosing_variable_or_capture = false;
};

struct PublicOutputAnnotation {
    Expr* builder = nullptr;
    bool event = false;
    std::size_t index = 0;
    SourceSpanRecord span;
};

struct StatementAnalysis {
    std::vector<RefAnnotation> refs;
    std::vector<NodeInputPortAnnotation> node_input_ports;
    std::vector<PublicOutputAnnotation> public_outputs;
};

class StatementAnalyzer final : public RecursiveASTVisitor<StatementAnalyzer> {
public:
    StatementAnalyzer(
        ASTContext& context,
        SourceModel const& sources,
        StatementAnalysis& result,
        std::unordered_map<FunctionDecl const*,
            std::unordered_map<std::string, SourceSpanRecord>>& graph_locals)
        : context_(context), sources_(sources), result_(result),
          graph_locals_(graph_locals)
    {}

    // Nested statement bodies are transformed independently. This mirrors the
    // old GCC plugin's statement-list walk and prevents duplicate annotations.
    bool TraverseCompoundStmt(CompoundStmt*) { return true; }
    bool TraverseLambdaExpr(LambdaExpr*) { return true; }

    bool VisitDeclStmt(DeclStmt* statement)
    {
        if (!statement) return true;
        for (auto* declaration : statement->decls()) {
            auto* variable = dyn_cast<VarDecl>(declaration);
            if (!variable || variable->isImplicit()
                || !is_annotatable_ref(variable->getType()))
                continue;

            auto const span = sources_.declaration_span(variable);
            auto const identity = declaration_identity(variable, span);
            if (!span || identity.empty()) continue;
            validate_unique_graph_local(variable, *span);

            // A class-type declaration without source syntax for an
            // initializer (for example `NodeRef x;`) has an implicit
            // CXXConstructExpr in Clang's AST. It is still uninitialized in
            // the configured-graph sense and must wait for its first assignment.
            if (!has_explicit_initializer(variable)) continue;

            // Source provenance is intentionally token-shaped: the declared
            // identifier is active, but the initializer expression is not.
            append_ref({
                .declaration = variable,
                .declaration_identity = identity,
                .span = *span,
                .node_builder = node_call_builder(variable->getInit()),
            });
            append_named_port_argument_spans(
                variable, identity, variable->getInit(), false);
        }
        return true;
    }

    bool VisitDeclRefExpr(DeclRefExpr* reference)
    {
        if (!reference || !sources_.is_user_source(reference->getExprLoc()))
            return true;
        auto* variable = dyn_cast<VarDecl>(reference->getDecl());
        if (!variable || variable->isImplicit()
            || !is_annotatable_ref(variable->getType()))
            return true;
        auto const span = sources_.source_span(reference->getSourceRange());
        auto const identity = declaration_identity(
            variable, sources_.declaration_span(variable));
        if (span && !identity.empty()) append_ref({
            variable,
            identity,
            *span,
            reference->refersToEnclosingVariableOrCapture(),
        });
        return true;
    }

    bool VisitCXXOperatorCallExpr(CXXOperatorCallExpr* call)
    {
        if (!call || call->getNumArgs() == 0) return true;

        if (call->getOperator() == OO_Equal) {
            auto* variable = referenced_variable(call->getArg(0));
            if (variable && !variable->isImplicit()
                && annotatable_ref_kind(variable->getType()) == AnnotatableRefKind::node
                && !variable->getType()->isReferenceType()
                && !has_explicit_initializer(variable)) {
                auto const declaration_source = sources_.declaration_span(variable);
                auto const identity = declaration_identity(
                    variable, declaration_source);
                if (!identity.empty() && declaration_source) {
                    validate_unique_graph_local(variable, *declaration_source);
                    // The declaration could not be annotated while the ref was
                    // empty. Once the assignment has completed, attach its
                    // declaration identifier as provenance too. The LHS use is
                    // picked up independently by VisitDeclRefExpr.
                    append_ref({
                        .declaration = variable,
                        .declaration_identity = identity,
                        .span = *declaration_source,
                        .node_builder = call->getNumArgs() > 1
                            ? node_call_builder(call->getArg(1))
                            : nullptr,
                    });
                }
                if (!identity.empty() && call->getNumArgs() > 1) {
                    append_named_port_argument_spans(
                        variable, identity, call->getArg(1), false);
                }
            }
        }

        if (call->getOperator() == OO_Call) {
            auto* variable = referenced_variable(call->getArg(0));
            if (variable
                && annotatable_ref_kind(variable->getType()) == AnnotatableRefKind::node) {
                auto const identity = declaration_identity(
                    variable, sources_.declaration_span(variable));
                if (!identity.empty()) {
                    auto const captured = expression_refers_to_capture(call->getArg(0));
                    for (unsigned i = 1; i < call->getNumArgs(); ++i) {
                        append_named_port_binding_annotation(
                            variable, identity, call->getArg(i), captured);
                    }
                }
            }
        }
        return true;
    }

    bool VisitCXXMemberCallExpr(CXXMemberCallExpr* call)
    {
        if (!call) return true;
        auto* method = call->getMethodDecl();
        if (!method) return true;
        auto const method_name = method->getNameAsString();

        if ((method_name == "outputs" || method_name == "event_outputs")
            && method->getParent()->getName() == "GraphBuilder"
            && sources_.is_user_source(call->getExprLoc())) {
            auto* builder = call->getImplicitObjectArgument();
            for (unsigned i = 0; i < call->getNumArgs(); ++i) {
                auto const binding = named_port_binding(call->getArg(i));
                if (!binding) continue;
                result_.public_outputs.push_back({
                    .builder = builder,
                    .event = method_name == "event_outputs",
                    .index = i,
                    .span = binding->span,
                });
            }
        }

        return true;
    }

private:
    ASTContext& context_;
    SourceModel const& sources_;
    StatementAnalysis& result_;
    std::unordered_map<FunctionDecl const*,
        std::unordered_map<std::string, SourceSpanRecord>>& graph_locals_;

    static VarDecl* referenced_variable(Expr* expression)
    {
        if (!expression) return nullptr;
        expression = expression->IgnoreParenImpCasts();
        if (auto* reference = dyn_cast<DeclRefExpr>(expression))
            return dyn_cast<VarDecl>(reference->getDecl());
        if (auto* unary = dyn_cast<UnaryOperator>(expression))
            return referenced_variable(unary->getSubExpr());
        return nullptr;
    }

    static bool has_explicit_initializer(VarDecl const* variable)
    {
        if (!variable || !variable->hasInit()) return false;
        auto const* initializer = variable->getInit();
        auto const* construction = dyn_cast<CXXConstructExpr>(initializer);
        if (!construction) return true;
        if (construction->getParenOrBraceRange().isValid()) return true;
        // A copy-initializer begins at its right-hand expression. An implicit
        // default constructor begins at the variable name.
        return construction->getBeginLoc() != variable->getLocation();
    }

    std::string declaration_identity(
        VarDecl const* declaration,
        std::optional<SourceSpanRecord> const& span) const
    {
        if (!declaration || !span) return declaration_usr_if_available(declaration);
        auto const* function = enclosing_function(declaration);
        if (!function || !function->isTemplateInstantiation()) {
            auto result = declaration_usr_if_available(declaration);
            if (!result.empty()) return result;
        }

        // Clang can assign a USR to a local in a generic-lambda
        // instantiation, but that USR embeds concrete template arguments
        // (for example the polyphonic voice index). Source annotations need
        // the lexical declaration identity instead so equivalent callback
        // instances aggregate by their exact configured source span. This is
        // only a source annotation key; NodeCodeKey remains the build-local
        // compiler-record join key.
        return "source-local:" + span->file + ':' + std::to_string(span->begin)
            + '@' + declaration->getNameAsString();
    }

    static Expr* peel_expression(Expr* expression)
    {
        if (!expression) return nullptr;
        for (;;) {
            auto* next = expression->IgnoreParenImpCasts();
            if (auto* cleanups = dyn_cast<ExprWithCleanups>(next))
                next = cleanups->getSubExpr();
            else if (auto* materialized = dyn_cast<MaterializeTemporaryExpr>(next))
                next = materialized->getSubExpr();
            else if (auto* bound = dyn_cast<CXXBindTemporaryExpr>(next))
                next = bound->getSubExpr();
            if (next == expression) return expression;
            expression = next;
        }
    }

    static Expr* node_call_builder(Expr* expression)
    {
        expression = peel_expression(expression);
        auto* call = dyn_cast_or_null<CXXMemberCallExpr>(expression);
        if (!call) return nullptr;
        auto const* method = call->getMethodDecl();
        if (!method || method->getNameAsString() != "node"
            || method->getParent()->getQualifiedNameAsString()
                != "iv::GraphBuilder") {
            return nullptr;
        }
        return call->getImplicitObjectArgument();
    }

    static bool expression_refers_to_capture(Expr* expression)
    {
        if (!expression) return false;
        expression = expression->IgnoreParenImpCasts();
        auto const* reference = dyn_cast<DeclRefExpr>(expression);
        return reference && reference->refersToEnclosingVariableOrCapture();
    }

    UserDefinedLiteral* named_port_literal_expr(Expr* expression) const
    {
        expression = peel_expression(expression);
        if (!expression) return nullptr;
        if (auto* literal = dyn_cast<UserDefinedLiteral>(expression)) {
            return sources_.named_port_literal(literal) ? literal : nullptr;
        }
        class Finder final : public RecursiveASTVisitor<Finder> {
        public:
            explicit Finder(SourceModel const& sources) : sources_(sources) {}
            bool VisitUserDefinedLiteral(UserDefinedLiteral* literal)
            {
                if (!found_ && sources_.named_port_literal(literal))
                    found_ = literal;
                return true;
            }
            UserDefinedLiteral* found() const { return found_; }
        private:
            SourceModel const& sources_;
            UserDefinedLiteral* found_ = nullptr;
        } finder(sources_);
        finder.TraverseStmt(expression);
        return finder.found();
    }

    std::optional<NamedPortLiteralRecord> named_port_binding(Expr* expression) const
    {
        expression = peel_expression(expression);
        auto* assignment = dyn_cast_or_null<CXXOperatorCallExpr>(expression);
        if (!assignment || assignment->getOperator() != OO_Equal
            || assignment->getNumArgs() < 2)
            return std::nullopt;
        auto* literal = named_port_literal_expr(assignment->getArg(0));
        return literal ? sources_.named_port_literal(literal) : std::nullopt;
    }

    void append_named_port_binding_annotation(
        VarDecl* variable,
        std::string const& identity,
        Expr* expression,
        bool captured)
    {
        if (auto const binding = named_port_binding(expression)) {
            append_node_input_port({
                .declaration = variable,
                .declaration_identity = identity,
                .port_name = binding->name,
                .event = binding->event,
                .span = binding->span,
                .refers_to_enclosing_variable_or_capture = captured,
            });
        }
    }

    void append_named_port_argument_spans(
        VarDecl* variable,
        std::string const& identity,
        Expr* expression,
        bool captured)
    {
        expression = peel_expression(expression);
        auto* call = dyn_cast_or_null<CallExpr>(expression);
        if (!call) return;
        unsigned begin = 0;
        if (auto* operator_call = dyn_cast<CXXOperatorCallExpr>(call)) {
            if (operator_call->getOperator() == OO_Call) begin = 1;
        }
        for (unsigned i = begin; i < call->getNumArgs(); ++i)
            append_named_port_binding_annotation(
                variable, identity, call->getArg(i), captured);
    }

    void append_ref(RefAnnotation annotation)
    {
        auto const duplicate = std::find_if(
            result_.refs.begin(), result_.refs.end(),
            [&](auto const& existing) {
                return existing.declaration == annotation.declaration
                    && existing.span == annotation.span;
            });
        if (duplicate == result_.refs.end())
            result_.refs.push_back(std::move(annotation));
    }

    void append_node_input_port(NodeInputPortAnnotation annotation)
    {
        auto const duplicate = std::find_if(
            result_.node_input_ports.begin(), result_.node_input_ports.end(),
            [&](auto const& existing) {
                return existing.declaration == annotation.declaration
                    && existing.port_name == annotation.port_name
                    && existing.event == annotation.event
                    && existing.span == annotation.span;
            });
        if (duplicate == result_.node_input_ports.end())
            result_.node_input_ports.push_back(std::move(annotation));
    }

    static FunctionDecl const* enclosing_function(VarDecl const* declaration)
    {
        if (!declaration) return nullptr;
        auto const* context = declaration->getDeclContext();
        while (context) {
            auto const* as_decl = Decl::castFromDeclContext(context);
            if (auto const* function = dyn_cast<FunctionDecl>(as_decl))
                return function;
            context = context->getParent();
        }
        return nullptr;
    }

    void validate_unique_graph_local(
        VarDecl* declaration,
        SourceSpanRecord const& span)
    {
        if (!declaration || declaration->getType()->isReferenceType()) return;
        auto const* function = enclosing_function(declaration);
        if (!function) return;
        auto& locals = graph_locals_[function];
        auto const [found, inserted] = locals.emplace(
            declaration->getNameAsString(), span);
        if (inserted || found->second == span) return;
        auto id = context_.getDiagnostics().getCustomDiagID(
            DiagnosticsEngine::Error,
            "graph-significant local '%0' is already declared in this function; use a unique local name");
        context_.getDiagnostics().Report(declaration->getLocation(), id)
            << declaration->getNameAsString();
    }

};

class HelperFinder final : public RecursiveASTVisitor<HelperFinder> {
public:
    bool VisitFunctionTemplateDecl(FunctionTemplateDecl* declaration)
    {
        if (declaration && declaration->getNameAsString() == "_annotate_source_info_after_statement")
            source_annotation = declaration;
        return true;
    }

    bool VisitFunctionDecl(FunctionDecl* declaration)
    {
        if (!declaration) return true;
        auto const name = declaration->getNameAsString();
        if (name == "_prepare_node_source_identity_before_statement")
            node_source_preparation = declaration;
        else if (name == "_annotate_public_output_after_statement")
            public_output_annotation = declaration;
        else if (name == "_annotate_node_input_source_info_after_statement")
            node_input_annotation = declaration;
        return true;
    }

    FunctionTemplateDecl* source_annotation = nullptr;
    FunctionDecl* node_source_preparation = nullptr;
    FunctionDecl* node_input_annotation = nullptr;
    FunctionDecl* public_output_annotation = nullptr;
};

class FunctionInstrumenter {
public:
    FunctionInstrumenter(
        CompilerInstance& compiler,
        SourceModel const& sources,
        bool source_annotations)
        : compiler_(compiler), context_(compiler.getASTContext()),
          sources_(sources), source_annotations_(source_annotations)
    {}

    void instrument(FunctionDecl* function)
    {
        if (!function || !function->doesThisDeclarationHaveABody()
            || !function->getBody() || instrumented_.contains(function)
            || !sources_.is_user_source(function->getLocation()))
            return;
        ensure_helpers();
        if (!node_source_preparation_function_
            || (source_annotations_
                && (!source_annotation_template_
                    || !node_input_annotation_function_
                    || !public_output_annotation_function_))) {
            auto id = compiler_.getDiagnostics().getCustomDiagID(
                DiagnosticsEngine::Error,
                "IV package annotation helpers were not found; include <intravenous/dsl.h> before configured module code");
            compiler_.getDiagnostics().Report(function->getLocation(), id);
            return;
        }
        instrumented_.insert(function);
        // ASTConsumer callbacks run after the parser has restored Sema's
        // translation-unit context.  Re-enter the owning function while
        // generating the annotations so Sema performs ordinary local-name
        // lookup and builds a well-formed call expression for CodeGen.
        Sema::ContextRAII function_context(sema(), function);
        function->setBody(transform_statement(function->getBody()));
    }

private:
    CompilerInstance& compiler_;
    ASTContext& context_;
    SourceModel const& sources_;
    bool source_annotations_ = true;
    llvm::DenseSet<FunctionDecl*> instrumented_;
    FunctionTemplateDecl* source_annotation_template_ = nullptr;
    FunctionDecl* node_source_preparation_function_ = nullptr;
    FunctionDecl* node_input_annotation_function_ = nullptr;
    FunctionDecl* public_output_annotation_function_ = nullptr;
    bool helpers_searched_ = false;
    std::unordered_map<FunctionDecl const*,
        std::unordered_map<std::string, SourceSpanRecord>> graph_locals_;

    Sema& sema() { return compiler_.getSema(); }

    void ensure_helpers()
    {
        if (helpers_searched_) return;
        helpers_searched_ = true;
        HelperFinder finder;
        finder.TraverseDecl(context_.getTranslationUnitDecl());
        source_annotation_template_ = finder.source_annotation;
        node_source_preparation_function_ = finder.node_source_preparation;
        node_input_annotation_function_ = finder.node_input_annotation;
        public_output_annotation_function_ = finder.public_output_annotation;
    }

    Stmt* transform_statement(Stmt* statement)
    {
        if (!statement) return nullptr;
        if (auto* lambda = dyn_cast<LambdaExpr>(statement)) {
            instrument(lambda->getCallOperator());
            return statement;
        }
        if (auto* compound = dyn_cast<CompoundStmt>(statement))
            return transform_compound(compound);

        for (auto it = statement->child_begin(); it != statement->child_end(); ++it) {
            if (*it) *it = transform_statement(*it);
        }
        return statement;
    }

    CompoundStmt* transform_compound(CompoundStmt* compound)
    {
        llvm::SmallVector<Stmt*, 32> body;
        body.reserve(compound->size());
        for (auto* original : compound->body()) {
            auto* statement = transform_statement(original);

            StatementAnalysis analysis;
            StatementAnalyzer analyzer(context_, sources_, analysis, graph_locals_);
            analyzer.TraverseStmt(statement);
            for (auto const& ref : analysis.refs) {
                if (ref.node_builder) {
                    if (auto* preparation = build_node_source_preparation(ref))
                        body.push_back(preparation);
                }
            }
            body.push_back(statement);
            if (source_annotations_) {
                for (auto const& ref : analysis.refs) {
                    if (auto* annotation = build_ref_annotation(ref))
                        body.push_back(annotation);
                }
                for (auto const& port : analysis.node_input_ports) {
                    if (auto* annotation = build_node_input_annotation(port))
                        body.push_back(annotation);
                }
                for (auto const& output : analysis.public_outputs) {
                    if (auto* annotation = build_public_output_annotation(output))
                        body.push_back(annotation);
                }
            }
        }
        return CompoundStmt::Create(
            context_, body, compound->getStoredFPFeaturesOrDefault(),
            compound->getLBracLoc(), compound->getRBracLoc());
    }

    Expr* make_decl_ref(ValueDecl* declaration, SourceLocation location)
    {
        if (!declaration) return nullptr;
        return sema().BuildDeclRefExpr(
            declaration,
            declaration->getType().getNonReferenceType(),
            VK_LValue,
            location);
    }

    Expr* make_variable_ref(
        VarDecl* declaration,
        SourceLocation location,
        bool refers_to_enclosing_variable_or_capture)
    {
        if (!declaration) return nullptr;
        return DeclRefExpr::Create(
            context_, NestedNameSpecifierLoc{}, SourceLocation{}, declaration,
            refers_to_enclosing_variable_or_capture,
            location,
            declaration->getType().getNonReferenceType(),
            VK_LValue);
    }

    Expr* make_address(Expr* expression, SourceLocation location)
    {
        if (!expression) return nullptr;
        auto result = sema().BuildUnaryOp(nullptr, location, UO_AddrOf, expression);
        return result.isInvalid() ? nullptr : result.get();
    }

    StringLiteral* make_string(std::string const& value, SourceLocation location)
    {
        auto type = context_.getStringLiteralArrayType(
            context_.CharTy, static_cast<unsigned>(value.size() + 1));
        return StringLiteral::Create(
            context_, value, StringLiteralKind::Ordinary, false, type, location);
    }

    IntegerLiteral* make_u32(std::uint32_t value, SourceLocation location)
    {
        return IntegerLiteral::Create(
            context_, llvm::APInt(32, value), context_.UnsignedIntTy, location);
    }

    IntegerLiteral* make_size(std::size_t value, SourceLocation location)
    {
        auto const bits = context_.getTypeSize(context_.getSizeType());
        return IntegerLiteral::Create(
            context_, llvm::APInt(bits, value), context_.getSizeType(), location);
    }

    FunctionDecl* source_annotation_specialization(
        QualType ref_type,
        SourceLocation location)
    {
        TemplateArgumentListInfo explicit_arguments(location, location);
        explicit_arguments.addArgument(sema().getTrivialTemplateArgumentLoc(
            TemplateArgument(ref_type), QualType{}, location));
        sema::TemplateDeductionInfo info(location);
        FunctionDecl* specialization = nullptr;
        auto result = sema().DeduceTemplateArguments(
            source_annotation_template_, &explicit_arguments,
            specialization, info, false);
        if (result != TemplateDeductionResult::Success || !specialization) {
            auto id = compiler_.getDiagnostics().getCustomDiagID(
                DiagnosticsEngine::Error,
                "cannot instantiate IV package annotation helper for type '%0'");
            compiler_.getDiagnostics().Report(location, id)
                << type_string(context_, ref_type);
            return nullptr;
        }
        return specialization;
    }

    Stmt* build_ref_annotation(RefAnnotation const& annotation)
    {
        auto const location = annotation.declaration->getLocation();
        auto* reference = make_variable_ref(
            annotation.declaration,
            location,
            annotation.refers_to_enclosing_variable_or_capture);
        auto* pointer = make_address(reference, location);
        if (!pointer) return nullptr;
        auto* specialization = source_annotation_specialization(
            annotation.declaration->getType().getNonReferenceType(), location);
        if (!specialization) return nullptr;
        auto* callee = make_decl_ref(specialization, location);
        if (!callee) return nullptr;
        llvm::SmallVector<Expr*, 5> arguments{
            pointer,
            make_string(annotation.declaration_identity, location),
            make_string(annotation.span.file, location),
            make_u32(annotation.span.begin, location),
            make_u32(annotation.span.end, location),
        };
        auto result = sema().BuildCallExpr(
            nullptr, callee, location, arguments, location);
        return result.isInvalid() ? nullptr : result.get();
    }

    Stmt* build_node_source_preparation(RefAnnotation const& annotation)
    {
        auto const location = annotation.declaration->getLocation();
        auto* pointer = builder_pointer(annotation.node_builder, location);
        if (!pointer) return nullptr;
        auto* callee = make_decl_ref(
            node_source_preparation_function_, location);
        if (!callee) return nullptr;
        llvm::SmallVector<Expr*, 3> arguments{
            pointer,
            make_string(annotation.declaration_identity, location),
            make_string(
                is_direct_module_build_graph_local(annotation.declaration)
                    ? annotation.declaration->getNameAsString()
                    : std::string{},
                location),
        };
        auto result = sema().BuildCallExpr(
            nullptr, callee, location, arguments, location);
        return result.isInvalid() ? nullptr : result.get();
    }

    static bool is_direct_module_build_graph_local(
        VarDecl const* declaration)
    {
        if (!declaration) return false;
        auto const* context = declaration->getDeclContext();
        while (context
            && !isa<FunctionDecl>(Decl::castFromDeclContext(context))) {
            context = context->getParent();
        }
        auto const* method = context
            ? dyn_cast<CXXMethodDecl>(Decl::castFromDeclContext(context))
            : nullptr;
        if (!method || method->getNameAsString() != "build_graph"
            || !method->isConst() || method->getNumParams() != 1) {
            return false;
        }
        auto const* builder_reference = method->getParamDecl(0)
            ->getType()->getAs<LValueReferenceType>();
        auto const* builder_record = builder_reference
            ? builder_reference->getPointeeType()->getAsCXXRecordDecl()
            : nullptr;
        return builder_record
            && builder_record->getQualifiedNameAsString() == "iv::GraphBuilder";
    }

    Stmt* build_node_input_annotation(NodeInputPortAnnotation const& annotation)
    {
        auto const location = annotation.declaration->getLocation();
        auto* reference = make_variable_ref(
            annotation.declaration,
            location,
            annotation.refers_to_enclosing_variable_or_capture);
        auto* pointer = make_address(reference, location);
        if (!pointer) return nullptr;
        auto* callee = make_decl_ref(node_input_annotation_function_, location);
        if (!callee) return nullptr;
        llvm::SmallVector<Expr*, 8> arguments{
            pointer,
            new (context_) CXXBoolLiteralExpr(
                annotation.event, context_.BoolTy, location),
            make_string(annotation.port_name, location),
            make_string(annotation.declaration_identity, location),
            make_string(annotation.span.file, location),
            make_u32(annotation.span.begin, location),
            make_u32(annotation.span.end, location),
        };
        auto result = sema().BuildCallExpr(
            nullptr, callee, location, arguments, location);
        return result.isInvalid() ? nullptr : result.get();
    }

    Expr* builder_pointer(Expr* builder, SourceLocation location)
    {
        if (!builder) return nullptr;
        builder = builder->IgnoreParenImpCasts();
        if (auto* ref = dyn_cast<DeclRefExpr>(builder)) {
            // The annotation is a separate statement, so it needs its own
            // expression tree. Preserve Clang's capture bit when cloning a
            // reference from a lambda body: a plain BuildDeclRefExpr here
            // would refer to the enclosing GraphBuilder directly instead of
            // the already-declared closure capture, and the output span would
            // silently disappear from the configured graph.
            auto* fresh = DeclRefExpr::Create(
                context_, NestedNameSpecifierLoc{}, SourceLocation{},
                ref->getDecl(), ref->refersToEnclosingVariableOrCapture(),
                location, ref->getType(), ref->getValueKind());
            return make_address(fresh, location);
        }
        // GraphBuilder's public API is unchanged; ordinary configured code uses
        // a builder lvalue. Do not re-evaluate arbitrary object expressions
        // after the statement merely to obtain an annotation target.
        auto id = compiler_.getDiagnostics().getCustomDiagID(
            DiagnosticsEngine::Error,
            "IV public output source annotation requires a GraphBuilder lvalue");
        compiler_.getDiagnostics().Report(location, id);
        return nullptr;
    }

    Stmt* build_public_output_annotation(PublicOutputAnnotation const& output)
    {
        auto const location = output.builder
            ? output.builder->getExprLoc()
            : SourceLocation{};
        auto* pointer = builder_pointer(output.builder, location);
        if (!pointer) return nullptr;
        auto* callee = make_decl_ref(public_output_annotation_function_, location);
        if (!callee) return nullptr;
        llvm::SmallVector<Expr*, 6> arguments{
            pointer,
            new (context_) CXXBoolLiteralExpr(output.event, context_.BoolTy, location),
            make_size(output.index, location),
            make_string(output.span.file, location),
            make_u32(output.span.begin, location),
            make_u32(output.span.end, location),
        };
        auto result = sema().BuildCallExpr(
            nullptr, callee, location, arguments, location);
        return result.isInvalid() ? nullptr : result.get();
    }
};

std::optional<QualType> direct_node_nested_type(
    ASTContext& context,
    CXXRecordDecl const* node,
    StringRef name)
{
    if (!node) return std::nullopt;
    for (auto const* declaration : node->decls()) {
        auto const* named = dyn_cast<NamedDecl>(declaration);
        if (!named || named->getName() != name) continue;
        if (auto const* record = dyn_cast<CXXRecordDecl>(declaration)) {
            return context.getCanonicalTagType(record);
        }
        if (auto const* alias = dyn_cast<TypedefNameDecl>(declaration)) {
            return context.getCanonicalType(alias->getUnderlyingType());
        }
    }
    return std::nullopt;
}

std::optional<QualType> node_nested_type(
    ASTContext& context,
    CXXRecordDecl const* node,
    StringRef name,
    std::set<CXXRecordDecl const*>& visited)
{
    if (!node || !visited.insert(node).second) return std::nullopt;
    if (auto type = direct_node_nested_type(context, node, name)) return type;
    // `typename Node::State` / `typename Node::TockState` use normal
    // base-class lookup too. Follow that lookup so inherited aliases and
    // nested records are keyed by the derived node that actually uses them.
    for (auto const& base : node->bases()) {
        auto const* base_record = base.getType()->getAsCXXRecordDecl();
        if (auto type = node_nested_type(context, base_record, name, visited))
            return type;
    }
    return std::nullopt;
}

std::optional<QualType> node_nested_type(
    ASTContext& context,
    CXXRecordDecl const* node,
    StringRef name)
{
    std::set<CXXRecordDecl const*> visited;
    return node_nested_type(context, node, name, visited);
}

class StateMetadataCollector final {
public:
    explicit StateMetadataCollector(ASTContext& context)
        : context_(context)
    {}

    void record_node(CXXRecordDecl const* node)
    {
        if (!node || !node->isCompleteDefinition() || node->isLambda()) return;
        // An uninstantiated class template has no concrete state ABI. Asking
        // Clang for its layout recursively instantiates dependent members, so
        // only concrete specializations participate in source metadata.
        if (!has_concrete_template_arguments(node)) return;

        auto const node_type = context_.getCanonicalTagType(node);
        auto const node_type_name = type_string(context_, node_type);
        // The canonical declaration of every specialization is the primary
        // class template. Deduplicate by concrete spelling so each NodeCodeKey
        // receives exactly one pair of State/TockState records.
        if (!seen_node_types_.insert(node_type_name).second) return;

        auto const node_type_usr = declaration_usr(context_, node);
        record_state(
            node, node_type, node_type_name, node_type_usr,
            "State", states_);
        record_state(
            node, node_type, node_type_name, node_type_usr,
            "TockState", background_states_);
    }

    llvm::json::Array take_states() { return std::move(states_); }
    llvm::json::Array take_background_states()
    {
        return std::move(background_states_);
    }

private:
    void record_state(
        CXXRecordDecl const* node,
        QualType node_type,
        std::string const& node_type_name,
        std::string const& node_type_usr,
        StringRef state_name,
        llvm::json::Array& output)
    {
        auto state_type = node_nested_type(context_, node, state_name);
        if (!state_type) return;
        auto const canonical_state_type = state_type->getCanonicalType();
        auto const* state_record = canonical_state_type->getAsCXXRecordDecl();
        if (state_record && !state_record->isCompleteDefinition()) return;

        if (state_record && state_record->getNumBases() != 0) {
            auto id = context_.getDiagnostics().getCustomDiagID(
                DiagnosticsEngine::Error,
                "Node::%0 must not have base classes");
            context_.getDiagnostics().Report(state_record->getLocation(), id)
                << state_name;
            return;
        }

        llvm::json::Array fields;
        if (state_record) {
            auto const& layout = context_.getASTRecordLayout(state_record);
            unsigned index = 0;
            for (auto* field : state_record->fields()) {
                auto const type = field->getType();
                llvm::json::Object field_record{
                    {"name", field->getNameAsString()},
                    {"nominal_id", declaration_usr(context_, field)},
                    {"type_usr", type_usr(context_, type)},
                    {"type", type_string(context_, type)},
                    {"bit_offset", static_cast<std::int64_t>(layout.getFieldOffset(index++))},
                    {"size_bits", static_cast<std::int64_t>(context_.getTypeSize(type))},
                    {"alignment_bits", static_cast<std::int64_t>(context_.getTypeAlign(type))},
                };
                if (field->isBitField()) {
                    field_record["bit_width"] = static_cast<std::int64_t>(
                        field->getBitWidthValue());
                }
                fields.push_back(std::move(field_record));
            }
        }

        output.push_back(llvm::json::Object{
            {"node_code_key", node_code_key(context_, node_type)},
            {"node_type_usr", node_type_usr},
            {"node_type", node_type_name},
            {"type_usr", type_usr(context_, canonical_state_type)},
            {"type", type_string(context_, canonical_state_type)},
            {"definition_fingerprint",
             type_definition_fingerprint(context_, canonical_state_type)},
            {"size_bits", static_cast<std::int64_t>(context_.getTypeSize(canonical_state_type))},
            {"alignment_bits", static_cast<std::int64_t>(context_.getTypeAlign(canonical_state_type))},
            {"fields", std::move(fields)},
        });
    }

    ASTContext& context_;
    llvm::json::Array states_;
    llvm::json::Array background_states_;
    std::set<std::string> seen_node_types_;
};

void collect_config_pointer_offsets(
    ASTContext& context,
    QualType type,
    std::uint64_t base_bit_offset,
    std::vector<std::uint64_t>& offsets)
{
    if (type->isPointerType()) {
        if (base_bit_offset % 8 == 0) offsets.push_back(base_bit_offset / 8);
        return;
    }
    if (auto const* array = dyn_cast<ConstantArrayType>(type.getTypePtr())) {
        auto const element_type = array->getElementType();
        auto const element_size = context.getTypeSize(element_type);
        auto const element_count = array->getSize().getLimitedValue();
        for (std::uint64_t index = 0; index < element_count; ++index) {
            collect_config_pointer_offsets(
                context, element_type, base_bit_offset + index * element_size,
                offsets);
        }
        return;
    }

    auto const* record = type->getAsCXXRecordDecl();
    if (!record || !record->isCompleteDefinition() || record->isUnion()
        || !record->isTriviallyCopyable()) {
        return;
    }
    auto const& layout = context.getASTRecordLayout(record);
    for (auto const& base : record->bases()) {
        if (base.isVirtual()) continue;
        auto const* base_record = base.getType()->getAsCXXRecordDecl();
        if (!base_record) continue;
        auto const base_offset = layout.getBaseClassOffset(base_record);
        collect_config_pointer_offsets(
            context, base.getType(),
            base_bit_offset + static_cast<std::uint64_t>(base_offset.getQuantity()) * 8,
            offsets);
    }
    unsigned index = 0;
    for (auto const* field : record->fields()) {
        collect_config_pointer_offsets(
            context, field->getType(),
            base_bit_offset + layout.getFieldOffset(index++), offsets);
    }
}

class NodeConfigMetadataCollector final {
public:
    explicit NodeConfigMetadataCollector(ASTContext& context)
        : context_(context)
    {}

    void record_node(CXXRecordDecl const* record)
    {
        if (!record || !record->isCompleteDefinition() || record->isLambda()
            || !record->isTriviallyCopyable()
            || !has_concrete_template_arguments(record)) {
            return;
        }

        auto const node_type = context_.getCanonicalTagType(record);
        auto const node_type_name = type_string(context_, node_type);
        if (!seen_node_types_.insert(node_type_name).second) return;

        std::vector<std::uint64_t> offsets;
        collect_config_pointer_offsets(context_, node_type, 0, offsets);
        std::ranges::sort(offsets);
        offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
        llvm::json::Array json_offsets;
        for (auto offset : offsets) {
            json_offsets.push_back(static_cast<std::int64_t>(offset));
        }

        fields_.push_back(llvm::json::Object{
            {"node_code_key", node_code_key(context_, node_type)},
            {"byte_offsets", std::move(json_offsets)},
        });
    }

    llvm::json::Array take_fields() && { return std::move(fields_); }

private:
    ASTContext& context_;
    llvm::json::Array fields_;
    std::set<std::string> seen_node_types_;
};

CXXRecordDecl const* node_type_from_compiler_record_specialization(
    VarTemplateSpecializationDecl const* specialization)
{
    if (!specialization) return nullptr;
    auto const* primary_template = specialization->getSpecializedTemplate();
    if (!primary_template || primary_template->getQualifiedNameAsString()
            != "iv::details::node_compiler_record") {
        return nullptr;
    }
    auto const values = specialization->getTemplateArgs().asArray();
    if (values.empty() || values.front().getKind() != TemplateArgument::Type) {
        return nullptr;
    }
    return values.front().getAsType()->getAsCXXRecordDecl();
}

class ReflectedNodeDiscovery final {
public:
    explicit ReflectedNodeDiscovery(ASTContext& context)
    {
        auto const iv_name = DeclarationName{&context.Idents.get("iv")};
        auto const details_name = DeclarationName{&context.Idents.get("details")};
        auto const record_name = DeclarationName{
            &context.Idents.get("node_compiler_record")};

        for (auto const* iv_declaration
             : context.getTranslationUnitDecl()->lookup(iv_name)) {
            auto const* iv_namespace = dyn_cast<NamespaceDecl>(iv_declaration);
            if (!iv_namespace) continue;
            for (auto const* details_declaration
                 : iv_namespace->lookup(details_name)) {
                auto const* details_namespace =
                    dyn_cast<NamespaceDecl>(details_declaration);
                if (!details_namespace) continue;
                for (auto const* declaration : details_namespace->lookup(record_name)) {
                    record_template(dyn_cast<VarTemplateDecl>(declaration));
                }
            }
        }
    }

    std::span<CXXRecordDecl const* const> nodes() const
    {
        return {nodes_.data(), nodes_.size()};
    }

private:
    void record_template(VarTemplateDecl const* variable_template)
    {
        if (!variable_template) return;

        auto const* primary_template = variable_template->getCanonicalDecl();
        if (primary_template->getQualifiedNameAsString()
                != "iv::details::node_compiler_record") {
            return;
        }

        // `reflect_node<T>` instantiates this variable template for every
        // node that actually enters a GraphBuilder. This remains available
        // even when the surrounding GraphBuilder::node<T> specialization is
        // materialized lazily during code generation.
        for (auto const* specialization : primary_template->specializations()) {
            auto const* node =
                node_type_from_compiler_record_specialization(specialization);
            if (!node
                || std::find(nodes_.begin(), nodes_.end(), node)
                    != nodes_.end()) {
                continue;
            }
            nodes_.push_back(node);
        }
    }

    std::vector<CXXRecordDecl const*> nodes_;
};

class ConfigurationExpressionAdapterFinder final
    : public RecursiveASTVisitor<ConfigurationExpressionAdapterFinder> {
public:
    bool VisitFunctionTemplateDecl(FunctionTemplateDecl* declaration)
    {
        if (!declaration) return true;
        auto const name = declaration->getQualifiedNameAsString();
        if (name == "iv::details::configuration_expression_details::construct_scalar")
            construct_scalar = declaration;
        else if (name == "iv::details::configuration_expression_details::read_scalar")
            read_scalar = declaration;
        else if (name == "iv::details::configuration_expression_details::default_construct")
            default_construct = declaration;
        else if (name == "iv::details::configuration_expression_details::destroy")
            destroy = declaration;
        else if (name == "iv::details::configuration_expression_details::assign")
            assign = declaration;
        else if (name == "iv::details::configuration_expression_details::append_element")
            append_element = declaration;
        else if (name == "iv::details::configuration_expression_details::assign_element")
            assign_element = declaration;
        else if (name == "iv::details::configuration_expression_details::element_count")
            element_count = declaration;
        else if (name == "iv::details::configuration_expression_details::element_at")
            element_at = declaration;
        else if (name == "iv::details::configuration_expression_details::construct_node_address")
            construct_node_address = declaration;
        return true;
    }

    FunctionTemplateDecl* construct_scalar = nullptr;
    FunctionTemplateDecl* read_scalar = nullptr;
    FunctionTemplateDecl* default_construct = nullptr;
    FunctionTemplateDecl* destroy = nullptr;
    FunctionTemplateDecl* assign = nullptr;
    FunctionTemplateDecl* append_element = nullptr;
    FunctionTemplateDecl* assign_element = nullptr;
    FunctionTemplateDecl* element_count = nullptr;
    FunctionTemplateDecl* element_at = nullptr;
    FunctionTemplateDecl* construct_node_address = nullptr;
};

class ConfigurationTypeIdentityCollector final
    : public RecursiveASTVisitor<ConfigurationTypeIdentityCollector> {
public:
    explicit ConfigurationTypeIdentityCollector(CompilerInstance& compiler)
        : compiler_(compiler), context_(compiler.getASTContext())
    {
        ConfigurationExpressionAdapterFinder finder;
        finder.TraverseDecl(context_.getTranslationUnitDecl());
        construct_scalar_ = finder.construct_scalar;
        read_scalar_ = finder.read_scalar;
        default_construct_ = finder.default_construct;
        destroy_ = finder.destroy;
        assign_ = finder.assign;
        append_element_ = finder.append_element;
        assign_element_ = finder.assign_element;
        element_count_ = finder.element_count;
        element_at_ = finder.element_at;
        construct_node_address_ = finder.construct_node_address;
    }

    bool VisitVarTemplateSpecializationDecl(
        VarTemplateSpecializationDecl* specialization)
    {
        if (!specialization) return true;
        auto const* variable_template = specialization->getSpecializedTemplate();
        if (!variable_template
            || variable_template->getCanonicalDecl()->getQualifiedNameAsString()
                != "iv::details::configuration_type_identity_storage") {
            return true;
        }
        auto const arguments = specialization->getTemplateArgs().asArray();
        if (arguments.size() != 1 || arguments.front().getKind() != TemplateArgument::Type) {
            return true;
        }
        auto const symbol = mangled_global_name(context_, specialization);
        if (symbol.empty() || !seen_.insert(symbol).second) return true;
        auto metadata = configuration_type_metadata(context_, arguments.front().getAsType());
        metadata["expression_schema"] = schema_metadata(
            arguments.front().getAsType(), specialization->getLocation());
        metadata["symbol"] = symbol;
        identities_.push_back(std::move(metadata));
        return true;
    }

    void record_type(
        QualType type,
        SourceLocation location,
        std::string symbol)
    {
        type = transported_configuration_type(type);
        if (symbol.empty() || !seen_.insert(symbol).second) return;
        auto metadata = configuration_type_metadata(context_, type);
        metadata["expression_schema"] = schema_metadata(type, location);
        metadata["symbol"] = std::move(symbol);
        metadata["synthesized"] = true;
        identities_.push_back(std::move(metadata));
    }

    llvm::json::Array take_identities() &&
    {
        std::ranges::sort(identities_, [](llvm::json::Value const& lhs,
                                          llvm::json::Value const& rhs) {
            return lhs.getAsObject()->getString("symbol")->str()
                < rhs.getAsObject()->getString("symbol")->str();
        });
        return std::move(identities_);
    }

private:
    FunctionDecl* specialize(
        FunctionTemplateDecl* function_template,
        QualType type,
        SourceLocation location)
    {
        if (!function_template) return nullptr;
        TemplateArgumentListInfo explicit_arguments(location, location);
        explicit_arguments.addArgument(compiler_.getSema().getTrivialTemplateArgumentLoc(
            TemplateArgument(type), QualType{}, location));
        sema::TemplateDeductionInfo deduction(location);
        FunctionDecl* specialization = nullptr;
        auto const result = compiler_.getSema().DeduceTemplateArguments(
            function_template, &explicit_arguments, specialization, deduction, false);
        if (result != TemplateDeductionResult::Success || !specialization) {
            auto id = compiler_.getDiagnostics().getCustomDiagID(
                DiagnosticsEngine::Error,
                "cannot generate configuration-expression adapter for type '%0'");
            compiler_.getDiagnostics().Report(location, id)
                << type_string(context_, type);
            return nullptr;
        }
        if (!specialization->hasAttr<UsedAttr>())
            specialization->addAttr(UsedAttr::CreateImplicit(context_));
        if (!specialization->doesThisDeclarationHaveABody()) {
            compiler_.getSema().MarkFunctionReferenced(location, specialization);
            compiler_.getSema().InstantiateFunctionDefinition(
                location, specialization, true, true);
        }
        return specialization;
    }

    std::string adapter_symbol(
        FunctionTemplateDecl* function_template,
        QualType type,
        SourceLocation location)
    {
        auto* function = specialize(function_template, type, location);
        if (!function) return {};
        ASTNameGenerator names(context_);
        return names.getName(function);
    }

    static ClassTemplateSpecializationDecl const* template_specialization(
        QualType type)
    {
        type = transported_configuration_type(type);
        auto const* record = type->getAsCXXRecordDecl();
        return dyn_cast_or_null<ClassTemplateSpecializationDecl>(record);
    }

    llvm::json::Object schema_metadata(QualType type, SourceLocation location)
    {
        std::vector<Type const*> active_types;
        return schema_metadata(type, location, active_types);
    }

    llvm::json::Object schema_metadata(
        QualType type,
        SourceLocation location,
        std::vector<Type const*>& active_types)
    {
        type = transported_configuration_type(type);
        llvm::json::Object result{
            {"display_name", type_string(context_, type)},
            {"size_bits", static_cast<std::int64_t>(context_.getTypeSize(type))},
            {"alignment_bits", static_cast<std::int64_t>(context_.getTypeAlign(type))},
            {"destroy_symbol", adapter_symbol(destroy_, type, location)},
        };
        auto const* canonical_type = context_.getCanonicalType(type).getTypePtr();
        if (std::ranges::contains(active_types, canonical_type)) {
            result["kind"] = "unsupported";
            result["reason"] =
                "recursive configuration value schemas are not supported";
            return result;
        }
        active_types.push_back(canonical_type);
        struct ActiveTypeGuard {
            std::vector<Type const*>& values;
            ~ActiveTypeGuard() { values.pop_back(); }
        } guard{active_types};

        if (type->isBooleanType()) {
            result["kind"] = "boolean";
            result["construct_scalar_symbol"] =
                adapter_symbol(construct_scalar_, type, location);
            result["read_scalar_symbol"] =
                adapter_symbol(read_scalar_, type, location);
            return result;
        }
        if (auto const* enumeration = type->getAs<EnumType>()) {
            result["kind"] = "enumeration";
            auto const underlying = enumeration->getDecl()->getIntegerType();
            if (context_.getTypeSize(underlying) > 64) {
                result["kind"] = "unsupported";
                result["reason"] = "enum underlying type is wider than 64 bits";
                return result;
            }
            result["integer_is_signed"] = underlying->isSignedIntegerType();
            result["integer_bit_width"] =
                static_cast<std::int64_t>(context_.getTypeSize(underlying));
            result["construct_scalar_symbol"] =
                adapter_symbol(construct_scalar_, type, location);
            result["read_scalar_symbol"] =
                adapter_symbol(read_scalar_, type, location);
            llvm::json::Array values;
            if (auto const* definition = enumeration->getDecl()->getDefinition()) {
                for (auto const* enumerator : definition->enumerators()) {
                    auto const value = enumerator->getInitVal();
                    llvm::json::Object item{{"name", enumerator->getNameAsString()}};
                    if (underlying->isSignedIntegerType())
                        item["signed_value"] = value.getSExtValue();
                    else
                        item["unsigned_value"] =
                            std::to_string(value.getZExtValue());
                    values.push_back(std::move(item));
                }
            }
            result["enumerators"] = std::move(values);
            return result;
        }
        if (type->isSignedIntegerType() || type->isUnsignedIntegerType()) {
            result["kind"] = type->isSignedIntegerType()
                ? "signed_integer" : "unsigned_integer";
            result["integer_is_signed"] = type->isSignedIntegerType();
            result["integer_bit_width"] =
                static_cast<std::int64_t>(context_.getTypeSize(type));
            result["construct_scalar_symbol"] =
                adapter_symbol(construct_scalar_, type, location);
            result["read_scalar_symbol"] =
                adapter_symbol(read_scalar_, type, location);
            return result;
        }
        if (type->isRealFloatingType()) {
            result["kind"] = "floating_point";
            result["construct_scalar_symbol"] =
                adapter_symbol(construct_scalar_, type, location);
            result["read_scalar_symbol"] =
                adapter_symbol(read_scalar_, type, location);
            return result;
        }
        if (auto const* specialization = template_specialization(type)) {
            auto const template_name = specialization->getSpecializedTemplate()
                ->getQualifiedNameAsString();
            auto const arguments = specialization->getTemplateArgs().asArray();
            if (template_name.ends_with("::basic_string") && !arguments.empty()
                && arguments.front().getKind() == TemplateArgument::Type
                && context_.hasSameType(
                    arguments.front().getAsType(), context_.CharTy)) {
                result["kind"] = "string";
                result["construct_scalar_symbol"] =
                    adapter_symbol(construct_scalar_, type, location);
                result["read_scalar_symbol"] =
                    adapter_symbol(read_scalar_, type, location);
                return result;
            }
            if ((template_name == "std::vector" || template_name == "std::array")
                && !arguments.empty()
                && arguments.front().getKind() == TemplateArgument::Type) {
                auto const element = arguments.front().getAsType();
                result["kind"] = "sequence";
                result["default_construct_symbol"] =
                    adapter_symbol(default_construct_, type, location);
                result["element_count_symbol"] =
                    adapter_symbol(element_count_, type, location);
                result["element_at_symbol"] =
                    adapter_symbol(element_at_, type, location);
                if (template_name == "std::vector") {
                    result["append_element_symbol"] =
                        adapter_symbol(append_element_, type, location);
                } else if (arguments.size() > 1
                    && arguments[1].getKind() == TemplateArgument::Integral) {
                    result["element_assign_symbol"] =
                        adapter_symbol(assign_element_, type, location);
                    result["fixed_element_count"] = static_cast<std::int64_t>(
                        arguments[1].getAsIntegral().getZExtValue());
                }
                result["element_type"] = schema_metadata(
                    element, location, active_types);
                return result;
            }
        }

        auto const name = record_name(type);
        if (name == "NodeRef" || name == "TypedNodeRef") {
            result["kind"] = "node_reference";
            return result;
        }
        // A subgraph closure is C++ construction behavior: it can be passed to
        // a construction call but has no restricted-expression form, and it
        // must not fall through to the converted-scalar record classification.
        if (auto const* record = type->getAsCXXRecordDecl(); record
            && record->getQualifiedNameAsString() == "iv::SubgraphClosure") {
            result["kind"] = "unsupported";
            result["reason"] =
                "a SubgraphClosure is construction-time C++ behavior with no "
                "configuration-expression form";
            return result;
        }
        if (auto const* record = type->getAsCXXRecordDecl(); record
            && record->getQualifiedNameAsString() == "iv::NodeAddress") {
            result["kind"] = "node_address";
            result["construct_node_address_symbol"] =
                adapter_symbol(construct_node_address_, type, location);
            return result;
        }

        if (auto const* record = type->getAsCXXRecordDecl(); record
            && record->getDefinition() && record->getDefinition()->isAggregate()) {
            auto const* definition = record->getDefinition();
            if (definition->isUnion() || definition->getNumBases() != 0) {
                result["kind"] = "unsupported";
                result["reason"] =
                    "aggregate unions and aggregate base classes are not supported";
                return result;
            }
            result["kind"] = "aggregate";
            result["default_construct_symbol"] =
                adapter_symbol(default_construct_, type, location);
            auto const& layout = context_.getASTRecordLayout(definition);
            llvm::json::Array fields;
            std::size_t index = 0;
            for (auto const* field : definition->fields()) {
                if (field->isBitField() || field->getType()->isReferenceType()
                    || field->getType().isConstQualified()
                    || field->getName().empty()) {
                    result["kind"] = "unsupported";
                    result["reason"] =
                        "aggregate has a const, bit-field, reference, or unnamed member";
                    return result;
                }
                auto const field_type = transported_configuration_type(field->getType());
                fields.push_back(llvm::json::Object{
                    {"name", field->getNameAsString()},
                    {"bit_offset", static_cast<std::int64_t>(
                        layout.getFieldOffset(index++))},
                    {"assign_symbol", adapter_symbol(assign_, field_type, location)},
                    {"type", schema_metadata(field_type, location, active_types)},
                });
            }
            result["fields"] = std::move(fields);
            return result;
        }

        if (type->getAsCXXRecordDecl()) {
            result["kind"] = "converted_scalar";
            result["construct_scalar_symbol"] =
                adapter_symbol(construct_scalar_, type, location);
            result["read_scalar_symbol"] =
                adapter_symbol(read_scalar_, type, location);
            return result;
        }
        result["kind"] = "unsupported";
        result["reason"] = "type is not a supported configuration-expression value";
        return result;
    }

    CompilerInstance& compiler_;
    ASTContext& context_;
    llvm::json::Array identities_;
    std::set<std::string> seen_;
    FunctionTemplateDecl* construct_scalar_ = nullptr;
    FunctionTemplateDecl* read_scalar_ = nullptr;
    FunctionTemplateDecl* default_construct_ = nullptr;
    FunctionTemplateDecl* destroy_ = nullptr;
    FunctionTemplateDecl* assign_ = nullptr;
    FunctionTemplateDecl* append_element_ = nullptr;
    FunctionTemplateDecl* assign_element_ = nullptr;
    FunctionTemplateDecl* element_count_ = nullptr;
    FunctionTemplateDecl* element_at_ = nullptr;
    FunctionTemplateDecl* construct_node_address_ = nullptr;
};

class NodeMessageHandlerCollector final {
public:
    NodeMessageHandlerCollector(
        CompilerInstance& compiler,
        ConfigurationTypeIdentityCollector& configuration_types)
        : compiler_(compiler)
        , context_(compiler.getASTContext())
        , configuration_types_(configuration_types)
    {}

    void record_node(CXXRecordDecl const* node)
    {
        if (!node) return;
        record(
            node,
            "iv::NodeMessageContext",
            false,
            {});
    }

    void record_module(
        std::string_view definition_id,
        CXXRecordDecl const* module)
    {
        if (!module) return;
        record(
            module,
            "iv::ModuleMessageContext",
            true,
            definition_id);
    }

    llvm::json::Array take_handlers() &&
    {
        std::ranges::sort(handlers_, [](auto const& lhs, auto const& rhs) {
            auto const* a = lhs.getAsObject();
            auto const* b = rhs.getAsObject();
            return std::tuple{
                a->getString("kind")->str(),
                a->getString("method_symbol")->str(),
                a->getString("event_type_symbol")->str()}
                < std::tuple{
                b->getString("kind")->str(),
                b->getString("method_symbol")->str(),
                b->getString("event_type_symbol")->str()};
        });
        return std::move(handlers_);
    }

private:
    void record(
        CXXRecordDecl const* owner,
        StringRef context_template_name,
        bool module,
        std::string_view definition_id)
    {
        if (!owner || !owner->isCompleteDefinition() || owner->isLambda()
            || !has_concrete_template_arguments(owner)) {
            return;
        }
        auto const owner_type = context_.getCanonicalTagType(owner);
        for (auto const* candidate : owner->methods()) {
            if (!candidate || candidate->getNameAsString() != "on_message") continue;
            auto* method = const_cast<CXXMethodDecl*>(candidate);
            auto reject = [&](StringRef message) {
                auto id = context_.getDiagnostics().getCustomDiagID(
                    DiagnosticsEngine::Error, "%0");
                context_.getDiagnostics().Report(method->getLocation(), id)
                    << message;
            };
            if (method->isStatic() || method->getDescribedFunctionTemplate()
                || method->getAccess() != AS_public || !method->isConst()
                || !method->getReturnType()->isVoidType()
                || method->getNumParams() != 2 || !method->hasBody()) {
                reject(
                    "on_message must be a public, non-template, const member with a void return, a typed message-context reference, and one const event reference");
                continue;
            }
            auto const* context_reference = method->getParamDecl(0)
                ->getType()->getAs<LValueReferenceType>();
            auto const* context_record = context_reference
                ? context_reference->getPointeeType()->getAsCXXRecordDecl()
                : nullptr;
            auto const* context_specialization =
                dyn_cast_or_null<ClassTemplateSpecializationDecl>(
                    context_record);
            auto const context_arguments = context_specialization
                ? context_specialization->getTemplateArgs().asArray()
                : ArrayRef<TemplateArgument>{};
            if (!context_specialization
                || context_specialization->getSpecializedTemplate()
                       ->getQualifiedNameAsString() != context_template_name
                || context_arguments.size() != 1
                || context_arguments.front().getKind()
                    != TemplateArgument::Type
                || !context_.hasSameType(
                    context_.getCanonicalType(
                        context_arguments.front().getAsType()),
                    context_.getCanonicalType(owner_type))) {
                reject(
                    "on_message first parameter must use the matching typed message context");
                continue;
            }
            auto const* event_reference = method->getParamDecl(1)
                ->getType()->getAs<LValueReferenceType>();
            if (!event_reference
                || !event_reference->getPointeeType().isConstQualified()) {
                reject("on_message event parameter must be Event const&");
                continue;
            }
            auto const event_type = transported_configuration_type(
                event_reference->getPointeeType());
            auto const method_symbol = mangled_function_name(context_, method);
            if (method_symbol.empty()) {
                reject("cannot generate the LLVM symbol for on_message");
                continue;
            }
            auto const identity_material = type_usr(context_, event_type) + ":"
                + type_definition_fingerprint(context_, event_type);
            auto const event_type_symbol = "__iv_node_message_type_"
                + hex_u64(fingerprint_hash(identity_material));
            configuration_types_.record_type(
                event_type, method->getLocation(), event_type_symbol);

            if (!method->hasAttr<UsedAttr>()) {
                method->addAttr(UsedAttr::CreateImplicit(context_));
            }
            compiler_.getSema().MarkFunctionReferenced(
                method->getLocation(), method);
            llvm::json::Object handler{
                {"kind", module ? "module" : "node"},
            };
            if (!module) {
                handler["node_code_key"] = node_code_key(context_, owner_type);
            }
            else handler["definition_id"] = std::string(definition_id);
            handler["method_symbol"] = method_symbol;
            handler["event_type_symbol"] = event_type_symbol;
            handlers_.push_back(std::move(handler));
        }
    }
    CompilerInstance& compiler_;
    ASTContext& context_;
    ConfigurationTypeIdentityCollector& configuration_types_;
    llvm::json::Array handlers_;
};

class RegisteredNodeAdapterFinder final
    : public RecursiveASTVisitor<RegisteredNodeAdapterFinder> {
public:
    bool VisitFunctionTemplateDecl(FunctionTemplateDecl* declaration)
    {
        if (!declaration) return true;
        auto const name = declaration->getQualifiedNameAsString();
        if (name == "iv::details::configure_node_constructor") {
            configure = declaration;
        } else if (name == "iv::details::node_constructor_signature") {
            signature = declaration;
        } else if (name == "iv::details::configure_module_class_constructor") {
            module_configure = declaration;
        } else if (name == "iv::details::module_class_constructor_signature") {
            module_signature = declaration;
        }
        return true;
    }

    FunctionTemplateDecl* configure = nullptr;
    FunctionTemplateDecl* signature = nullptr;
    FunctionTemplateDecl* module_configure = nullptr;
    FunctionTemplateDecl* module_signature = nullptr;
};

class PackageDefinitionCollector final
    : public RecursiveASTVisitor<PackageDefinitionCollector> {
public:
    struct RegisteredModuleClass {
        std::string definition_id{};
        CXXRecordDecl const* type = nullptr;
    };

    explicit PackageDefinitionCollector(CompilerInstance& compiler)
        : compiler_(compiler), context_(compiler.getASTContext())
    {}

    bool VisitVarDecl(VarDecl* declaration)
    {
        if (!declaration || !declaration->hasInit()) return true;
        auto const* record = declaration->getType()->getAsCXXRecordDecl();
        if (!record
            || record->getQualifiedNameAsString()
                != "iv::details::PackageDefinition") {
            return true;
        }

        auto const variable_name = declaration->getName();
        bool const is_module_function =
            variable_name.starts_with("iv_package_module_definition_");
        bool const is_module_class =
            variable_name.starts_with("iv_package_module_class_definition_");
        bool const is_module = is_module_function || is_module_class;
        bool const is_node = variable_name.starts_with("iv_package_node_definition_");
        if (!is_module && !is_node) return true;

        auto* initializer = dyn_cast<InitListExpr>(
            declaration->getInit()->IgnoreParenImpCasts());
        if (!initializer || initializer->getNumInits() < 11) return true;

        auto expression = [&](unsigned index) -> Expr* {
            return initializer->getInit(index)->IgnoreParenImpCasts();
        };
        auto const* literal = dyn_cast<StringLiteral>(expression(1));
        if (!literal || literal->getString().empty()) return true;

        auto id = literal->getString().str();
        if (!seen_.insert(id).second) {
            auto diagnostic = compiler_.getDiagnostics().getCustomDiagID(
                DiagnosticsEngine::Error,
                "IV package defines stable ID '%0' more than once");
            compiler_.getDiagnostics().Report(declaration->getLocation(), diagnostic)
                << id;
            return true;
        }
        llvm::json::Object metadata{
            {"id", id},
            {"kind", is_module ? "module" : "node"},
            {"declaration_usr", declaration_usr(context_, declaration)},
        };
        if (auto const* source_file = dyn_cast<StringLiteral>(expression(3))) {
            metadata["source_file"] = source_file->getString();
        }

        if (is_module) {
            Expr const* implementation = expression(7);
            if (auto const* address = dyn_cast<UnaryOperator>(implementation)) {
                implementation = address->getSubExpr()->IgnoreParenImpCasts();
            }
            if (auto const* reference = dyn_cast<DeclRefExpr>(implementation)) {
                metadata["implementation_usr"] = declaration_usr(
                    context_, reference->getDecl());
            }
            if (is_module_class) {
                auto const* module = module_class_type(expression(10));
                if (!module) {
                    auto diagnostic = compiler_.getDiagnostics().getCustomDiagID(
                        DiagnosticsEngine::Error,
                        "cannot identify the IV_MODULE_CLASS implementation type");
                    compiler_.getDiagnostics().Report(
                        declaration->getLocation(), diagnostic);
                } else {
                    metadata["module_type_usr"] = declaration_usr(context_, module);
                    module_classes_.push_back({
                        .definition_id = id,
                        .type = module,
                    });
                    install_module_adapter(initializer, declaration, module);
                }
            }
        } else {
            Expr const* compiler_record = expression(9);
            if (auto const* address = dyn_cast<UnaryOperator>(compiler_record)) {
                compiler_record = address->getSubExpr()->IgnoreParenImpCasts();
            }
            if (auto const* reference = dyn_cast<DeclRefExpr>(compiler_record)) {
                auto const* specialization = dyn_cast<VarTemplateSpecializationDecl>(
                    reference->getDecl());
                if (auto const* node = node_type_from_compiler_record_specialization(
                        specialization)) {
                    auto const type = context_.getCanonicalTagType(node);
                    metadata["node_type_usr"] = declaration_usr(context_, node);
                    metadata["node_code_key"] = node_code_key(context_, type);
                    install_node_adapter(initializer, declaration, node);
                }
            }
        }
        definitions_.push_back(std::move(metadata));
        return true;
    }

    llvm::json::Array take_definitions() &&
    {
        std::ranges::sort(definitions_, [](llvm::json::Value const& lhs,
                                           llvm::json::Value const& rhs) {
            return lhs.getAsObject()->getString("id")->str()
                < rhs.getAsObject()->getString("id")->str();
        });
        return std::move(definitions_);
    }

    std::span<RegisteredModuleClass const> module_classes() const noexcept
    {
        return module_classes_;
    }

private:
    CompilerInstance& compiler_;
    ASTContext& context_;
    llvm::json::Array definitions_;
    std::set<std::string> seen_;
    std::vector<RegisteredModuleClass> module_classes_;

    Sema& sema() { return compiler_.getSema(); }

    void ensure_node_adapter_templates()
    {
        if (helpers_searched_) return;
        helpers_searched_ = true;
        RegisteredNodeAdapterFinder finder;
        finder.TraverseDecl(context_.getTranslationUnitDecl());
        node_configure_template_ = finder.configure;
        node_signature_template_ = finder.signature;
        module_configure_template_ = finder.module_configure;
        module_signature_template_ = finder.module_signature;
    }

    CXXRecordDecl const* module_class_type(Expr const* expression) const
    {
        if (!expression) return nullptr;
        expression = expression->IgnoreParenImpCasts();
        if (auto const* address = dyn_cast<UnaryOperator>(expression)) {
            expression = address->getSubExpr()->IgnoreParenImpCasts();
        }
        auto const* reference = dyn_cast<DeclRefExpr>(expression);
        auto const* function = reference
            ? dyn_cast<FunctionDecl>(reference->getDecl())
            : nullptr;
        auto const* arguments = function
            ? function->getTemplateSpecializationArgs()
            : nullptr;
        if (!arguments || arguments->size() == 0
            || arguments->get(0).getKind() != TemplateArgument::Type) {
            return nullptr;
        }
        return arguments->get(0).getAsType()->getAsCXXRecordDecl();
    }

    FunctionDecl* specialize(
        FunctionTemplateDecl* function_template,
        std::span<QualType const> arguments,
        SourceLocation location)
    {
        if (!function_template) return nullptr;
        TemplateArgumentListInfo explicit_arguments(location, location);
        for (auto type : arguments) {
            explicit_arguments.addArgument(sema().getTrivialTemplateArgumentLoc(
                TemplateArgument(type), QualType{}, location));
        }
        sema::TemplateDeductionInfo deduction(location);
        FunctionDecl* specialization = nullptr;
        auto const result = sema().DeduceTemplateArguments(
            function_template, &explicit_arguments, specialization, deduction, false);
        if (result == TemplateDeductionResult::Success && specialization) {
            return specialization;
        }
        auto id = compiler_.getDiagnostics().getCustomDiagID(
            DiagnosticsEngine::Error,
            "cannot generate IV_NODE constructor adapter");
        compiler_.getDiagnostics().Report(location, id);
        return nullptr;
    }

    Expr* function_pointer(FunctionDecl* function, SourceLocation location)
    {
        if (!function) return nullptr;
        auto reference = sema().BuildDeclRefExpr(
            function, function->getType(), VK_LValue, location);
        auto address = sema().BuildUnaryOp(
            nullptr, location, UO_AddrOf, reference);
        return address.isInvalid() ? nullptr : address.get();
    }

    std::optional<std::vector<QualType>> visible_node_constructor_parameters(
        CXXRecordDecl const* node,
        SourceLocation location,
        bool module_class = false)
    {
        std::vector<CXXConstructorDecl*> constructors;
        for (auto* constructor : node->ctors()) {
            if (!constructor || constructor->isCopyOrMoveConstructor()) continue;
            if (constructor->isDeleted()) continue;
            constructors.push_back(constructor);
        }
        // An aggregate/default-only node has no CXXConstructorDecl until Sema
        // materializes one lazily.  Do not ask Sema to materialize or inspect
        // that special member while the parser is delivering the IV_NODE's
        // top-level declaration: Clang 23 can crash in that re-entrant state.
        // An empty explicit-constructor list is the zero-argument adapter;
        // constructing it remains the normal C++ compiler's responsibility,
        // so a deleted/inaccessible implicit default constructor still yields
        // an ordinary provider-package diagnostic.
        if (constructors.empty()) return std::vector<QualType>{};
        if (constructors.size() != 1) {
            auto id = compiler_.getDiagnostics().getCustomDiagID(
                DiagnosticsEngine::Error, "%0");
            compiler_.getDiagnostics().Report(location, id)
                << (module_class
                    ? "IV_MODULE_CLASS requires exactly one non-copy, non-move constructor"
                    : "IV_NODE requires exactly one non-copy, non-move constructor");
            return std::nullopt;
        }
        auto* constructor = constructors.front();
        if (constructor->getDescribedFunctionTemplate()
            || constructor->getAccess() != AS_public) {
            auto id = compiler_.getDiagnostics().getCustomDiagID(
                DiagnosticsEngine::Error, "%0");
            compiler_.getDiagnostics().Report(location, id)
                << (module_class
                    ? "IV_MODULE_CLASS constructor must be a public non-template constructor"
                    : "IV_NODE constructor must be a public non-template constructor");
            return std::nullopt;
        }
        std::vector<QualType> parameters;
        parameters.reserve(constructor->getNumParams());
        for (auto const* parameter : constructor->parameters()) {
            parameters.push_back(parameter->getType());
        }
        return parameters;
    }

    void install_node_adapter(
        InitListExpr* initializer,
        VarDecl* declaration,
        CXXRecordDecl const* node)
    {
        ensure_node_adapter_templates();
        auto const location = declaration->getLocation();
        if (!node_configure_template_ || !node_signature_template_) {
            auto id = compiler_.getDiagnostics().getCustomDiagID(
                DiagnosticsEngine::Error,
                "IV_NODE adapter templates were not found; include <intravenous/dsl.h>");
            compiler_.getDiagnostics().Report(location, id);
            return;
        }
        auto parameters = visible_node_constructor_parameters(node, location);
        if (!parameters) return;

        std::vector<QualType> arguments;
        arguments.reserve(1 + parameters->size());
        arguments.push_back(context_.getCanonicalTagType(node));
        arguments.insert(arguments.end(), parameters->begin(), parameters->end());
        auto* configure = specialize(
            node_configure_template_, arguments, location);
        auto* signature = specialize(
            node_signature_template_, arguments, location);
        auto* configure_pointer = function_pointer(configure, location);
        auto* signature_pointer = function_pointer(signature, location);
        if (!configure_pointer || !signature_pointer) return;
        initializer->setInit(8, configure_pointer);
        initializer->setInit(10, signature_pointer);
    }

    void install_module_adapter(
        InitListExpr* initializer,
        VarDecl* declaration,
        CXXRecordDecl const* module)
    {
        ensure_node_adapter_templates();
        auto const location = declaration->getLocation();
        if (!module_configure_template_ || !module_signature_template_) {
            auto id = compiler_.getDiagnostics().getCustomDiagID(
                DiagnosticsEngine::Error,
                "IV_MODULE_CLASS adapter templates were not found; include <intravenous/dsl.h>");
            compiler_.getDiagnostics().Report(location, id);
            return;
        }
        auto parameters = visible_node_constructor_parameters(
            module, location, true);
        if (!parameters) return;

        std::vector<QualType> arguments;
        arguments.reserve(1 + parameters->size());
        arguments.push_back(context_.getCanonicalTagType(module));
        arguments.insert(arguments.end(), parameters->begin(), parameters->end());
        auto* configure = specialize(
            module_configure_template_, arguments, location);
        auto* signature = specialize(
            module_signature_template_, arguments, location);
        auto* configure_pointer = function_pointer(configure, location);
        auto* signature_pointer = function_pointer(signature, location);
        if (!configure_pointer || !signature_pointer) return;
        initializer->setInit(7, configure_pointer);
        initializer->setInit(10, signature_pointer);
    }

    bool helpers_searched_ = false;
    FunctionTemplateDecl* node_configure_template_ = nullptr;
    FunctionTemplateDecl* node_signature_template_ = nullptr;
    FunctionTemplateDecl* module_configure_template_ = nullptr;
    FunctionTemplateDecl* module_signature_template_ = nullptr;
};

std::filesystem::path metadata_path(
    CompilerInstance& compiler,
    std::filesystem::path const& directory)
{
    auto const input = compiler.getFrontendOpts().Inputs.empty()
        ? std::string("unknown")
        : compiler.getFrontendOpts().Inputs.front().getFile().str();
    auto const hash = static_cast<std::uint64_t>(llvm::hash_value(input));
    auto name = std::filesystem::path(input).filename().string();
    if (name.empty()) name = "translation-unit";
    return directory / (name + "." + std::to_string(hash) + ".ivmeta.json");
}

void write_state_metadata(
    CompilerInstance& compiler,
    std::filesystem::path const& metadata_dir)
{
    auto& context = compiler.getASTContext();
    StateMetadataCollector state_collector(context);
    NodeConfigMetadataCollector node_config_collector(context);
    ReflectedNodeDiscovery reflected_node_discovery(context);
    ConfigurationTypeIdentityCollector configuration_type_collector(compiler);
    NodeMessageHandlerCollector node_message_collector(
        compiler, configuration_type_collector);
    PackageDefinitionCollector package_definition_collector(compiler);
    // IV_NODE rewrites materialize the constructor-adapter specializations
    // that reference configuration_type_identity_storage<T>. Discover those
    // first so the same snapshot sees eagerly materialized identities; the
    // mutation listener below still refreshes the sidecar for CodeGen-lazy
    // specializations.
    package_definition_collector.TraverseDecl(context.getTranslationUnitDecl());
    configuration_type_collector.TraverseDecl(context.getTranslationUnitDecl());
    // A compiler record is emitted only for a type passed to GraphBuilder.
    // Calls at HandleTranslationUnit see ordinary records; the exact
    // variable-template listener re-runs this small collection when CodeGen
    // materializes a deferred record. Neither path walks unrelated AST nodes.
    for (auto const* node : reflected_node_discovery.nodes()) {
        state_collector.record_node(node);
        node_config_collector.record_node(node);
        node_message_collector.record_node(node);
    }
    for (auto const& module : package_definition_collector.module_classes()) {
        node_message_collector.record_module(
            module.definition_id,
            module.type);
    }

    std::error_code error;
    std::filesystem::create_directories(metadata_dir, error);
    if (error) {
        auto id = compiler.getDiagnostics().getCustomDiagID(
            DiagnosticsEngine::Error,
            "cannot create IV Clang metadata directory '%0': %1");
        compiler.getDiagnostics().Report(id)
            << metadata_dir.string() << error.message();
        return;
    }
    auto const output = metadata_path(compiler, metadata_dir);
    llvm::raw_fd_ostream stream(output.string(), error, llvm::sys::fs::OF_Text);
    if (error) {
        auto id = compiler.getDiagnostics().getCustomDiagID(
            DiagnosticsEngine::Error,
            "cannot write IV Clang metadata '%0': %1");
        compiler.getDiagnostics().Report(id) << output.string() << error.message();
        return;
    }
    auto node_message_handlers =
        std::move(node_message_collector).take_handlers();
    auto configuration_types =
        std::move(configuration_type_collector).take_identities();
    stream << llvm::formatv(
        "{0:2}", llvm::json::Value(llvm::json::Object{
            {"states", state_collector.take_states()},
            {"background_states", state_collector.take_background_states()},
            {"config_pointers", std::move(node_config_collector).take_fields()},
            {"configuration_types", std::move(configuration_types)},
            {"node_message_handlers", std::move(node_message_handlers)},
            {"package_definitions", std::move(package_definition_collector).take_definitions()},
        }));
    stream << '\n';
}

class FunctionDiscovery final : public RecursiveASTVisitor<FunctionDiscovery> {
public:
    explicit FunctionDiscovery(FunctionInstrumenter& instrumenter)
        : instrumenter_(instrumenter)
    {}

    bool VisitFunctionDecl(FunctionDecl* declaration)
    {
        instrumenter_.instrument(declaration);
        return true;
    }

private:
    FunctionInstrumenter& instrumenter_;
};

class ModuleMutationListener final : public ASTMutationListener {
public:
    ModuleMutationListener(
        CompilerInstance& compiler,
        std::filesystem::path metadata_dir,
        FunctionInstrumenter* instrumenter)
        : compiler_(compiler),
          metadata_dir_(std::move(metadata_dir)),
          instrumenter_(instrumenter),
          configuration_type_collector_(compiler)
    {}

    void FunctionDefinitionInstantiated(FunctionDecl const* definition) override
    {
        // Generic-lambda bodies become concrete only after normal source
        // traversal. This callback precedes CodeGen for the instantiated
        // definition, so the existing Sema-based rewrite remains in its IR.
        if (instrumenter_) instrumenter_->instrument(const_cast<FunctionDecl*>(definition));
    }

    void AddedCXXTemplateSpecialization(
        VarTemplateDecl const*,
        VarTemplateSpecializationDecl const* specialization) override
    {
        // A node_compiler_record<T> specialization is the precise signal that
        // a concrete T entered GraphBuilder. Some are deferred until CodeGen,
        // after HandleTranslationUnit has written the initial sidecar. Refresh
        // only for that event; unrelated completed AST records must neither
        // trigger a snapshot nor become node metadata.
        auto const is_configuration_identity = [&] {
            auto const* variable_template = specialization
                ? specialization->getSpecializedTemplate()
                : nullptr;
            return variable_template
                && variable_template->getCanonicalDecl()->getQualifiedNameAsString()
                    == "iv::details::configuration_type_identity_storage";
        };
        if (is_configuration_identity()) {
            // Materialize schema callbacks while the parser still owns a
            // valid translation-unit scope. Doing this for the first time in
            // HandleTranslationUnit is too late for implicit special-member
            // definitions such as std::array's assignment operator.
            configuration_type_collector_.VisitVarTemplateSpecializationDecl(
                const_cast<VarTemplateSpecializationDecl*>(specialization));
        }
        if (initial_snapshot_written_
            && (node_type_from_compiler_record_specialization(specialization)
                || is_configuration_identity())) {
            write_snapshot();
        }
    }

    void write_initial_snapshot()
    {
        write_snapshot();
        initial_snapshot_written_ = true;
    }

private:
    void write_snapshot()
    {
        // This walks only the small set of node_compiler_record variable
        // specializations, not the translation unit. The finalizer runs after
        // the compiler has finished, so its sidecar read observes the latest
        // replacement snapshot.
        write_state_metadata(compiler_, metadata_dir_);
    }

    CompilerInstance& compiler_;
    std::filesystem::path metadata_dir_;
    FunctionInstrumenter* instrumenter_ = nullptr;
    ConfigurationTypeIdentityCollector configuration_type_collector_;
    bool initial_snapshot_written_ = false;
};

class ModuleConsumer final : public ASTConsumer {
public:
    ModuleConsumer(
        CompilerInstance& compiler,
        std::filesystem::path core_source_dir,
        std::filesystem::path metadata_dir,
        bool source_introspection)
        : compiler_(compiler),
          sources_(compiler.getASTContext(), std::move(core_source_dir)),
          instrumenter_(compiler, sources_, source_introspection),
          mutation_listener_(
              compiler,
              std::move(metadata_dir),
              &instrumenter_),
          package_definition_rewriter_(compiler)
    {}

    bool HandleTopLevelDecl(DeclGroupRef declarations) override
    {
        // AddBeforeMainAction invokes this before the normal CodeGen consumer
        // receives a top-level declaration.  IV_NODE records are const data,
        // so their placeholder callbacks must be replaced here rather than
        // during HandleTranslationUnit, after CodeGen has emitted the record.
        for (auto* declaration : declarations) {
            package_definition_rewriter_.TraverseDecl(declaration);
        }

        FunctionDiscovery discovery(instrumenter_);
        for (auto* declaration : declarations) {
            discovery.TraverseDecl(declaration);
        }
        return true;
    }

    void HandleInlineFunctionDefinition(FunctionDecl* declaration) override
    {
        if (auto* method = dyn_cast_or_null<CXXMethodDecl>(declaration);
            method && method->getNameAsString() == "on_message") {
            if (!method->hasAttr<UsedAttr>()) {
                method->addAttr(UsedAttr::CreateImplicit(
                    compiler_.getASTContext()));
            }
            compiler_.getSema().MarkFunctionReferenced(
                method->getLocation(), method);
        }
        instrumenter_.instrument(declaration);
    }

    ASTMutationListener* GetASTMutationListener() override
    {
        return &mutation_listener_;
    }

    void HandleTranslationUnit(ASTContext& context) override
    {
        // Template instantiations can be materialized after their owning
        // top-level declaration was first seen. Catch any remaining configured
        // function bodies before the frontend finishes. Source-identity
        // preparation is mandatory even when optional source annotations are
        // disabled.
        FunctionDiscovery discovery(instrumenter_);
        discovery.TraverseDecl(context.getTranslationUnitDecl());
        // AddBeforeMainAction reaches this point before CodeGen. It contains
        // all ordinary node compiler records. Deferred ones update the same
        // sidecar through AddedCXXTemplateSpecialization above.
        mutation_listener_.write_initial_snapshot();
    }

private:
    CompilerInstance& compiler_;
    SourceModel sources_;
    FunctionInstrumenter instrumenter_;
    ModuleMutationListener mutation_listener_;
    PackageDefinitionCollector package_definition_rewriter_;
};

class ModulePlugin final : public PluginASTAction {
public:
    ActionType getActionType() override { return AddBeforeMainAction; }

    bool ParseArgs(
        CompilerInstance const& compiler,
        std::vector<std::string> const& args) override
    {
        for (auto const& argument : args) {
            constexpr std::string_view core_prefix = "core-source-dir=";
            constexpr std::string_view metadata_prefix = "metadata-dir=";
            constexpr std::string_view source_prefix = "source-introspection=";
            if (std::string_view(argument).starts_with(core_prefix)) {
                core_source_dir_ = argument.substr(core_prefix.size());
            } else if (std::string_view(argument).starts_with(metadata_prefix)) {
                metadata_dir_ = argument.substr(metadata_prefix.size());
            } else if (std::string_view(argument).starts_with(source_prefix)) {
                auto const value = std::string_view(argument).substr(source_prefix.size());
                if (value == "0") source_introspection_ = false;
                else if (value == "1") source_introspection_ = true;
                else {
                    auto id = compiler.getDiagnostics().getCustomDiagID(
                        DiagnosticsEngine::Error,
                        "iv_module_metadata source-introspection must be 0 or 1");
                    compiler.getDiagnostics().Report(id);
                    return false;
                }
            } else {
                auto id = compiler.getDiagnostics().getCustomDiagID(
                    DiagnosticsEngine::Error,
                    "unknown iv_module_metadata argument '%0'");
                compiler.getDiagnostics().Report(id) << argument;
                return false;
            }
        }
        if (core_source_dir_.empty()) {
            auto id = compiler.getDiagnostics().getCustomDiagID(
                DiagnosticsEngine::Error,
                "iv_module_metadata requires core-source-dir=<path>");
            compiler.getDiagnostics().Report(id);
            return false;
        }
        if (metadata_dir_.empty()) {
            auto id = compiler.getDiagnostics().getCustomDiagID(
                DiagnosticsEngine::Error,
                "iv_module_metadata requires metadata-dir=<path>");
            compiler.getDiagnostics().Report(id);
            return false;
        }
        return true;
    }

    std::unique_ptr<ASTConsumer> CreateASTConsumer(
        CompilerInstance& compiler,
        llvm::StringRef) override
    {
        // PCH generation has no Sema instance and does not compile a module
        // translation unit. The consuming source compile runs this plugin
        // again, where it emits the required State metadata.
        if (compiler.getFrontendOpts().ProgramAction == frontend::GeneratePCH) {
            return std::make_unique<ASTConsumer>();
        }
        return std::make_unique<ModuleConsumer>(
            compiler, core_source_dir_, metadata_dir_, source_introspection_);
    }

private:
    std::filesystem::path core_source_dir_;
    std::filesystem::path metadata_dir_;
    bool source_introspection_ = true;
};

} // namespace

static clang::FrontendPluginRegistry::Add<ModulePlugin> registration(
    "iv_module_metadata",
    "instrument IV module source identities and collect state-layout metadata");
