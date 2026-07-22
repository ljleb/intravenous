#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Index/USRGeneration.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
    llvm::cl::OptionCategory tool_category("iv_clang_source_span_rewriter options");
    llvm::cl::opt<std::string> output_path(
        "output",
        llvm::cl::desc("Write the rewritten source to this path instead of rewriting in place"),
        llvm::cl::value_desc("path")
    );
    llvm::cl::opt<std::string> repo_root_path(
        "repo-root",
        llvm::cl::desc("Project root used to identify core sources (defaults to current working directory)"),
        llvm::cl::value_desc("path")
    );
    llvm::cl::opt<std::string> effective_command_source(
        "effective-command-source",
        llvm::cl::desc("Compilation-database source whose wrapper-expanded command is used"),
        llvm::cl::value_desc("path")
    );

    struct FileRange {
        clang::FileID file_id;
        unsigned begin = 0;
        unsigned end = 0;

        unsigned length() const
        {
            return end - begin;
        }
    };

    struct WrapSpec {
        unsigned wrapped_begin = 0;
        unsigned wrapped_end = 0;
        unsigned span_begin = 0;
        unsigned span_end = 0;
        std::string declaration_identity;
        std::string annotation_function;
        bool binding_wrap = false;

        bool operator==(WrapSpec const&) const = default;
    };

    struct InsertSpec {
        unsigned offset = 0;
        std::string text;

        bool operator==(InsertSpec const&) const = default;
    };

    std::string normalized_path(std::filesystem::path const& path)
    {
        return path.lexically_normal().generic_string();
    }

    std::string cxx_string_literal(std::string_view value)
    {
        std::string out;
        out.reserve(value.size() + 2);
        out.push_back('"');
        for (char c : value) {
            switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(c);
                break;
            }
        }
        out.push_back('"');
        return out;
    }

    std::optional<std::filesystem::path> configured_core_source_dir()
    {
        std::error_code ec;
        std::filesystem::path root =
            repo_root_path.empty()
                ? std::filesystem::current_path(ec)
                : std::filesystem::path(repo_root_path.getValue());

        if (ec || root.empty()) {
            return std::nullopt;
        }

        root = root.lexically_normal();
        std::filesystem::path const core = root / "intravenous";
        if (std::filesystem::exists(core, ec)) {
            return core;
        }
        return std::nullopt;
    }

    bool is_within_path(std::filesystem::path const& child, std::filesystem::path const& parent)
    {
        std::string child_norm = normalized_path(child);
        std::string parent_norm = normalized_path(parent);
        if (parent_norm.empty()) {
            return false;
        }
        if (child_norm == parent_norm) {
            return true;
        }
        if (!parent_norm.ends_with('/')) {
            parent_norm.push_back('/');
        }
        return child_norm.starts_with(parent_norm);
    }

    bool is_user_source_path(
        std::filesystem::path const& path,
        std::optional<std::filesystem::path> const& core_source_dir
    )
    {
        if (!core_source_dir.has_value()) {
            return true;
        }
        return !is_within_path(path, *core_source_dir);
    }

    bool is_reference_type(clang::QualType type)
    {
        return !type.isNull() && type->isReferenceType();
    }

    std::string source_annotation_function(clang::QualType type)
    {
        if (type.isNull()) {
            return {};
        }

        clang::QualType const stripped = type.getNonReferenceType().getUnqualifiedType();
        if (stripped.isNull()) {
            return {};
        }

        auto const* record = stripped->getAsCXXRecordDecl();
        if (!record) {
            clang::QualType const canonical = stripped.getCanonicalType();
            if (!canonical.isNull()) {
                record = canonical->getAsCXXRecordDecl();
            }
        }
        if (!record) {
            return {};
        }

        std::string const qualified_name = record->getQualifiedNameAsString();
        if (qualified_name == "iv::PublicSampleInputRef") {
            return "iv::_annotate_public_input_source_info";
        }
        if (qualified_name == "iv::PublicEventInputRef") {
            return "iv::_annotate_public_event_input_source_info";
        }
        if (qualified_name == "iv::NodeRef"
            || qualified_name == "iv::TypedNodeRef"
            || qualified_name == "iv::StructuredNodeRef") {
            return "iv::_annotate_node_source_info";
        }
        return {};
    }

    clang::Expr const* strip_trivial_expr_wrappers(clang::Expr const* expr)
    {
        while (true) {
            if (auto const* wrapped = llvm::dyn_cast<clang::ExprWithCleanups>(expr)) {
                expr = wrapped->getSubExpr();
            } else if (auto const* wrapped = llvm::dyn_cast<clang::MaterializeTemporaryExpr>(expr)) {
                expr = wrapped->getSubExpr();
            } else if (auto const* wrapped = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(expr)) {
                expr = wrapped->getSubExpr();
            } else if (auto const* wrapped = llvm::dyn_cast<clang::ImplicitCastExpr>(expr)) {
                expr = wrapped->getSubExpr();
            } else {
                return expr;
            }
        }
    }

    class SourceSpanCollector final : public clang::RecursiveASTVisitor<SourceSpanCollector> {
        clang::ASTContext& _context;
        clang::SourceManager& _source_manager;
        clang::LangOptions const& _lang_options;
        clang::FileID _main_file_id;
        std::optional<std::filesystem::path> _core_source_dir;
        std::vector<WrapSpec> _wraps;
        std::vector<InsertSpec> _insertions;

        bool is_user_source_path(llvm::StringRef path) const
        {
            if (path.empty()) {
                return false;
            }
            return ::is_user_source_path(std::filesystem::path(path.str()), _core_source_dir);
        }

        bool is_user_source_location(clang::SourceLocation location) const
        {
            if (location.isInvalid() || location.isMacroID() || _source_manager.isInSystemHeader(location)) {
                return false;
            }

            clang::SourceLocation const spelling = _source_manager.getSpellingLoc(location);
            if (_source_manager.getFileID(spelling) != _main_file_id) {
                return false;
            }
            llvm::StringRef const path = _source_manager.getFilename(spelling);
            return !path.empty() && is_user_source_path(path);
        }

    public:
        // The AST contains every declaration pulled in by the module DSL and
        // its third-party dependencies. None of those declarations can be
        // rewritten: edits are restricted to the main user-authored source
        // file. Pruning them before RecursiveASTVisitor descends into their
        // subtrees avoids walking the full header AST on every hot reload.
        bool TraverseDecl(clang::Decl* decl)
        {
            if (!decl || llvm::isa<clang::TranslationUnitDecl>(decl)) {
                return clang::RecursiveASTVisitor<SourceSpanCollector>::TraverseDecl(decl);
            }

            if (!is_user_source_location(decl->getLocation())) {
                return true;
            }

            return clang::RecursiveASTVisitor<SourceSpanCollector>::TraverseDecl(decl);
        }

    private:
        std::optional<FileRange> file_range_for_source_range(clang::SourceRange range) const
        {
            clang::SourceLocation const begin_loc = _source_manager.getSpellingLoc(range.getBegin());
            clang::SourceLocation const end_begin = _source_manager.getSpellingLoc(range.getEnd());
            clang::SourceLocation const end_loc =
                clang::Lexer::getLocForEndOfToken(end_begin, 0, _source_manager, _lang_options);

            if (
                begin_loc.isInvalid()
                || end_loc.isInvalid()
                || begin_loc.isMacroID()
                || end_loc.isMacroID()
                || _source_manager.getFileID(begin_loc) != _source_manager.getFileID(end_loc)
                || _source_manager.getFileID(begin_loc) != _main_file_id
            ) {
                return std::nullopt;
            }

            llvm::StringRef const path = _source_manager.getFilename(begin_loc);
            if (path.empty() || !is_user_source_path(path)) {
                return std::nullopt;
            }

            return FileRange {
                .file_id = _source_manager.getFileID(begin_loc),
                .begin = _source_manager.getFileOffset(begin_loc),
                .end = _source_manager.getFileOffset(end_loc),
            };
        }

        std::optional<FileRange> token_range_for_location(clang::SourceLocation location) const
        {
            clang::SourceLocation const begin_loc = _source_manager.getSpellingLoc(location);
            clang::SourceLocation const end_loc =
                clang::Lexer::getLocForEndOfToken(begin_loc, 0, _source_manager, _lang_options);

            if (
                begin_loc.isInvalid()
                || end_loc.isInvalid()
                || begin_loc.isMacroID()
                || end_loc.isMacroID()
                || _source_manager.getFileID(begin_loc) != _source_manager.getFileID(end_loc)
                || _source_manager.getFileID(begin_loc) != _main_file_id
            ) {
                return std::nullopt;
            }

            llvm::StringRef const path = _source_manager.getFilename(begin_loc);
            if (path.empty() || !is_user_source_path(path)) {
                return std::nullopt;
            }

            return FileRange {
                .file_id = _source_manager.getFileID(begin_loc),
                .begin = _source_manager.getFileOffset(begin_loc),
                .end = _source_manager.getFileOffset(end_loc),
            };
        }

        std::string declaration_identity_for(clang::Decl const* decl) const
        {
            if (!decl) {
                return {};
            }

            llvm::SmallString<256> usr;
            auto const* canonical = decl->getCanonicalDecl();
            if (clang::index::generateUSRForDecl(canonical, usr, _lang_options)) {
                return {};
            }
            return std::string(usr.str());
        }

        std::string declaration_identity_for_named_expr(clang::Expr const* expr) const
        {
            if (!expr) {
                return {};
            }

            if (auto const* decl_ref = llvm::dyn_cast<clang::DeclRefExpr>(expr)) {
                return declaration_identity_for(decl_ref->getDecl());
            }

            if (auto const* member = llvm::dyn_cast<clang::MemberExpr>(expr)) {
                return declaration_identity_for(member->getMemberDecl());
            }

            return {};
        }

        void add_wrap(
            FileRange wrapped,
            FileRange span,
            std::string declaration_identity,
            std::string annotation_function,
            bool binding_wrap
        )
        {
            if (
                wrapped.file_id != span.file_id
                || wrapped.begin >= wrapped.end
                || span.begin >= span.end
            ) {
                return;
            }

            _wraps.push_back(WrapSpec {
                .wrapped_begin = wrapped.begin,
                .wrapped_end = wrapped.end,
                .span_begin = span.begin,
                .span_end = span.end,
                .declaration_identity = std::move(declaration_identity),
                .annotation_function = std::move(annotation_function),
                .binding_wrap = binding_wrap,
            });
        }

        bool is_instrumentation_call(clang::Expr const* expr) const
        {
            auto const* call = llvm::dyn_cast<clang::CallExpr>(expr);
            if (!call) {
                return false;
            }

            auto const* callee = call->getDirectCallee();
            if (!callee) {
                return false;
            }
            auto const name = callee->getQualifiedNameAsString();
            return name == "iv::_annotate_node_source_info"
                || name == "iv::_annotate_public_input_source_info"
                || name == "iv::_annotate_public_event_input_source_info";
        }

        bool is_std_move_argument_context(clang::Expr const* expr) const
        {
            clang::Expr const* current = expr;

            while (true) {
                auto const parents = _context.getParents(*current);
                if (parents.empty()) {
                    return false;
                }

                auto const* parent_stmt = parents[0].get<clang::Stmt>();
                if (!parent_stmt) {
                    return false;
                }

                if (auto const* parent_expr = llvm::dyn_cast<clang::Expr>(parent_stmt)) {
                    if (
                        llvm::isa<clang::ExprWithCleanups>(parent_expr)
                        || llvm::isa<clang::MaterializeTemporaryExpr>(parent_expr)
                        || llvm::isa<clang::CXXBindTemporaryExpr>(parent_expr)
                        || llvm::isa<clang::ImplicitCastExpr>(parent_expr)
                        || llvm::isa<clang::ParenExpr>(parent_expr)
                    ) {
                        current = parent_expr;
                        continue;
                    }
                }

                auto const* call = llvm::dyn_cast<clang::CallExpr>(parent_stmt);
                if (!call) {
                    return false;
                }

                auto const* callee = call->getDirectCallee();
                return callee && callee->getQualifiedNameAsString() == "std::move";
            }
        }

        bool is_assignment_lhs_context(clang::Expr const* expr) const
        {
            clang::Expr const* current = expr;

            while (true) {
                auto const parents = _context.getParents(*current);
                if (parents.empty()) {
                    return false;
                }

                auto const* parent_stmt = parents[0].get<clang::Stmt>();
                if (!parent_stmt) {
                    return false;
                }

                if (auto const* parent_expr = llvm::dyn_cast<clang::Expr>(parent_stmt)) {
                    if (
                        llvm::isa<clang::ExprWithCleanups>(parent_expr)
                        || llvm::isa<clang::MaterializeTemporaryExpr>(parent_expr)
                        || llvm::isa<clang::CXXBindTemporaryExpr>(parent_expr)
                        || llvm::isa<clang::ImplicitCastExpr>(parent_expr)
                        || llvm::isa<clang::ParenExpr>(parent_expr)
                    ) {
                        current = parent_expr;
                        continue;
                    }
                }

                if (auto const* binary = llvm::dyn_cast<clang::BinaryOperator>(parent_stmt)) {
                    return binary->isAssignmentOp()
                        && strip_trivial_expr_wrappers(binary->getLHS()) == strip_trivial_expr_wrappers(current);
                }

                if (auto const* overloaded = llvm::dyn_cast<clang::CXXOperatorCallExpr>(parent_stmt)) {
                    return overloaded->getOperator() == clang::OO_Equal
                        && overloaded->getNumArgs() >= 1
                        && strip_trivial_expr_wrappers(overloaded->getArg(0)) == strip_trivial_expr_wrappers(current);
                }

                return false;
            }
        }

        std::optional<FileRange> token_range_for_named_expr(clang::Expr const* expr) const
        {
            if (!expr) {
                return std::nullopt;
            }

            if (auto const* decl_ref = llvm::dyn_cast<clang::DeclRefExpr>(expr)) {
                return token_range_for_location(decl_ref->getLocation());
            }

            if (auto const* member = llvm::dyn_cast<clang::MemberExpr>(expr)) {
                return token_range_for_location(member->getMemberLoc());
            }

            return std::nullopt;
        }

        void maybe_add_symbol_reference_wrap(clang::Expr* expr)
        {
            if (!expr) {
                return;
            }

            auto const annotation_function = source_annotation_function(expr->getType());
            if (annotation_function.empty()) {
                return;
            }

            if (strip_trivial_expr_wrappers(expr) != expr) {
                return;
            }

            if (!is_user_source_location(expr->getBeginLoc()) || is_instrumentation_call(expr)) {
                return;
            }

            if (is_assignment_lhs_context(expr)) {
                return;
            }

            if (is_std_move_argument_context(expr)) {
                return;
            }

            auto const wrapped_range = file_range_for_source_range(expr->getSourceRange());
            auto const span_range = token_range_for_named_expr(expr);
            auto const declaration_identity = declaration_identity_for_named_expr(expr);
            if (!wrapped_range.has_value() || !span_range.has_value() || declaration_identity.empty()) {
                return;
            }

            add_wrap(*wrapped_range, *span_range, declaration_identity, annotation_function, false);
        }

        void maybe_add_binding_wrap(clang::VarDecl* decl)
        {
            if (!decl) {
                return;
            }

            clang::Expr const* init = decl->getInit();
            if (!init) {
                return;
            }

            clang::SourceLocation const decl_loc = _source_manager.getSpellingLoc(decl->getLocation());
            clang::SourceLocation const init_begin = _source_manager.getSpellingLoc(init->getBeginLoc());

            clang::QualType const decl_type = decl->getType();
            auto const annotation_function = source_annotation_function(decl_type);
            if (
                !decl->isLocalVarDecl()
                || !decl->hasInit()
                || init_begin.isInvalid()
                || decl_loc == init_begin
                || is_reference_type(decl_type)
                || annotation_function.empty()
                || !is_user_source_location(decl->getLocation())
            ) {
                return;
            }

            auto const wrapped_range = file_range_for_source_range(init->getSourceRange());
            auto const span_range = token_range_for_location(decl->getLocation());
            auto const declaration_identity = declaration_identity_for(decl);
            if (!wrapped_range.has_value() || !span_range.has_value() || declaration_identity.empty()) {
                return;
            }

            add_wrap(*wrapped_range, *span_range, declaration_identity, annotation_function, true);
        }

        void maybe_add_empty_declaration_init(clang::VarDecl* decl)
        {
            if (!decl) {
                return;
            }

            clang::QualType const decl_type = decl->getType();
            if (
                !decl->isLocalVarDecl()
                || decl->hasInit()
                || is_reference_type(decl_type)
                || source_annotation_function(decl_type) != "iv::_annotate_node_source_info"
                || !is_user_source_location(decl->getLocation())
            ) {
                return;
            }

            auto const name_range = token_range_for_location(decl->getLocation());
            auto const declaration_identity = declaration_identity_for(decl);
            if (!name_range.has_value() || declaration_identity.empty()) {
                return;
            }

            _insertions.push_back(InsertSpec {
                .offset = name_range->end,
                .text = " { iv::logical_empty_tag, " + cxx_string_literal(declaration_identity) + " }",
            });
        }

        void maybe_add_assignment_wrap(clang::Expr* lhs, clang::Expr* rhs)
        {
            if (!lhs || !rhs) {
                return;
            }

            clang::QualType const lhs_type = lhs->getType();
            auto const annotation_function = source_annotation_function(lhs_type);
            if (
                annotation_function.empty()
                || !is_user_source_location(lhs->getBeginLoc())
            ) {
                return;
            }

            auto const wrapped_range = file_range_for_source_range(rhs->getSourceRange());
            auto const span_range = token_range_for_named_expr(lhs);
            auto const declaration_identity = declaration_identity_for_named_expr(lhs);
            if (!wrapped_range.has_value() || !span_range.has_value() || declaration_identity.empty()) {
                return;
            }

            add_wrap(*wrapped_range, *span_range, declaration_identity, annotation_function, true);
        }

        static void deduplicate_wraps(std::vector<WrapSpec>& wraps)
        {
            std::sort(wraps.begin(), wraps.end(), [](WrapSpec const& a, WrapSpec const& b) {
                return std::tie(a.wrapped_begin, a.wrapped_end, a.span_begin, a.span_end, a.declaration_identity, a.annotation_function, a.binding_wrap)
                    < std::tie(b.wrapped_begin, b.wrapped_end, b.span_begin, b.span_end, b.declaration_identity, b.annotation_function, b.binding_wrap);
            });
            wraps.erase(std::unique(wraps.begin(), wraps.end()), wraps.end());
        }

        static void deduplicate_insertions(std::vector<InsertSpec>& insertions)
        {
            std::sort(insertions.begin(), insertions.end(), [](InsertSpec const& a, InsertSpec const& b) {
                return std::tie(a.offset, a.text) < std::tie(b.offset, b.text);
            });
            insertions.erase(std::unique(insertions.begin(), insertions.end()), insertions.end());
        }

        static std::string render_with_wraps(
            std::string_view input,
            std::vector<WrapSpec> wraps,
            std::vector<InsertSpec> insertions,
            std::string_view file_path
        )
        {
            deduplicate_wraps(wraps);
            deduplicate_insertions(insertions);
            std::string const encoded_file_path = cxx_string_literal(file_path);

            std::unordered_map<unsigned, std::vector<WrapSpec const*>> begin_events;
            std::unordered_map<unsigned, std::vector<WrapSpec const*>> end_events;
            std::unordered_map<unsigned, std::vector<InsertSpec const*>> insert_events;
            begin_events.reserve(wraps.size());
            end_events.reserve(wraps.size());
            for (auto const& wrap : wraps) {
                begin_events[wrap.wrapped_begin].push_back(&wrap);
                end_events[wrap.wrapped_end].push_back(&wrap);
            }
            insert_events.reserve(insertions.size());
            for (auto const& insertion : insertions) {
                insert_events[insertion.offset].push_back(&insertion);
            }

            auto const begin_order = [](WrapSpec const* a, WrapSpec const* b) {
                if (a->wrapped_end - a->wrapped_begin != b->wrapped_end - b->wrapped_begin) {
                    return (a->wrapped_end - a->wrapped_begin) > (b->wrapped_end - b->wrapped_begin);
                }
                if (a->binding_wrap != b->binding_wrap) {
                    return a->binding_wrap && !b->binding_wrap;
                }
                return std::tie(a->span_begin, a->span_end) < std::tie(b->span_begin, b->span_end);
            };

            auto const end_order = [](WrapSpec const* a, WrapSpec const* b) {
                if (a->wrapped_end - a->wrapped_begin != b->wrapped_end - b->wrapped_begin) {
                    return (a->wrapped_end - a->wrapped_begin) < (b->wrapped_end - b->wrapped_begin);
                }
                if (a->binding_wrap != b->binding_wrap) {
                    return !a->binding_wrap && b->binding_wrap;
                }
                return std::tie(a->span_begin, a->span_end) < std::tie(b->span_begin, b->span_end);
            };

            std::string output;
            output.reserve(input.size() + wraps.size() * 48);

            for (unsigned offset = 0; offset <= input.size(); ++offset) {
                if (auto it = end_events.find(offset); it != end_events.end()) {
                    auto events = it->second;
                    std::sort(events.begin(), events.end(), end_order);
                    for (WrapSpec const* wrap : events) {
                        output += ", ";
                        output += cxx_string_literal(wrap->declaration_identity);
                        output += ", ";
                        output += encoded_file_path;
                        output += ", ";
                        output += std::to_string(wrap->span_begin);
                        output += ", ";
                        output += std::to_string(wrap->span_end);
                        output += ")";
                    }
                }

                if (auto it = begin_events.find(offset); it != begin_events.end()) {
                    auto events = it->second;
                    std::sort(events.begin(), events.end(), begin_order);
                    for (WrapSpec const* wrap : events) {
                        (void)wrap;
                        output += wrap->annotation_function;
                        output += "(";
                    }
                }

                if (auto it = insert_events.find(offset); it != insert_events.end()) {
                    auto events = it->second;
                    std::sort(events.begin(), events.end(), [](InsertSpec const* a, InsertSpec const* b) {
                        return a->text < b->text;
                    });
                    for (InsertSpec const* insertion : events) {
                        output += insertion->text;
                    }
                }

                if (offset < input.size()) {
                    output.push_back(input[offset]);
                }
            }

            return output;
        }

    public:
        explicit SourceSpanCollector(clang::ASTContext& context) :
            _context(context),
            _source_manager(context.getSourceManager()),
            _lang_options(context.getLangOpts()),
            _main_file_id(_source_manager.getMainFileID())
        {
            _core_source_dir = configured_core_source_dir();
        }

        bool VisitVarDecl(clang::VarDecl* decl)
        {
            maybe_add_binding_wrap(decl);
            maybe_add_empty_declaration_init(decl);
            return true;
        }

        bool VisitDeclRefExpr(clang::DeclRefExpr* expr)
        {
            maybe_add_symbol_reference_wrap(expr);
            return true;
        }

        bool VisitMemberExpr(clang::MemberExpr* expr)
        {
            maybe_add_symbol_reference_wrap(expr);
            return true;
        }

        bool VisitBinaryOperator(clang::BinaryOperator* op)
        {
            if (op && op->isAssignmentOp()) {
                maybe_add_assignment_wrap(op->getLHS(), op->getRHS());
            }
            return true;
        }

        bool VisitCXXOperatorCallExpr(clang::CXXOperatorCallExpr* op)
        {
            if (!op) {
                return true;
            }

            if (op->getOperator() == clang::OO_Equal && op->getNumArgs() >= 2) {
                maybe_add_assignment_wrap(op->getArg(0), op->getArg(1));
            }
            return true;
        }

        bool write_changes()
        {
            bool invalid = false;
            llvm::StringRef const buffer = _source_manager.getBufferData(_main_file_id, &invalid);
            if (invalid) {
                return false;
            }

            llvm::StringRef const path_ref =
                _source_manager.getFilename(_source_manager.getLocForStartOfFile(_main_file_id));
            if (path_ref.empty()) {
                return false;
            }

            bool const write_in_place = output_path.empty();
            std::string const normalized_main_path = normalized_path(std::filesystem::path(path_ref.str()));
            std::string const rewritten =
                (_wraps.empty() && _insertions.empty())
                    ? buffer.str()
                    : render_with_wraps(buffer.str(), _wraps, _insertions, normalized_main_path);
            if (write_in_place && rewritten == buffer) {
                return false;
            }

            std::filesystem::path const path =
                write_in_place
                    ? std::filesystem::path(path_ref.str())
                    : std::filesystem::path(output_path.getValue());
            if (path.has_parent_path()) {
                std::filesystem::create_directories(path.parent_path());
            }
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out) {
                llvm::errs() << "failed to rewrite " << path.string() << "\n";
                return false;
            }
            out << rewritten;
            return true;
        }
    };

    class RewriteConsumer final : public clang::ASTConsumer {
        clang::CompilerInstance& _compiler;

    public:
        explicit RewriteConsumer(clang::CompilerInstance& compiler) :
            _compiler(compiler)
        {}

        void HandleTranslationUnit(clang::ASTContext& context) override
        {
            SourceSpanCollector collector(context);
            collector.TraverseDecl(context.getTranslationUnitDecl());
            if (!collector.write_changes()) {
                return;
            }

            clang::SourceManager const& source_manager = _compiler.getSourceManager();
            llvm::StringRef const input =
                source_manager.getFilename(source_manager.getLocForStartOfFile(source_manager.getMainFileID()));
            llvm::errs() << "rewrote " << input << " -> "
                         << (output_path.empty() ? input : llvm::StringRef(output_path.getValue()))
                         << "\n";
        }
    };

    class RewriteAction final : public clang::ASTFrontendAction {
    public:
        std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
            clang::CompilerInstance& compiler,
            llvm::StringRef /*in_file*/
        ) override
        {
            return std::make_unique<RewriteConsumer>(compiler);
        }
    };

    std::optional<std::vector<std::string>> wrapper_expanded_cc1_arguments(
        clang::tooling::CompileCommand const& command,
        std::string const& input_source
    )
    {
        if (command.CommandLine.empty()) {
            return std::nullopt;
        }

        std::filesystem::path const trace_path =
            std::filesystem::temp_directory_path()
            / ("iv-source-span-rewriter-" + std::to_string(std::hash<std::string> {}(
                command.CommandLine.front() + command.Filename
            )) + ".trace");
        std::string const trace_path_string = trace_path.string();

        std::vector<llvm::StringRef> args;
        args.reserve(command.CommandLine.size() + 1);
        for (std::string const& arg : command.CommandLine) {
            // The generated source does not exist until this rewriter runs.
            // Expand the consumer command against the real input source, while
            // retaining every compile option (including CMake's PCH flags).
            args.emplace_back(arg == command.Filename ? input_source : arg);
        }
        args.emplace_back("-###");

        std::array<std::optional<llvm::StringRef>, 3> redirects {
            std::nullopt,
            std::nullopt,
            llvm::StringRef(trace_path_string),
        };
        std::string execution_error;
        int const exit_code = llvm::sys::ExecuteAndWait(
            command.CommandLine.front(),
            args,
            std::nullopt,
            redirects,
            0,
            0,
            &execution_error
        );
        std::ifstream trace(trace_path);
        std::string const output((std::istreambuf_iterator<char>(trace)), std::istreambuf_iterator<char>());
        std::error_code ignored;
        std::filesystem::remove(trace_path, ignored);
        if (exit_code != 0) {
            llvm::errs() << "failed to expand compiler wrapper (exit " << exit_code << "): "
                         << execution_error << "\n"
                         << output;
            return std::nullopt;
        }

        // Clang's -### output shell-quotes every argument, so look for the
        // token without assuming surrounding whitespace or quote style.
        std::string_view const marker = "-cc1";
        std::size_t const marker_pos = output.rfind(marker);
        if (marker_pos == std::string::npos) {
            llvm::errs() << "compiler wrapper did not report a Clang cc1 command:\n" << output;
            return std::nullopt;
        }
        std::size_t const line_begin = output.rfind('\n', marker_pos);
        llvm::StringRef const line = llvm::StringRef(output).substr(
            line_begin == std::string::npos ? 0 : line_begin + 1
        );

        llvm::BumpPtrAllocator allocator;
        llvm::StringSaver saver(allocator);
        llvm::SmallVector<char const*, 128> tokens;
        llvm::cl::TokenizeGNUCommandLine(line, saver, tokens);
        auto cc1 = std::find_if(tokens.begin(), tokens.end(), [](char const* token) {
            return std::string_view(token) == "-cc1";
        });
        if (cc1 == tokens.end()) {
            llvm::errs() << "could not parse the Clang cc1 command\n";
            return std::nullopt;
        }

        std::vector<std::string> expanded;
        expanded.reserve(static_cast<std::size_t>(tokens.end() - cc1 - 1));
        for (++cc1; cc1 != tokens.end(); ++cc1) {
            expanded.emplace_back(*cc1);
        }
        return expanded;
    }

    int run_with_effective_command(
        clang::tooling::CompilationDatabase const& database,
        std::string const& command_source,
        std::string const& input_source
    )
    {
        auto const commands = database.getCompileCommands(command_source);
        if (commands.empty()) {
            llvm::errs() << "no compilation command for " << command_source << "\n";
            return 1;
        }
        auto args = wrapper_expanded_cc1_arguments(commands.front(), input_source);
        if (!args) {
            return 1;
        }

        auto diagnostic_options = std::make_shared<clang::DiagnosticOptions>();
        clang::DiagnosticsEngine diagnostics(
            llvm::makeIntrusiveRefCnt<clang::DiagnosticIDs>(),
            *diagnostic_options
        );
        auto invocation = std::make_shared<clang::CompilerInvocation>();
        std::vector<char const*> arg_pointers;
        arg_pointers.reserve(args->size());
        for (std::string const& arg : *args) {
            arg_pointers.push_back(arg.c_str());
        }
        if (!clang::CompilerInvocation::CreateFromArgs(*invocation, arg_pointers, diagnostics)) {
            return 1;
        }

        auto& frontend_options = invocation->getFrontendOpts();
        frontend_options.Inputs.clear();
        frontend_options.Inputs.emplace_back(input_source, clang::InputKind(clang::Language::CXX));
        frontend_options.OutputFile.clear();
        frontend_options.ProgramAction = clang::frontend::ParseSyntaxOnly;

        clang::CompilerInstance compiler(std::move(invocation));
        compiler.createDiagnostics();
        if (!compiler.hasDiagnostics()) {
            return 1;
        }
        compiler.createFileManager();
        compiler.createSourceManager();
        RewriteAction action;
        return compiler.ExecuteAction(action) ? 0 : 1;
    }
}

int main(int argc, char** argv)
{
    std::vector<char const*> argv_view(argv, argv + argc);
    auto expected_parser = clang::tooling::CommonOptionsParser::create(argc, argv_view.data(), tool_category);
    if (!expected_parser) {
        llvm::errs() << llvm::toString(expected_parser.takeError()) << "\n";
        return 1;
    }

    clang::tooling::CommonOptionsParser& options = expected_parser.get();
    if (!effective_command_source.empty()) {
        if (options.getSourcePathList().size() != 1) {
            llvm::errs() << "--effective-command-source requires exactly one input source\n";
            return 1;
        }
        return run_with_effective_command(
            options.getCompilations(),
            effective_command_source,
            options.getSourcePathList().front()
        );
    }
    clang::tooling::ClangTool tool(options.getCompilations(), options.getSourcePathList());
    return tool.run(clang::tooling::newFrontendActionFactory<RewriteAction>().get());
}
