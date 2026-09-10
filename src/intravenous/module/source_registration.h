#pragma once

// The source registration boundary is intentionally small and lives in the
// prebuilt builder library.  An IV source registers graph definitions while
// its temporary authoring generation is initialized; the finalizer then asks
// for that complete, source-local definition set.  Nothing in this header is
// a persistent registry -- stable-ID validation and publication remain host
// responsibilities.

#include <intravenous/graph/builder/syntax.h>

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
    SourceModuleBuildFunction module_build = nullptr;
    // A primitive registration owns a source-TU thunk. The thunk is invoked
    // through the existing builder library during authoring, so a generated
    // interface header never needs to include the provider's source.
    SourceNodeBuildFunction node_build = nullptr;
    void const* node_compiler_record = nullptr;
};

void register_source_definition(SourceRegistrationView);
std::size_t source_definition_count() noexcept;
SourceRegistrationView source_definition_at(std::size_t);
std::size_t source_module_count() noexcept;
SourceRegistrationView source_module_at(std::size_t);
NodeRef author_registered_source_node(GraphBuilder&, std::string_view id);
void clear_source_definitions() noexcept;

class SourceModuleRegistration final {
public:
    SourceModuleRegistration(
        char const* id,
        std::size_t id_size,
        SourceModuleBuildFunction build);
};

class SourceNodeRegistration final {
public:
    SourceNodeRegistration(
        char const* id,
        std::size_t id_size,
        SourceNodeBuildFunction build,
        void const* node_compiler_record);
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

// The generated per-source interface header emits this same declaration for
// imported iv modules.  Keeping it as one macro gives same-source and
// imported callers exactly the same dynamic (NodeRef) contract without ever
// including implementation source.
#define IV_MODULE_INTERFACE(Id, Function) \
    template<> struct iv::node_interface<Id> { \
        template<class... Args> \
        static ::iv::NodeRef author(::iv::GraphBuilder& builder, Args&&...) { \
            static_assert(sizeof...(Args) == 0, \
                "the current IV_MODULE interface accepts no authored arguments"); \
            return builder.template module<&Function>(Id); \
        } \
    }

// IV_NODE interfaces are intentionally zero-argument. Their concrete type
// and compiler record remain in the provider translation unit while the
// shared builder registry dispatches the source-TU authoring thunk.
#define IV_NODE_INTERFACE(Id) \
    template<> struct iv::node_interface<Id> { \
        template<class... Args> \
        static ::iv::NodeRef author(::iv::GraphBuilder& builder, Args&&...) { \
            static_assert(sizeof...(Args) == 0, \
                "the current IV_NODE interface accepts no authored arguments"); \
            return ::iv::details::author_registered_source_node(builder, Id); \
        } \
    }

// Each registration gets its own named constructor entry. The authoring JIT
// runs those entries before it asks for the source definition set; the
// finalizer can then remove precisely these source-only entries without
// touching unrelated user static initialization.
#define IV_MODULE(Id, Function) \
    IV_MODULE_IMPL(Id, Function, __COUNTER__)
#define IV_MODULE_IMPL(Id, Function, Unique) \
    IV_MODULE_INTERFACE(Id, Function); \
    namespace { \
    IV_SOURCE_REGISTRATION_USED IV_SOURCE_REGISTRATION_CONSTRUCTOR \
    void IV_SOURCE_CONCAT(iv_source_module_registration_ctor_, Unique)(); \
    void IV_SOURCE_CONCAT(iv_source_module_registration_ctor_, Unique)() { \
        static const ::iv::details::SourceModuleRegistration \
            IV_SOURCE_CONCAT(iv_source_module_registration_, Unique){ \
                Id, sizeof(Id) - 1, Function}; \
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
    IV_NODE_INTERFACE(Id); \
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
                &::iv::details::node_compiler_record<Node>}; \
    } \
    }
