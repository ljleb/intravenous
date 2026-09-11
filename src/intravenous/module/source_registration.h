#pragma once

// The source registration boundary is intentionally small and lives in the
// prebuilt builder library. An IV source registers graph definitions while
// its temporary authoring generation is initialized. The finalizer publishes
// only registrations owned by the independently built source root. Nothing
// here is a persistent registry -- stable-ID validation and publication remain
// host responsibilities.

#include <intravenous/graph/builder/syntax.h>
#include <intravenous/node/code_key.h>

#include <cstddef>
#include <string_view>

namespace iv {
class GraphBuilder;
class NodeRef;

template<fixed_string Id>
struct node_interface;

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
    // __FILE__ from the registration site lets the source artifact publish
    // only definitions that belong to its own root directory.
    char const* source_file = nullptr;
    std::size_t source_file_size = 0;
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
// The interface protocol intentionally dispatches both implementation kinds
// through this one operation.  A caller records only an ID; it never embeds a
// provider function name in a generated header.
NodeRef author_registered_source_definition(GraphBuilder&, std::string_view id);
void clear_source_definitions() noexcept;

class SourceModuleRegistration final {
public:
    SourceModuleRegistration(
        char const* id,
        std::size_t id_size,
        SourceModuleBuildFunction build,
        char const* source_file,
        std::size_t source_file_size);
};

class SourceNodeRegistration final {
public:
    SourceNodeRegistration(
        char const* id,
        std::size_t id_size,
        SourceNodeBuildFunction build,
        void const* node_compiler_record,
        char const* source_file,
        std::size_t source_file_size);
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

// The generated <iv/nodes/...> header emits this exact definition for every
// registered ID.  Node type versus iv-module is implementation-private: both
// have the same dynamic NodeRef contract at this stage of the source
// migration, and neither exposes a provider C++ symbol.
#define IV_REGISTERED_INTERFACE(Id) \
    template<> struct iv::node_interface<Id> { \
        template<class... Args> \
        static ::iv::NodeRef author(::iv::GraphBuilder& builder, Args&&...) { \
            static_assert(sizeof...(Args) == 0, \
                "registered IV definitions do not yet declare public authored arguments"); \
            return ::iv::details::author_registered_source_definition(builder, Id); \
        } \
    }

// Each registration gets its own named constructor entry. The authoring JIT
// runs those entries before it asks for the source definition set; the
// finalizer can then remove precisely these source-only entries without
// touching unrelated user static initialization.
#define IV_MODULE(Id, Function) \
    IV_MODULE_IMPL(Id, Function, __COUNTER__)
#define IV_MODULE_IMPL(Id, Function, Unique) \
    IV_REGISTERED_INTERFACE(Id); \
    namespace { \
    IV_SOURCE_REGISTRATION_USED IV_SOURCE_REGISTRATION_CONSTRUCTOR \
    void IV_SOURCE_CONCAT(iv_source_module_registration_ctor_, Unique)(); \
    void IV_SOURCE_CONCAT(iv_source_module_registration_ctor_, Unique)() { \
        static const ::iv::details::SourceModuleRegistration \
            IV_SOURCE_CONCAT(iv_source_module_registration_, Unique){ \
                Id, sizeof(Id) - 1, Function, \
                __FILE__, sizeof(__FILE__) - 1}; \
    } \
    }

// IV_NODE provides an immediately fresh same-translation-unit interface.  An
// imported interface is generated separately; it never includes this node's
// implementation source.  The current execution model still authors the
// concrete primitive through GraphBuilder, which makes this a narrow source
// refinement rather than the deferred whole-graph execution rewrite.
#define IV_NODE(Id, Node) \
    IV_NODE_IMPL(Id, Node, __COUNTER__)
#define IV_NODE_IMPL(Id, Node, Unique) \
    IV_REGISTERED_INTERFACE(Id); \
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
                __FILE__, sizeof(__FILE__) - 1}; \
    } \
    }
