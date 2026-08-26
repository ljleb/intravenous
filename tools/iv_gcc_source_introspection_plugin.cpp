#include "gcc-plugin.h"
#include "plugin-version.h"
#include "tree.h"
#include "tree-iterator.h"
#include "print-tree.h"
#include "langhooks.h"
#include "cp/cp-tree.h"
#include "c-family/c-common.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

int plugin_is_GPL_compatible;

namespace {
struct SourceSpan {
    std::string file_path;
    std::uint32_t begin = 0;
    std::uint32_t end = 0;

    bool operator==(SourceSpan const& other) const
    {
        return file_path == other.file_path
            && begin == other.begin
            && end == other.end;
    }
};

struct SourceFile {
    std::string path;
    std::string text;
    std::vector<std::size_t> line_offsets;
};

struct RefAnnotation {
    tree value = nullptr;
    tree declaration = nullptr;
    bool value_is_pointer = false;
    std::string declaration_identity;
    SourceSpan span;
};

struct PublicOutputAnnotation {
    tree builder_pointer = nullptr;
    bool event = false;
    std::size_t ordinal = 0;
    SourceSpan span;
};

struct StatementAnalysis {
    std::vector<RefAnnotation> refs;
    std::vector<PublicOutputAnnotation> public_outputs;
};

std::filesystem::path core_source_dir;
std::string debug_function;
std::unordered_map<std::string, SourceFile> source_files;
std::unordered_map<std::string, SourceSpan> graph_local_declarations;
std::unordered_set<tree> transformed_bodies;

tree source_annotation_template;
tree public_output_annotation_function;

std::filesystem::path normalized_path(std::filesystem::path path)
{
    std::error_code error;
    auto normalized = std::filesystem::weakly_canonical(path, error);
    if (!error)
        return normalized;
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
        if (child_it == child.end() || *child_it != *parent_it)
            return false;
    }
    return true;
}

std::string identifier(tree declaration)
{
    if (!declaration || !DECL_P(declaration) || !DECL_NAME(declaration))
        return {};
    return IDENTIFIER_POINTER(DECL_NAME(declaration));
}

tree called_function(tree expression)
{
    if (!expression)
        return nullptr;
    if (tree const call = extract_call_expr(expression))
        expression = call;

    tree function;
    if (TREE_CODE(expression) == CALL_EXPR)
        function = CALL_EXPR_FN(expression);
    else if (TREE_CODE(expression) == AGGR_INIT_EXPR)
        function = AGGR_INIT_EXPR_FN(expression);
    else
        return nullptr;

    while (function && (TREE_CODE(function) == ADDR_EXPR
                        || TREE_CODE(function) == NOP_EXPR
                        || TREE_CODE(function) == CONVERT_EXPR))
        function = TREE_OPERAND(function, 0);
    return function && TREE_CODE(function) == FUNCTION_DECL
        ? function
        : nullptr;
}

bool function_is_named(tree function, std::string_view name)
{
    return function && identifier(function) == name;
}

tree context_of(tree value)
{
    if (!value)
        return nullptr;
    if (DECL_P(value))
        return DECL_CONTEXT(value);
    if (TYPE_P(value))
        return TYPE_CONTEXT(value);
    return nullptr;
}

tree enclosing_nonlambda_function(tree declaration)
{
    for (tree context = context_of(declaration); context;
         context = context_of(context)) {
        if (TREE_CODE(context) == FUNCTION_DECL
            && !LAMBDA_FUNCTION_P(context))
            return context;
    }
    return nullptr;
}

std::string declaration_identity(tree declaration)
{
    auto const name = identifier(declaration);
    if (TREE_CODE(declaration) == VAR_DECL && !name.empty()) {
        if (tree const function = enclosing_nonlambda_function(declaration)) {
            tree const assembler_name = DECL_ASSEMBLER_NAME(function);
            if (assembler_name)
                return std::string(IDENTIFIER_POINTER(assembler_name))
                    + "@" + name;
        }
    }

    if (auto const* printable = lang_hooks.decl_printable_name(
            declaration, 2))
        return printable;
    return name;
}

tree unqualified_object_type(tree type)
{
    if (!type)
        return nullptr;
    while (TREE_CODE(type) == REFERENCE_TYPE)
        type = TREE_TYPE(type);
    return TYPE_MAIN_VARIANT(type);
}

std::string record_type_name(tree type)
{
    type = unqualified_object_type(type);
    if (!type)
        return {};
    tree name = TYPE_NAME(type);
    if (name && TREE_CODE(name) == TYPE_DECL)
        name = DECL_NAME(name);
    return name && TREE_CODE(name) == IDENTIFIER_NODE
        ? IDENTIFIER_POINTER(name)
        : std::string {};
}

enum class AnnotatableRefKind {
    none,
    node,
    public_sample_input,
    public_event_input,
};

AnnotatableRefKind annotatable_ref_kind(tree type)
{
    auto const name = record_type_name(type);
    if (name == "NodeRef" || name == "TypedNodeRef")
        return AnnotatableRefKind::node;
    if (name == "PublicSampleInputRef")
        return AnnotatableRefKind::public_sample_input;
    if (name == "PublicEventInputRef")
        return AnnotatableRefKind::public_event_input;
    return AnnotatableRefKind::none;
}

bool is_user_source_location(location_t location)
{
    if (location == UNKNOWN_LOCATION || in_system_header_at(location))
        return false;
    auto const expanded = expand_location_to_spelling_point(location);
    if (!expanded.file || expanded.line <= 0 || expanded.column <= 0)
        return false;
    return !path_is_within(
        normalized_path(expanded.file), core_source_dir);
}

SourceFile const* source_file(std::string_view path)
{
    auto const normalized = normalized_path(path).generic_string();
    if (auto const found = source_files.find(normalized);
        found != source_files.end())
        return &found->second;

    std::ifstream input(normalized, std::ios::binary);
    if (!input)
        return nullptr;
    SourceFile file {
        .path = normalized,
        .text = {
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>(),
        },
        .line_offsets = {0},
    };
    for (std::size_t i = 0; i < file.text.size(); ++i) {
        if (file.text[i] == '\n')
            file.line_offsets.push_back(i + 1);
    }
    return &source_files.emplace(normalized, std::move(file)).first->second;
}

std::optional<std::size_t> source_offset(
    SourceFile const& file,
    expanded_location location)
{
    if (location.line <= 0 || location.column <= 0
        || static_cast<std::size_t>(location.line) > file.line_offsets.size())
        return std::nullopt;
    auto const offset = file.line_offsets[location.line - 1]
        + static_cast<std::size_t>(location.column - 1);
    if (offset > file.text.size())
        return std::nullopt;
    return offset;
}

std::optional<SourceSpan> source_span(source_range range)
{
    if (range.m_start == UNKNOWN_LOCATION
        || range.m_finish == UNKNOWN_LOCATION
        || !is_user_source_location(range.m_start))
        return std::nullopt;
    auto const start = expand_location_to_spelling_point(range.m_start);
    auto const finish = expand_location_to_spelling_point(range.m_finish);
    if (!start.file || !finish.file
        || normalized_path(start.file) != normalized_path(finish.file))
        return std::nullopt;
    auto const* file = source_file(start.file);
    if (!file)
        return std::nullopt;
    auto const begin = source_offset(*file, start);
    auto const last = source_offset(*file, finish);
    if (!begin || !last || *begin > *last
        || *last >= file->text.size()
        || *last + 1 > std::numeric_limits<std::uint32_t>::max())
        return std::nullopt;
    return SourceSpan {
        .file_path = file->path,
        .begin = static_cast<std::uint32_t>(*begin),
        .end = static_cast<std::uint32_t>(*last + 1),
    };
}

std::optional<SourceSpan> expression_span(tree expression)
{
    if (!expression || !EXPR_P(expression))
        return std::nullopt;
    return source_span(EXPR_LOCATION_RANGE(expression));
}

std::optional<SourceSpan> declaration_span(tree declaration)
{
    if (!declaration || !DECL_P(declaration)
        || !DECL_NAME(declaration))
        return std::nullopt;
    auto const start = expand_location_to_spelling_point(
        DECL_SOURCE_LOCATION(declaration));
    if (!start.file || !is_user_source_location(
            DECL_SOURCE_LOCATION(declaration)))
        return std::nullopt;
    auto const* file = source_file(start.file);
    if (!file)
        return std::nullopt;
    auto const begin = source_offset(*file, start);
    auto const name = identifier(declaration);
    if (!begin || name.empty() || *begin + name.size() > file->text.size()
        || file->text.compare(*begin, name.size(), name) != 0
        || *begin + name.size()
            > std::numeric_limits<std::uint32_t>::max())
        return std::nullopt;
    return SourceSpan {
        .file_path = file->path,
        .begin = static_cast<std::uint32_t>(*begin),
        .end = static_cast<std::uint32_t>(*begin + name.size()),
    };
}

bool identifier_character(char character)
{
    return (character >= 'a' && character <= 'z')
        || (character >= 'A' && character <= 'Z')
        || (character >= '0' && character <= '9')
        || character == '_';
}

std::optional<SourceSpan> named_expression_span(
    tree expression,
    tree declaration)
{
    auto span = expression_span(expression);
    if (!span)
        return std::nullopt;
    auto const* file = source_file(span->file_path);
    auto const name = identifier(declaration);
    if (!file || name.empty() || span->end > file->text.size())
        return std::nullopt;

    std::optional<std::size_t> match;
    auto position = static_cast<std::size_t>(span->begin);
    while (position + name.size() <= span->end) {
        position = file->text.find(name, position);
        if (position == std::string::npos
            || position + name.size() > span->end)
            break;
        bool const begins_identifier = position > 0
            && identifier_character(file->text[position - 1]);
        bool const ends_identifier = position + name.size() < file->text.size()
            && identifier_character(file->text[position + name.size()]);
        if (!begins_identifier && !ends_identifier)
            match = position;
        position += name.size();
    }
    if (!match)
        return std::nullopt;
    span->begin = static_cast<std::uint32_t>(*match);
    span->end = static_cast<std::uint32_t>(*match + name.size());
    return span;
}

tree referenced_declaration(tree expression)
{
    if (!expression)
        return nullptr;
    if (TREE_CODE(expression) == VAR_DECL
        || TREE_CODE(expression) == FIELD_DECL)
        return expression;
    if (TREE_CODE(expression) == COMPONENT_REF) {
        tree const field = TREE_OPERAND(expression, 1);
        return field && TREE_CODE(field) == FIELD_DECL ? field : nullptr;
    }
    switch (TREE_CODE(expression)) {
    case NOP_EXPR:
    case CONVERT_EXPR:
    case VIEW_CONVERT_EXPR:
    case NON_LVALUE_EXPR:
    case INDIRECT_REF:
    case ADDR_EXPR:
        return referenced_declaration(TREE_OPERAND(expression, 0));
    default:
        return nullptr;
    }
}

bool declaration_has_source_initializer(tree declaration)
{
    auto span = declaration_span(declaration);
    if (!span)
        return true;
    auto const* file = source_file(span->file_path);
    if (!file)
        return true;
    std::size_t position = span->end;
    while (position < file->text.size()
           && (file->text[position] == ' '
               || file->text[position] == '\t'
               || file->text[position] == '\r'
               || file->text[position] == '\n'))
        ++position;
    if (position == file->text.size())
        return true;
    return file->text[position] == '='
        || file->text[position] == '{'
        || file->text[position] == '(';
}

void append_ref_annotation(
    StatementAnalysis& analysis,
    RefAnnotation annotation)
{
    auto const duplicate = std::find_if(
        analysis.refs.begin(), analysis.refs.end(),
        [&](auto const& existing) {
            return existing.declaration == annotation.declaration
                && existing.span == annotation.span;
        });
    if (duplicate == analysis.refs.end())
        analysis.refs.push_back(std::move(annotation));
}

void validate_unique_graph_local(
    tree declaration,
    std::string const& identity,
    SourceSpan const& span)
{
    if (TREE_CODE(declaration) != VAR_DECL
        || TREE_CODE(TREE_TYPE(declaration)) == REFERENCE_TYPE)
        return;
    auto const [found, inserted] = graph_local_declarations.emplace(
        identity, span);
    if (inserted || found->second == span)
        return;
    error_at(
        DECL_SOURCE_LOCATION(declaration),
        "graph-significant local %qs is already declared in this function; "
        "use a unique local name",
        identifier(declaration).c_str());
}

tree analyze_statement_node(tree* node, int* walk_subtrees, void* data)
{
    if (!node || !*node)
        return nullptr;
    if (TREE_CODE(*node) == STATEMENT_LIST) {
        *walk_subtrees = 0;
        return nullptr;
    }
    if (TREE_CODE(*node) == LAMBDA_EXPR) {
        *walk_subtrees = 0;
        return nullptr;
    }

    auto& analysis = *static_cast<StatementAnalysis*>(data);
    if (TREE_CODE(*node) == DECL_EXPR) {
        tree const declaration = DECL_EXPR_DECL(*node);
        if (!DECL_ARTIFICIAL(declaration)) {
            auto const kind = annotatable_ref_kind(TREE_TYPE(declaration));
            if (kind != AnnotatableRefKind::none) {
                auto const span = declaration_span(declaration);
                auto const identity = declaration_identity(declaration);
                if (span && !identity.empty())
                    validate_unique_graph_local(declaration, identity, *span);
                if (kind == AnnotatableRefKind::node
                    && TREE_CODE(TREE_TYPE(declaration)) != REFERENCE_TYPE
                    && !declaration_has_source_initializer(declaration)) {
                    *walk_subtrees = 0;
                    return nullptr;
                }
            }
        }
    }

    if (TREE_CODE(*node) == INIT_EXPR) {
        tree const destination = TREE_OPERAND(*node, 0);
        tree const declaration = referenced_declaration(destination);
        if (declaration && !DECL_ARTIFICIAL(declaration)
            && TREE_CODE(TREE_TYPE(declaration)) != REFERENCE_TYPE) {
            auto const kind = annotatable_ref_kind(TREE_TYPE(declaration));
            if (kind != AnnotatableRefKind::none) {
                auto span = kind == AnnotatableRefKind::node
                    ? declaration_span(declaration)
                    : expression_span(TREE_OPERAND(*node, 1));
                auto const identity = declaration_identity(declaration);
                if (span && !identity.empty()) {
                    validate_unique_graph_local(
                        declaration, identity, *span);
                    append_ref_annotation(analysis, {
                        .value = destination,
                        .declaration = declaration,
                        .value_is_pointer = false,
                        .declaration_identity = identity,
                        .span = *span,
                    });
                }
            }
        }
    }

    if (TREE_CODE(*node) == ADDR_EXPR) {
        tree const declaration = referenced_declaration(*node);
        if (declaration) {
            auto const kind = annotatable_ref_kind(TREE_TYPE(declaration));
            if (kind != AnnotatableRefKind::none) {
                if (kind == AnnotatableRefKind::node
                    && !declaration_has_source_initializer(declaration)) {
                    return nullptr;
                }
                auto const span = named_expression_span(*node, declaration);
                auto const identity = declaration_identity(declaration);
                if (span && !identity.empty()) {
                    append_ref_annotation(analysis, {
                        .value = *node,
                        .declaration = declaration,
                        .value_is_pointer = true,
                        .declaration_identity = identity,
                        .span = *span,
                    });
                }
            }
        }
    }

    if (TREE_CODE(*node) == CALL_EXPR) {
        tree const function = called_function(*node);
        if (function_is_named(function, "operator=")
            && call_expr_nargs(*node) > 0) {
            tree const destination = CALL_EXPR_ARG(*node, 0);
            tree const declaration = referenced_declaration(destination);
            if (declaration && !DECL_ARTIFICIAL(declaration)
                && annotatable_ref_kind(TREE_TYPE(declaration))
                    == AnnotatableRefKind::node
                && TREE_CODE(TREE_TYPE(declaration)) != REFERENCE_TYPE
                && !declaration_has_source_initializer(declaration)) {
                auto const identity = declaration_identity(declaration);
                auto const declaration_source_span = declaration_span(declaration);
                auto initialization_source_span = expression_span(*node);
                if (!initialization_source_span)
                    initialization_source_span = named_expression_span(
                        destination, declaration);
                if (!identity.empty() && declaration_source_span) {
                    validate_unique_graph_local(
                        declaration, identity, *declaration_source_span);
                    append_ref_annotation(analysis, {
                        .value = destination,
                        .declaration = declaration,
                        .value_is_pointer = TREE_CODE(TREE_TYPE(destination))
                            == POINTER_TYPE,
                        .declaration_identity = identity,
                        .span = *declaration_source_span,
                    });
                }
                if (!identity.empty() && initialization_source_span) {
                    append_ref_annotation(analysis, {
                        .value = destination,
                        .declaration = declaration,
                        .value_is_pointer = TREE_CODE(TREE_TYPE(destination))
                            == POINTER_TYPE,
                        .declaration_identity = identity,
                        .span = *initialization_source_span,
                    });
                }
            }
        }

        bool const sample_outputs = function_is_named(function, "outputs");
        bool const event_outputs = function_is_named(function, "event_outputs");
        if ((sample_outputs || event_outputs)
            && record_type_name(DECL_CONTEXT(function)) == "GraphBuilder"
            && call_expr_nargs(*node) > 1
            && is_user_source_location(EXPR_LOCATION(*node))) {
            tree const builder_pointer = CALL_EXPR_ARG(*node, 0);
            auto const argument_count = static_cast<std::size_t>(
                call_expr_nargs(*node));
            for (std::size_t i = 1; i < argument_count; ++i) {
                if (auto const span = expression_span(
                        CALL_EXPR_ARG(*node, i))) {
                    analysis.public_outputs.push_back({
                        .builder_pointer = builder_pointer,
                        .event = event_outputs,
                        .ordinal = i - 1,
                        .span = *span,
                    });
                }
            }
        }
    }

    return nullptr;
}

tree string_pointer(std::string const& value)
{
    return decay_conversion(build_string_literal(value.c_str()), tf_none);
}

tree expression_statement(location_t location, tree expression)
{
    tree const statement = build_stmt(location, EXPR_STMT, expression);
    protected_set_expr_location(statement, location);
    return statement;
}

tree build_ref_annotation(RefAnnotation const& annotation)
{
    tree pointer = annotation.value;
    if (!annotation.value_is_pointer) {
        pointer = cp_build_addr_expr(pointer, tf_warning_or_error);
        if (pointer == error_mark_node)
            return nullptr;
    }
    tree const pointer_type = TREE_TYPE(pointer);
    if (!pointer_type || TREE_CODE(pointer_type) != POINTER_TYPE)
        return nullptr;

    tree const template_arguments = make_tree_vec(1);
    TREE_VEC_ELT(template_arguments, 0) = TREE_TYPE(pointer_type);
    tree const specialization = instantiate_template(
        source_annotation_template,
        template_arguments,
        tf_warning_or_error);
    if (specialization == error_mark_node)
        return nullptr;

    tree arguments[] = {
        pointer,
        string_pointer(annotation.declaration_identity),
        string_pointer(annotation.span.file_path),
        build_int_cst(unsigned_type_node, annotation.span.begin),
        build_int_cst(unsigned_type_node, annotation.span.end),
    };
    tree const call = build_cxx_call(
        specialization, 5, arguments, tf_warning_or_error);
    if (call == error_mark_node)
        return nullptr;
    return expression_statement(
        DECL_SOURCE_LOCATION(annotation.declaration), call);
}

tree build_public_output_annotation(
    PublicOutputAnnotation const& output)
{
    tree arguments[] = {
        output.builder_pointer,
        output.event ? boolean_true_node : boolean_false_node,
        build_int_cst(size_type_node, output.ordinal),
        string_pointer(output.span.file_path),
        build_int_cst(unsigned_type_node, output.span.begin),
        build_int_cst(unsigned_type_node, output.span.end),
    };
    tree const call = build_cxx_call(
        public_output_annotation_function,
        6,
        arguments,
        tf_warning_or_error);
    if (call == error_mark_node)
        return nullptr;
    return expression_statement(EXPR_LOCATION(output.builder_pointer), call);
}

tree annotate_statement_lists(tree* node, int* walk_subtrees, void*)
{
    if (!node || !*node || TREE_CODE(*node) != STATEMENT_LIST)
        return nullptr;

    for (auto iterator = tsi_start(*node); !tsi_end_p(iterator);
         tsi_next(&iterator)) {
        tree* const statement = tsi_stmt_ptr(iterator);
        cp_walk_tree_without_duplicates(
            statement, annotate_statement_lists, nullptr);

        StatementAnalysis analysis;
        cp_walk_tree_without_duplicates(
            statement, analyze_statement_node, &analysis);

        std::vector<tree> annotations;
        annotations.reserve(
            analysis.refs.size()
            + analysis.public_outputs.size());
        for (auto const& ref : analysis.refs) {
            if (tree const annotation = build_ref_annotation(ref))
                annotations.push_back(annotation);
        }
        for (auto const& output : analysis.public_outputs) {
            if (tree const annotation = build_public_output_annotation(output))
                annotations.push_back(annotation);
        }
        for (tree annotation : annotations)
            tsi_link_after(&iterator, annotation, TSI_NEW_STMT);
    }

    *walk_subtrees = 0;
    return nullptr;
}

void transform_body(tree* body)
{
    if (!body || !*body
        || transformed_bodies.find(*body) != transformed_bodies.end())
        return;
    transformed_bodies.insert(*body);
    cp_walk_tree_without_duplicates(
        body, annotate_statement_lists, nullptr);
}

void finish_parse_function(void* gcc_data, void*)
{
    tree const function = static_cast<tree>(gcc_data);
    if (!function || TREE_CODE(function) != FUNCTION_DECL)
        return;

    if (function_is_named(
            function, "_annotate_source_info_after_statement")) {
        source_annotation_template = DECL_TEMPLATE_INFO(function)
            ? DECL_TI_TEMPLATE(function)
            : function;
    } else if (function_is_named(
                   function, "_annotate_public_output_after_statement")) {
        public_output_annotation_function = function;
    }

    if (!DECL_SAVED_TREE(function)
        || !source_annotation_template
        || !public_output_annotation_function
        || !is_user_source_location(DECL_SOURCE_LOCATION(function)))
        return;

    if (!debug_function.empty()
        && identifier(function) == debug_function) {
        std::fprintf(stderr, "\n=== DECL_SAVED_TREE(%s) ===\n",
            debug_function.c_str());
        debug_tree(DECL_SAVED_TREE(function));
        if (auto* definition = retrieve_constexpr_fundef(function)) {
            std::fprintf(stderr, "\n=== constexpr_fundef::body(%s) ===\n",
                debug_function.c_str());
            debug_tree(definition->body);
        }
        std::fprintf(stderr, "\n=== end %s trees ===\n",
            debug_function.c_str());
    }

    transform_body(&DECL_SAVED_TREE(function));
    if (auto* definition = retrieve_constexpr_fundef(function))
        transform_body(&definition->body);
}

bool parse_plugin_arguments(plugin_name_args const& plugin_info)
{
    for (int i = 0; i < plugin_info.argc; ++i) {
        std::string_view const key = plugin_info.argv[i].key
            ? plugin_info.argv[i].key
            : "";
        std::string_view const value = plugin_info.argv[i].value
            ? plugin_info.argv[i].value
            : "";
        if (key == "core-source-dir") {
            core_source_dir = normalized_path(std::string(value));
        } else if (key == "dump-function") {
            debug_function = value;
        } else {
            error("unknown %qs plugin argument %qs",
                "iv_gcc_source_introspection_plugin",
                std::string(key).c_str());
            return false;
        }
    }
    if (core_source_dir.empty()) {
        error("plugin %qs requires option %qs",
            "iv_gcc_source_introspection_plugin",
            "-fplugin-arg-iv_gcc_source_introspection_plugin-core-source-dir=<path>");
        return false;
    }
    return true;
}
}

int plugin_init(
    plugin_name_args* plugin_info,
    plugin_gcc_version* version)
{
    if (!plugin_default_version_check(version, &gcc_version)
        || !parse_plugin_arguments(*plugin_info))
        return 1;
    register_callback(
        plugin_info->base_name,
        PLUGIN_FINISH_PARSE_FUNCTION,
        finish_parse_function,
        nullptr);
    return 0;
}
