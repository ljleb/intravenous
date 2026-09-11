#pragma once

// Stable package definitions emitted by IV_NODE / IV_MODULE. Definitions are data,
// not a process-global side effect: the finalizer collects these records into
// each IV package, and BuilderSession receives the records from the loaded IV
// packages used for one graph configuration.

#include <intravenous/graph/builder/syntax.h>
#include <intravenous/module/configuration_argument.h>
#include <intravenous/node/code_key.h>

#include <cstddef>
#include <string_view>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <string>

namespace iv {
class GraphBuilder;
class NodeRef;

namespace details {
enum class PackageDefinitionKind {
    node,
    module,
};

using IvModuleConfigureFunction = void (*)(
    GraphBuilder&, std::span<ConfigurationArgument>);
using NodeTypeConfigureFunction = NodeRef (*)(GraphBuilder&);

struct PackageDefinition {
    PackageDefinitionKind kind{};
    char const* id = nullptr;
    std::size_t id_size = 0;
    char const* source_file = nullptr;
    std::size_t source_file_size = 0;
    char const* package_root = nullptr;
    std::size_t package_root_size = 0;
    IvModuleConfigureFunction module_build = nullptr;
    NodeTypeConfigureFunction node_build = nullptr;
    void const* node_compiler_record = nullptr;
};

static_assert(std::is_standard_layout_v<PackageDefinition>);
static_assert(std::is_trivially_copyable_v<PackageDefinition>);

inline constexpr std::string_view package_definition_section =
    "iv_package_definitions";

// g.node<"id"> resolves synchronously through the definitions attached to
// this GraphBuilder's BuilderSession. An iv module is fully configured before
// this function returns; no unresolved package-definition node is preserved.
NodeRef configure_package_definition(
    GraphBuilder&, std::string_view id, std::span<ConfigurationArgument> arguments);

template<auto Function>
struct IvModuleConfigureAdapter;

template<class Return, class... Args, Return (*Function)(GraphBuilder&, Args...)>
struct IvModuleConfigureAdapter<Function> {
    static_assert(std::is_void_v<Return>,
        "an IV module configuration function must return void");

    template<class Arg>
    static decltype(auto) argument(ConfigurationArgument& value)
    {
        using T = std::remove_cvref_t<Arg>;
        constexpr auto expected = clang_type_name<T>();
        if (!value.data || configuration_argument_type(value) != expected) {
            throw std::invalid_argument(
                "IV module configuration argument type mismatch: expected '"
                + std::string(expected) + "', got '"
                + std::string(configuration_argument_type(value)) + "'");
        }
        auto& object = *static_cast<T*>(value.data);
        if constexpr (std::is_lvalue_reference_v<Arg>) {
            return static_cast<Arg>(object);
        } else if constexpr (std::is_rvalue_reference_v<Arg>) {
            return static_cast<Arg>(std::move(object));
        } else {
            return T(std::move(object));
        }
    }

    template<std::size_t... Index>
    static void invoke(
        GraphBuilder& builder,
        std::span<ConfigurationArgument> arguments,
        std::index_sequence<Index...>)
    {
        Function(builder, argument<Args>(arguments[Index])...);
    }

    static void configure(
        GraphBuilder& builder,
        std::span<ConfigurationArgument> arguments)
    {
        if (arguments.size() != sizeof...(Args)) {
            throw std::invalid_argument(
                "IV module configuration argument count mismatch");
        }
        invoke(builder, arguments, std::index_sequence_for<Args...>{});
    }
};
} // namespace details
} // namespace iv

#if defined(__clang__)
#define IV_PACKAGE_DEFINITION_RECORD \
    __attribute__((used, section("iv_package_definitions")))
#else
#error "IV package definition requires Clang"
#endif

#define IV_PACKAGE_CONCAT_INNER(a, b) a##b
#define IV_PACKAGE_CONCAT(a, b) IV_PACKAGE_CONCAT_INNER(a, b)

#ifndef IV_PACKAGE_ROOT
#define IV_PACKAGE_ROOT __FILE__
#endif

#define IV_MODULE(Id, Function) \
    IV_MODULE_IMPL(Id, Function, __COUNTER__)
#define IV_MODULE_IMPL(Id, Function, Unique) \
    namespace { \
    IV_PACKAGE_DEFINITION_RECORD constinit const \
        ::iv::details::PackageDefinition \
        IV_PACKAGE_CONCAT(iv_package_module_definition_, Unique){ \
            ::iv::details::PackageDefinitionKind::module, \
            Id, sizeof(Id) - 1, \
            __FILE__, sizeof(__FILE__) - 1, \
            IV_PACKAGE_ROOT, sizeof(IV_PACKAGE_ROOT) - 1, \
            &::iv::details::IvModuleConfigureAdapter<Function>::configure, \
            nullptr, nullptr}; \
    }

#define IV_NODE(Id, Node) \
    IV_NODE_IMPL(Id, Node, __COUNTER__)
#define IV_NODE_IMPL(Id, Node, Unique) \
    namespace { \
    ::iv::NodeRef IV_PACKAGE_CONCAT(iv_package_node_configure_, Unique)( \
        ::iv::GraphBuilder& builder) { \
        return builder.template node<Node>(); \
    } \
    IV_PACKAGE_DEFINITION_RECORD constinit const \
        ::iv::details::PackageDefinition \
        IV_PACKAGE_CONCAT(iv_package_node_definition_, Unique){ \
            ::iv::details::PackageDefinitionKind::node, \
            Id, sizeof(Id) - 1, \
            __FILE__, sizeof(__FILE__) - 1, \
            IV_PACKAGE_ROOT, sizeof(IV_PACKAGE_ROOT) - 1, \
            nullptr, &IV_PACKAGE_CONCAT(iv_package_node_configure_, Unique), \
            &::iv::details::node_compiler_record<Node>}; \
    }
