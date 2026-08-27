#include "gcc-plugin.h"
#include "plugin-version.h"
#include "tree.h"
#include "tree-iterator.h"
#include "cp/cp-tree.h"
#include "c-family/c-common.h"

#include <cstdio>
#include <string_view>

int plugin_is_GPL_compatible;

namespace {
tree annotation_function;
tree graph_annotation_function;
tree initialized_graph_annotation_function;

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
                        || TREE_CODE(function) == NOP_EXPR))
        function = TREE_OPERAND(function, 0);
    return function && TREE_CODE(function) == FUNCTION_DECL
        ? function
        : nullptr;
}

bool function_is_named(tree function, std::string_view name)
{
    return function && DECL_NAME(function)
        && std::string_view(IDENTIFIER_POINTER(DECL_NAME(function))) == name;
}

tree log_real_authored_expression(tree* node, int*, void*)
{
    if (!node || !*node || !EXPR_P(*node))
        return nullptr;
    source_range const range = EXPR_LOCATION_RANGE(*node);
    expanded_location const start = expand_location(range.m_start);
    expanded_location const finish = expand_location(range.m_finish);
    std::string_view const file = start.file ? start.file : "";
    constexpr std::string_view suffix = "real_authored.hpp";
    if (file.size() < suffix.size()
        || file.substr(file.size() - suffix.size()) != suffix
        || start.line != 43)
        return nullptr;
    std::fprintf(
        stderr,
        "expression code=%s start=%d:%d finish=%d:%d wrapper=%d\n",
        get_tree_code_name(TREE_CODE(*node)),
        start.line,
        start.column,
        finish.line,
        finish.column,
        location_wrapper_p(*node));
    return nullptr;
}

tree annotate_probe_call(tree* node, int* walk_subtrees, void*)
{
    if (!node || !*node || TREE_CODE(*node) != CALL_EXPR)
        return nullptr;

    tree const function = called_function(*node);
    if (!function_is_named(function, "iv_plugin_probe_mark"))
        return nullptr;

    tree const argument = CALL_EXPR_ARG(*node, 1);
    location_t const location = EXPR_LOCATION(*node);
    CALL_EXPR_ARG(*node, 1) = build_int_cst(
        TREE_TYPE(argument), LOCATION_LINE(location));
    *walk_subtrees = 0;
    return nullptr;
}

struct AnnotationTarget {
    tree function = nullptr;
    std::string_view callee_name;
    std::string_view source_suffix;
};

tree wrap_node_call(tree* node, int* walk_subtrees, void* data)
{
    auto const& target = *static_cast<AnnotationTarget const*>(data);
    if (!node || !*node || !target.function
        || (TREE_CODE(*node) != CALL_EXPR
            && TREE_CODE(*node) != TARGET_EXPR))
        return nullptr;

    tree const function = called_function(*node);
    if (!function_is_named(function, target.callee_name))
        return nullptr;

    location_t const location = EXPR_LOCATION(*node);
    source_range const range = EXPR_LOCATION_RANGE(*node);
    expanded_location const start = expand_location(range.m_start);
    expanded_location const finish = expand_location(range.m_finish);
    std::string_view const source_file = start.file ? start.file : "";
    if (!target.source_suffix.empty()
        && (source_file.size() < target.source_suffix.size()
            || source_file.substr(source_file.size() - target.source_suffix.size())
                != target.source_suffix))
        return nullptr;
    tree const template_arguments = make_tree_vec(1);
    TREE_VEC_ELT(template_arguments, 0) = TREE_TYPE(*node);
    tree const annotation_specialization = instantiate_template(
        target.function, template_arguments, tf_warning_or_error);
    if (annotation_specialization == error_mark_node)
        return error_mark_node;
    tree arguments[] = {
        *node,
        decay_conversion(build_string_literal(start.file), tf_none),
        build_int_cst(unsigned_type_node, start.line),
        build_int_cst(unsigned_type_node, start.column),
        build_int_cst(unsigned_type_node, finish.line),
        build_int_cst(unsigned_type_node, finish.column),
    };
    *node = build_cxx_call(
        annotation_specialization, 6, arguments, tf_warning_or_error);
    if (*node == error_mark_node)
        return error_mark_node;
    protected_set_expr_location(*node, location);
    *walk_subtrees = 0;
    return nullptr;
}

struct InitializedNodeCall {
    tree destination = nullptr;
    tree initializer = nullptr;
};

tree find_initialized_node_call(
    tree* node, int* walk_subtrees, void* data)
{
    if (!node || !*node)
        return nullptr;
    if (TREE_CODE(*node) == STATEMENT_LIST) {
        *walk_subtrees = 0;
        return nullptr;
    }
    if (TREE_CODE(*node) != INIT_EXPR)
        return nullptr;

    tree const destination = TREE_OPERAND(*node, 0);
    tree const initializer = TREE_OPERAND(*node, 1);
    tree const function = called_function(initializer);
    if (!function_is_named(function, "node"))
        return nullptr;

    location_t const location = EXPR_LOCATION(initializer);
    source_range const range = EXPR_LOCATION_RANGE(initializer);
    expanded_location const start = expand_location(range.m_start);
    expanded_location const finish = expand_location(range.m_finish);
    std::string_view const source_file = start.file ? start.file : "";
    constexpr std::string_view source_suffix = "real_authored.hpp";
    if (source_file.size() < source_suffix.size()
        || source_file.substr(source_file.size() - source_suffix.size())
            != source_suffix)
        return nullptr;

    auto& result = *static_cast<InitializedNodeCall*>(data);
    result = {
        .destination = destination,
        .initializer = initializer,
    };
    *walk_subtrees = 0;
    return nullptr;
}

tree build_initialized_node_annotation(InitializedNodeCall const& node_call)
{
    tree const destination = node_call.destination;
    tree const initializer = node_call.initializer;
    location_t const location = EXPR_LOCATION(initializer);
    source_range const range = EXPR_LOCATION_RANGE(initializer);
    expanded_location const start = expand_location(range.m_start);
    expanded_location const finish = expand_location(range.m_finish);

    tree const template_arguments = make_tree_vec(1);
    TREE_VEC_ELT(template_arguments, 0) = TREE_TYPE(destination);
    tree const annotation_specialization = instantiate_template(
        initialized_graph_annotation_function,
        template_arguments,
        tf_warning_or_error);
    if (annotation_specialization == error_mark_node)
        return nullptr;

    tree const destination_address = cp_build_addr_expr(
        destination, tf_warning_or_error);
    if (destination_address == error_mark_node)
        return nullptr;
    tree arguments[] = {
        destination_address,
        decay_conversion(build_string_literal(start.file), tf_none),
        build_int_cst(unsigned_type_node, start.line),
        build_int_cst(unsigned_type_node, start.column),
        build_int_cst(unsigned_type_node, finish.line),
        build_int_cst(unsigned_type_node, finish.column),
    };
    tree const annotation = build_cxx_call(
        annotation_specialization, 6, arguments, tf_warning_or_error);
    if (annotation == error_mark_node)
        return nullptr;

    tree const statement = build_stmt(location, EXPR_STMT, annotation);
    protected_set_expr_location(statement, location);
    return statement;
}

tree annotate_statement_list(tree* node, int* walk_subtrees, void*)
{
    if (!node || !*node || TREE_CODE(*node) != STATEMENT_LIST)
        return nullptr;

    for (auto iterator = tsi_start(*node); !tsi_end_p(iterator);
         tsi_next(&iterator)) {
        tree* const statement = tsi_stmt_ptr(iterator);
        cp_walk_tree_without_duplicates(
            statement, annotate_statement_list, nullptr);

        InitializedNodeCall node_call;
        cp_walk_tree_without_duplicates(
            statement, find_initialized_node_call, &node_call);
        if (!node_call.destination)
            continue;

        tree const annotation = build_initialized_node_annotation(node_call);
        if (!annotation)
            return error_mark_node;
        tsi_link_after(&iterator, annotation, TSI_SAME_STMT);
    }

    *walk_subtrees = 0;
    return nullptr;
}

void transform_function_body(tree* body)
{
    cp_walk_tree_without_duplicates(
        body, annotate_probe_call, nullptr);
    if (initialized_graph_annotation_function)
        cp_walk_tree_without_duplicates(
            body, annotate_statement_list, nullptr);
    AnnotationTarget probe_target{
        .function = annotation_function,
        .callee_name = "iv_plugin_probe_node",
    };
    cp_walk_tree_without_duplicates(
        body, wrap_node_call, &probe_target);
    AnnotationTarget graph_target{
        .function = graph_annotation_function,
        .callee_name = "node",
        .source_suffix = "real_authored.hpp",
    };
    cp_walk_tree_without_duplicates(
        body, wrap_node_call, &graph_target);
}

void annotate_function(void* gcc_data, void*)
{
    tree const function = static_cast<tree>(gcc_data);
    if (function_is_named(function, "iv_plugin_probe_annotate")) {
        annotation_function = DECL_TEMPLATE_INFO(function)
            ? DECL_TI_TEMPLATE(function)
            : function;
    }
    if (function_is_named(function, "iv_plugin_graph_annotate")) {
        graph_annotation_function = DECL_TEMPLATE_INFO(function)
            ? DECL_TI_TEMPLATE(function)
            : function;
    }
    if (function_is_named(
            function, "iv_plugin_graph_annotate_initialized")) {
        initialized_graph_annotation_function = DECL_TEMPLATE_INFO(function)
            ? DECL_TI_TEMPLATE(function)
            : function;
    }
    if (!function || TREE_CODE(function) != FUNCTION_DECL
        || !DECL_SAVED_TREE(function))
        return;
    if (function_is_named(function, "real_authored_entry"))
        cp_walk_tree_without_duplicates(
            &DECL_SAVED_TREE(function),
            log_real_authored_expression,
            nullptr);
    transform_function_body(&DECL_SAVED_TREE(function));
    if (auto* constexpr_definition = retrieve_constexpr_fundef(function))
        transform_function_body(&constexpr_definition->body);
}

}

int plugin_init(
    plugin_name_args* plugin_info,
    plugin_gcc_version* version)
{
    if (!plugin_default_version_check(version, &gcc_version))
        return 1;
    register_callback(
        plugin_info->base_name,
        PLUGIN_FINISH_PARSE_FUNCTION,
        annotate_function,
        nullptr);
    return 0;
}
