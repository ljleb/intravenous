#pragma once

// Stable registrations emitted by IV_NODE / IV_MODULE. Registration is data,
// not a process-global side effect: the finalizer collects these records into
// each IV package, and BuilderSession receives the records from the loaded IV
// sources used for one graph configuration.

#include <intravenous/graph/builder/syntax.h>
#include <intravenous/node/code_key.h>

#include <cstddef>
#include <string_view>
#include <type_traits>

namespace iv {
class GraphBuilder;
class NodeRef;

namespace details {
enum class PackageRegistrationKind {
    node,
    module,
};

using IvModuleConfigureFunction = void (*)(GraphBuilder&);
using NodeTypeConfigureFunction = NodeRef (*)(GraphBuilder&);

struct PackageRegistration {
    PackageRegistrationKind kind{};
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

static_assert(std::is_standard_layout_v<PackageRegistration>);
static_assert(std::is_trivially_copyable_v<PackageRegistration>);

inline constexpr std::string_view package_registration_section =
    "iv_package_registrations";

// g.node<"id"> resolves synchronously through the registrations attached to
// this GraphBuilder's BuilderSession. An iv module is fully configured before
// this function returns; no unresolved registered-node bundle is preserved.
NodeRef configure_registered_definition(GraphBuilder&, std::string_view id);
} // namespace details
} // namespace iv

#if defined(__clang__)
#define IV_PACKAGE_REGISTRATION_RECORD \
    __attribute__((used, section("iv_package_registrations")))
#else
#error "IV package registration requires Clang"
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
    IV_PACKAGE_REGISTRATION_RECORD constinit const \
        ::iv::details::PackageRegistration \
        IV_PACKAGE_CONCAT(iv_package_module_registration_, Unique){ \
            ::iv::details::PackageRegistrationKind::module, \
            Id, sizeof(Id) - 1, \
            __FILE__, sizeof(__FILE__) - 1, \
            IV_PACKAGE_ROOT, sizeof(IV_PACKAGE_ROOT) - 1, \
            Function, nullptr, nullptr}; \
    }

#define IV_NODE(Id, Node) \
    IV_NODE_IMPL(Id, Node, __COUNTER__)
#define IV_NODE_IMPL(Id, Node, Unique) \
    namespace { \
    ::iv::NodeRef IV_PACKAGE_CONCAT(iv_package_node_configure_, Unique)( \
        ::iv::GraphBuilder& builder) { \
        return builder.template node<Node>(); \
    } \
    IV_PACKAGE_REGISTRATION_RECORD constinit const \
        ::iv::details::PackageRegistration \
        IV_PACKAGE_CONCAT(iv_package_node_registration_, Unique){ \
            ::iv::details::PackageRegistrationKind::node, \
            Id, sizeof(Id) - 1, \
            __FILE__, sizeof(__FILE__) - 1, \
            IV_PACKAGE_ROOT, sizeof(IV_PACKAGE_ROOT) - 1, \
            nullptr, &IV_PACKAGE_CONCAT(iv_package_node_configure_, Unique), \
            &::iv::details::node_compiler_record<Node>}; \
    }
