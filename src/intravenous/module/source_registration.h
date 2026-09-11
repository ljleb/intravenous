#pragma once

// The source registration boundary is intentionally small and lives in the
// prebuilt builder library. An IV source registers graph definitions while
// its temporary authoring generation is initialized. The finalizer publishes
// only registrations owned by the independently built source root. The host
// assembles the loaded source artifacts into one authoring generation; an ID
// lookup here dispatches through that generation rather than preserving an
// unresolved graph placeholder.

#include <intravenous/graph/builder/syntax.h>
#include <intravenous/node/code_key.h>

#include <cstddef>
#include <string_view>

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
    // __FILE__ from the registration site remains available for accurate
    // compiler metadata about the declaration location.
    char const* source_file = nullptr;
    std::size_t source_file_size = 0;
    // ModuleSupport.cmake injects the canonical package root independently
    // of __FILE__. Custom CMake sources can otherwise be compiled with a
    // relative file spelling, which is not a reliable ownership boundary.
    char const* source_root = nullptr;
    std::size_t source_root_size = 0;
    SourceModuleBuildFunction module_build = nullptr;
    // A primitive registration owns a source-TU thunk. The finalizer invokes
    // it only while authoring this source's independently published node-type
    // template; generated interfaces never invoke it or include provider
    // source.
    SourceNodeBuildFunction node_build = nullptr;
    void const* node_compiler_record = nullptr;
};

void register_source_definition(SourceRegistrationView);
std::size_t source_definition_count() noexcept;
SourceRegistrationView source_definition_at(std::size_t);
std::size_t source_module_count(std::string_view source_root) noexcept;
SourceRegistrationView source_module_at(std::string_view source_root, std::size_t);
std::size_t source_node_count(std::string_view source_root) noexcept;
SourceRegistrationView source_node_at(std::string_view source_root, std::size_t);
NodeCodeKey source_node_code_key(std::string_view source_root, std::size_t);
// The generic ID API dispatches both implementation kinds through this one
// operation. A primitive is authored directly; an iv module executes its
// builder and is embedded before this function returns.
NodeRef author_registered_source_definition(GraphBuilder&, std::string_view id);
void clear_source_definitions() noexcept;
void clear_source_definitions_for_root(std::string_view source_root) noexcept;

class SourceModuleRegistration final {
public:
    SourceModuleRegistration(
        char const* id,
        std::size_t id_size,
        SourceModuleBuildFunction build,
        char const* source_file,
        std::size_t source_file_size,
        char const* source_root,
        std::size_t source_root_size);
};

class SourceNodeRegistration final {
public:
    SourceNodeRegistration(
        char const* id,
        std::size_t id_size,
        SourceNodeBuildFunction build,
        void const* node_compiler_record,
        char const* source_file,
        std::size_t source_file_size,
        char const* source_root,
        std::size_t source_root_size);
};
} // namespace details
} // namespace iv

#if defined(__GNUC__) || defined(__clang__)
#define IV_SOURCE_REGISTRATION_USED __attribute__((used))
#define IV_SOURCE_REGISTRATION_CONSTRUCTOR __attribute__((constructor))
#else
#define IV_SOURCE_REGISTRATION_USED
#define IV_SOURCE_REGISTRATION_CONSTRUCTOR
#endif

#define IV_SOURCE_CONCAT_INNER(a, b) a##b
#define IV_SOURCE_CONCAT(a, b) IV_SOURCE_CONCAT_INNER(a, b)

// Package CMake builds supply a canonical source root.  The fallback retains
// standalone/single-file behavior while source packages never infer
// ownership from a potentially relative __FILE__ spelling.
#ifndef IV_SOURCE_REGISTRATION_ROOT
#define IV_SOURCE_REGISTRATION_ROOT __FILE__
#endif

// Each registration gets its own named constructor entry. The authoring JIT
// runs those entries before it asks for the source definition set. The
// finalized source artifact retains them so its provider callbacks can join a
// later shared authoring generation.
#define IV_MODULE(Id, Function) \
    IV_MODULE_IMPL(Id, Function, __COUNTER__)
#define IV_MODULE_IMPL(Id, Function, Unique) \
    namespace { \
    IV_SOURCE_REGISTRATION_USED IV_SOURCE_REGISTRATION_CONSTRUCTOR \
    void IV_SOURCE_CONCAT(iv_source_module_registration_ctor_, Unique)(); \
    void IV_SOURCE_CONCAT(iv_source_module_registration_ctor_, Unique)() { \
        static const ::iv::details::SourceModuleRegistration \
            IV_SOURCE_CONCAT(iv_source_module_registration_, Unique){ \
                Id, sizeof(Id) - 1, Function, \
                __FILE__, sizeof(__FILE__) - 1, \
                IV_SOURCE_REGISTRATION_ROOT, \
                sizeof(IV_SOURCE_REGISTRATION_ROOT) - 1}; \
    } \
    }

// IV_NODE registers the primitive's source-local authoring thunk. The generic
// g.node<Id>() path is available without an imported specialization and calls
// this thunk only after the source has joined the authoring generation.
#define IV_NODE(Id, Node) \
    IV_NODE_IMPL(Id, Node, __COUNTER__)
#define IV_NODE_IMPL(Id, Node, Unique) \
    namespace { \
    IV_SOURCE_REGISTRATION_USED IV_SOURCE_REGISTRATION_CONSTRUCTOR \
    void IV_SOURCE_CONCAT(iv_source_node_registration_ctor_, Unique)(); \
    void IV_SOURCE_CONCAT(iv_source_node_registration_ctor_, Unique)() { \
        static const ::iv::details::SourceNodeRegistration \
            IV_SOURCE_CONCAT(iv_source_node_registration_, Unique){ \
                Id, sizeof(Id) - 1, \
                [](::iv::GraphBuilder& builder) -> ::iv::NodeRef { \
                    return builder.template node<Node>(); \
                }, \
                &::iv::details::node_compiler_record<Node>, \
                __FILE__, sizeof(__FILE__) - 1, \
                IV_SOURCE_REGISTRATION_ROOT, \
                sizeof(IV_SOURCE_REGISTRATION_ROOT) - 1}; \
    } \
    }
