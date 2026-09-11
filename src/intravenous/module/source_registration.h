#pragma once

// Stable registrations emitted by IV_NODE / IV_MODULE. Registration is data,
// not a process-global side effect: the finalizer collects these records into
// each IV source, and BuilderSession receives the records from the loaded IV
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
enum class SourceRegistrationKind {
    node,
    module,
};

using SourceModuleBuildFunction = void (*)(GraphBuilder&);
using SourceNodeBuildFunction = NodeRef (*)(GraphBuilder&);

struct SourceRegistrationView {
    SourceRegistrationKind kind{};
    char const* id = nullptr;
    std::size_t id_size = 0;
    char const* source_file = nullptr;
    std::size_t source_file_size = 0;
    char const* source_root = nullptr;
    std::size_t source_root_size = 0;
    SourceModuleBuildFunction module_build = nullptr;
    SourceNodeBuildFunction node_build = nullptr;
    void const* node_compiler_record = nullptr;
};

static_assert(std::is_standard_layout_v<SourceRegistrationView>);
static_assert(std::is_trivially_copyable_v<SourceRegistrationView>);

inline constexpr std::string_view source_registration_section =
    "iv_source_registrations";

// g.node<"id"> resolves synchronously through the registrations attached to
// this GraphBuilder's BuilderSession. An iv module is fully configured before
// this function returns; no unresolved registered-node bundle is preserved.
NodeRef author_registered_source_definition(GraphBuilder&, std::string_view id);
} // namespace details
} // namespace iv

#if defined(__clang__)
#define IV_SOURCE_REGISTRATION_RECORD \
    __attribute__((used, section("iv_source_registrations")))
#else
#error "IV source registration requires Clang"
#endif

#define IV_SOURCE_CONCAT_INNER(a, b) a##b
#define IV_SOURCE_CONCAT(a, b) IV_SOURCE_CONCAT_INNER(a, b)

#ifndef IV_SOURCE_REGISTRATION_ROOT
#define IV_SOURCE_REGISTRATION_ROOT __FILE__
#endif

#define IV_MODULE(Id, Function) \
    IV_MODULE_IMPL(Id, Function, __COUNTER__)
#define IV_MODULE_IMPL(Id, Function, Unique) \
    namespace { \
    IV_SOURCE_REGISTRATION_RECORD constinit const \
        ::iv::details::SourceRegistrationView \
        IV_SOURCE_CONCAT(iv_source_module_registration_, Unique){ \
            ::iv::details::SourceRegistrationKind::module, \
            Id, sizeof(Id) - 1, \
            __FILE__, sizeof(__FILE__) - 1, \
            IV_SOURCE_REGISTRATION_ROOT, sizeof(IV_SOURCE_REGISTRATION_ROOT) - 1, \
            Function, nullptr, nullptr}; \
    }

#define IV_NODE(Id, Node) \
    IV_NODE_IMPL(Id, Node, __COUNTER__)
#define IV_NODE_IMPL(Id, Node, Unique) \
    namespace { \
    ::iv::NodeRef IV_SOURCE_CONCAT(iv_source_node_build_, Unique)( \
        ::iv::GraphBuilder& builder) { \
        return builder.template node<Node>(); \
    } \
    IV_SOURCE_REGISTRATION_RECORD constinit const \
        ::iv::details::SourceRegistrationView \
        IV_SOURCE_CONCAT(iv_source_node_registration_, Unique){ \
            ::iv::details::SourceRegistrationKind::node, \
            Id, sizeof(Id) - 1, \
            __FILE__, sizeof(__FILE__) - 1, \
            IV_SOURCE_REGISTRATION_ROOT, sizeof(IV_SOURCE_REGISTRATION_ROOT) - 1, \
            nullptr, &IV_SOURCE_CONCAT(iv_source_node_build_, Unique), \
            &::iv::details::node_compiler_record<Node>}; \
    }
