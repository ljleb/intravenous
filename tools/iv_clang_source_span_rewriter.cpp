#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
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
        bool binding_wrap = false;

        bool operator==(WrapSpec const&) const = default;
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

    bool is_source_span_ref_like(clang::QualType type)
    {
        if (type.isNull()) {
            return false;
        }

        clang::QualType const stripped = type.getNonReferenceType().getUnqualifiedType();
        if (stripped.isNull()) {
            return false;
        }

        auto const* record = stripped->getAsCXXRecordDecl();
        if (!record) {
            clang::QualType const canonical = stripped.getCanonicalType();
            if (!canonical.isNull()) {
                record = canonical->getAsCXXRecordDecl();
            }
        }
        if (!record) {
            return false;
        }

        std::string const qualified_name = record->getQualifiedNameAsString();
        return qualified_name == "iv::NodeRef"
            || qualified_name == "iv::SamplePortRef"
            || qualified_name == "iv::TypedNodeRef"
            || qualified_name == "iv::StructuredNodeRef";
    }

    bool should_wrap_overloaded_operator(clang::OverloadedOperatorKind op)
    {
        switch (op) {
        case clang::OO_Call:
        case clang::OO_Subscript:
        case clang::OO_LessLess:
        case clang::OO_GreaterGreater:
            return true;
        default:
            return false;
        }
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

        void add_wrap(FileRange wrapped, FileRange span, bool binding_wrap)
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
            return callee && callee->getQualifiedNameAsString() == "iv::_add_node_source_span";
        }

        bool is_excluded_lvalue_context(clang::Expr const* expr) const
        {
            clang::Expr const* current = expr;

            while (true) {
                auto const parents = _context.getParents(*current);
                if (parents.empty()) {
                    return false;
                }

                if (auto const* parent_decl = parents[0].get<clang::VarDecl>()) {
                    if (!parent_decl->hasInit()) {
                        return true;
                    }

                    clang::Expr const* init = parent_decl->getInit();
                    if (!init) {
                        return true;
                    }

                    clang::SourceLocation const decl_loc =
                        _source_manager.getSpellingLoc(parent_decl->getLocation());
                    clang::SourceLocation const init_begin =
                        _source_manager.getSpellingLoc(init->getBeginLoc());
                    if (!decl_loc.isInvalid() && !init_begin.isInvalid() && decl_loc == init_begin) {
                        return true;
                    }

                    return is_reference_type(parent_decl->getType())
                        && parent_decl->hasInit()
                        && strip_trivial_expr_wrappers(init) == strip_trivial_expr_wrappers(current);
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

        void maybe_add_expr_wrap(clang::Expr* expr)
        {
            if (!expr) {
                return;
            }

            if (llvm::isa<clang::BinaryOperator>(expr)) {
                return;
            }
            if (auto const* overloaded = llvm::dyn_cast<clang::CXXOperatorCallExpr>(expr)) {
                if (!should_wrap_overloaded_operator(overloaded->getOperator())) {
                    return;
                }
            }

            clang::QualType const expr_type = expr->getType();
            if (!is_source_span_ref_like(expr_type)) {
                return;
            }

            if (strip_trivial_expr_wrappers(expr) != expr) {
                return;
            }

            if (!is_user_source_location(expr->getBeginLoc()) || is_instrumentation_call(expr)) {
                return;
            }

            if (is_excluded_lvalue_context(expr)) {
                return;
            }

            auto const range = file_range_for_source_range(expr->getSourceRange());
            if (!range.has_value()) {
                return;
            }

            add_wrap(*range, *range, false);
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
            if (
                !decl->isLocalVarDecl()
                || !decl->hasInit()
                || init_begin.isInvalid()
                || decl_loc == init_begin
                || is_reference_type(decl_type)
                || !is_source_span_ref_like(decl_type)
                || !is_user_source_location(decl->getLocation())
            ) {
                return;
            }

            auto const wrapped_range = file_range_for_source_range(init->getSourceRange());
            auto const span_range = token_range_for_location(decl->getLocation());
            if (!wrapped_range.has_value() || !span_range.has_value()) {
                return;
            }

            add_wrap(*wrapped_range, *span_range, true);
        }

        void maybe_add_assignment_wrap(clang::Expr* lhs, clang::Expr* rhs)
        {
            if (!lhs || !rhs) {
                return;
            }

            clang::QualType const lhs_type = lhs->getType();
            if (
                !is_source_span_ref_like(lhs_type)
                || !is_user_source_location(lhs->getBeginLoc())
            ) {
                return;
            }

            auto const wrapped_range = file_range_for_source_range(rhs->getSourceRange());
            auto const span_range = file_range_for_source_range(lhs->getSourceRange());
            if (!wrapped_range.has_value() || !span_range.has_value()) {
                return;
            }

            add_wrap(*wrapped_range, *span_range, true);
        }

        static void deduplicate_wraps(std::vector<WrapSpec>& wraps)
        {
            std::sort(wraps.begin(), wraps.end(), [](WrapSpec const& a, WrapSpec const& b) {
                return std::tie(a.wrapped_begin, a.wrapped_end, a.span_begin, a.span_end, a.binding_wrap)
                    < std::tie(b.wrapped_begin, b.wrapped_end, b.span_begin, b.span_end, b.binding_wrap);
            });
            wraps.erase(std::unique(wraps.begin(), wraps.end()), wraps.end());
        }

        static std::string render_with_wraps(
            std::string_view input,
            std::vector<WrapSpec> wraps,
            std::string_view file_path
        )
        {
            deduplicate_wraps(wraps);
            std::string const encoded_file_path = cxx_string_literal(file_path);

            std::unordered_map<unsigned, std::vector<WrapSpec const*>> begin_events;
            std::unordered_map<unsigned, std::vector<WrapSpec const*>> end_events;
            begin_events.reserve(wraps.size());
            end_events.reserve(wraps.size());
            for (auto const& wrap : wraps) {
                begin_events[wrap.wrapped_begin].push_back(&wrap);
                end_events[wrap.wrapped_end].push_back(&wrap);
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
                        output += "iv::_add_node_source_span(";
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

        bool VisitExpr(clang::Expr* expr)
        {
            maybe_add_expr_wrap(expr);
            return true;
        }

        bool VisitVarDecl(clang::VarDecl* decl)
        {
            maybe_add_binding_wrap(decl);
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
            if (op && op->getOperator() == clang::OO_Equal && op->getNumArgs() >= 2) {
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
                _wraps.empty()
                    ? buffer.str()
                    : render_with_wraps(buffer.str(), _wraps, normalized_main_path);
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
            llvm::errs() << "rewrote "
                         << source_manager.getFilename(source_manager.getLocForStartOfFile(source_manager.getMainFileID()))
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
    clang::tooling::ClangTool tool(options.getCompilations(), options.getSourcePathList());
    return tool.run(clang::tooling::newFrontendActionFactory<RewriteAction>().get());
}
