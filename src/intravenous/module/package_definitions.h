#pragma once

// Stable package definitions emitted by IV_NODE / IV_MODULE. Definitions are
// data, not a process-global side effect: the finalizer collects these records
// into each IV package, and BuilderSession receives the records from the
// loaded IV packages used for one graph configuration.

#include <intravenous/graph/builder/syntax.h>
#include <intravenous/graph/builder/node_refs.h>
#include <intravenous/channel_layout.h>
#include <intravenous/module/configuration_argument.h>
#include <intravenous/node/code_key.h>
#include <intravenous/node/traits.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace iv {
class GraphBuilder;
class NodeRef;

namespace details {
enum class PackageDefinitionKind {
    node,
    module,
};

// The registration ABI exposes the provider's construction interface without
// exposing C++ declarations to a consumer. `parameter_types` contains the
// canonical transported type of every public parameter in order. The leading
// GraphBuilder& of an IV_MODULE is deliberately not represented here.
struct RegisteredSignature {
    ConfigurationTypeIdentity const* const* parameter_types = nullptr;
    ConfigurationValueOperations const* const* parameter_operations = nullptr;
    std::size_t required_argument_count = 0;
    std::size_t argument_count = 0;
};
static_assert(std::is_standard_layout_v<RegisteredSignature>);
static_assert(std::is_trivially_copyable_v<RegisteredSignature>);

using IvModuleConfigureFunction = void (*)(
    GraphBuilder&, std::span<ConfigurationArgument>);
using NodeTypeConfigureFunction = NodeRef (*)(
    GraphBuilder&, std::span<ConfigurationArgument>, ChannelLayout const*);
using RegisteredSignatureFunction = RegisteredSignature const* (*)();

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
    RegisteredSignatureFunction signature = nullptr;
};

static_assert(std::is_standard_layout_v<PackageDefinition>);
static_assert(std::is_trivially_copyable_v<PackageDefinition>);

// The compiler-only record section must not share the exported accessor's
// symbol name. LLVM emits a section symbol for named sections on ELF; using
// "iv_package_definitions" for both produces an MC "invalid symbol
// redefinition" error when ORC materializes the package.
#define IV_PACKAGE_DEFINITION_SECTION_NAME "iv_package_definition_records"

inline constexpr std::string_view package_definition_section =
    IV_PACKAGE_DEFINITION_SECTION_NAME;
inline constexpr std::string_view package_definitions_abi_symbol =
    "iv_package_definitions";
static_assert(package_definition_section != package_definitions_abi_symbol);

// g.node<"id"> resolves synchronously through the definitions attached to
// this GraphBuilder's BuilderSession. An iv module is fully configured before
// this function returns; no unresolved package-definition node is preserved.
NodeRef configure_package_definition(
    GraphBuilder&, std::string_view id, std::span<ConfigurationArgument> arguments);
NodeRef configure_tiled_package_definition(
    GraphBuilder&, std::string_view id, ChannelLayout,
    std::span<ConfigurationArgument> arguments);

// This is the private concrete-node construction primitive. Registered node
// adapters use it after validating their erased arguments; source-facing code
// uses only GraphBuilder::node<"stable.id">(...).
template<class Node, class... Args>
node_ref_for_t<Node> configure_concrete_node(GraphBuilder&, Args&&...);
template<class Node, class... Args>
NodeRef configure_concrete_node_tiled(
    GraphBuilder&, ChannelLayout, Args&&...);

inline std::string describe_configuration_type(
    ConfigurationTypeIdentity const* identity)
{
    auto const display = configuration_type_display_name(identity);
    if (!display.empty()) return std::string(display);
    auto const nominal = configuration_type_nominal_id(identity);
    return nominal.empty() ? std::string("<unavailable type identity>")
                           : std::string(nominal);
}

[[noreturn]] inline void throw_configuration_argument_count_mismatch(
    std::string_view id,
    RegisteredSignature const& signature,
    std::size_t supplied)
{
    throw std::invalid_argument(
        "registered definition '" + std::string(id)
        + "' expects between "
        + std::to_string(signature.required_argument_count) + " and "
        + std::to_string(signature.argument_count)
        + " configuration arguments, but received " + std::to_string(supplied));
}

[[noreturn]] inline void throw_configuration_argument_type_mismatch(
    std::string_view id,
    std::size_t index,
    ConfigurationTypeIdentity const* expected,
    ConfigurationTypeIdentity const* actual)
{
    throw std::invalid_argument(
        "registered definition '" + std::string(id)
        + "' configuration argument " + std::to_string(index)
        + " has type '" + describe_configuration_type(actual)
        + "', but the provider requires '" + describe_configuration_type(expected)
        + "' (registered-ID argument conversion is not supported)");
}

inline void validate_registered_signature_shape(
    std::string_view id,
    RegisteredSignature const& signature)
{
    if (signature.required_argument_count > signature.argument_count
        || (signature.argument_count != 0
            && (!signature.parameter_types || !signature.parameter_operations))) {
        throw std::logic_error(
            "registered definition '" + std::string(id)
            + "' published an invalid construction signature");
    }
}

inline void validate_registered_signature(
    std::string_view id,
    RegisteredSignature const& signature,
    std::span<ConfigurationArgument> arguments)
{
    validate_registered_signature_shape(id, signature);
    if (arguments.size() < signature.required_argument_count
        || arguments.size() > signature.argument_count) {
        throw_configuration_argument_count_mismatch(id, signature, arguments.size());
    }
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        auto const* expected = signature.parameter_types[index];
        auto const* actual = arguments[index].type;
        if (!arguments[index].data || !same_configuration_type(expected, actual)) {
            throw_configuration_argument_type_mismatch(id, index, expected, actual);
        }
    }
}

template<std::size_t Required, class... Args>
struct RegisteredSignatureStorage {
    static_assert(Required <= sizeof...(Args));

    inline static constexpr std::array<ConfigurationTypeIdentity const*, sizeof...(Args)>
        parameter_types{configuration_type_identity<std::remove_cvref_t<Args>>()...};
    inline static constexpr std::array<ConfigurationValueOperations const*, sizeof...(Args)>
        parameter_operations{configuration_value_operations<std::remove_cvref_t<Args>>()...};
    inline static constexpr RegisteredSignature value{
        .parameter_types = parameter_types.data(),
        .parameter_operations = parameter_operations.data(),
        .required_argument_count = Required,
        .argument_count = sizeof...(Args),
    };
};

template<class... Args>
struct ErasedConfigurationArguments {
    using Tuple = std::tuple<Args...>;

    template<class Arg>
    static decltype(auto) restore(ConfigurationArgument& value)
    {
        using T = std::remove_cvref_t<Arg>;
        auto const* expected = configuration_type_identity<T>();
        if (!value.data || !same_configuration_type(expected, value.type)) {
            throw_configuration_argument_type_mismatch(
                "<provider adapter>", 0, expected, value.type);
        }
        if (value.is_array_decay) {
            // A string literal (and another array expression) reaches an
            // ordinary pointer-by-value parameter through C++'s built-in
            // array-to-pointer conversion. It has no pointer object that a
            // provider may safely bind by reference, so keep that distinction
            // explicit rather than pretending an array owns one.
            if constexpr (std::is_pointer_v<T>
                && !std::is_reference_v<Arg>) {
                return static_cast<T>(value.data);
            } else {
                throw std::invalid_argument(
                    "registered definition '<provider adapter>' cannot bind an "
                    "array-decayed configuration argument by reference");
            }
        }
        if constexpr (std::is_lvalue_reference_v<Arg>) {
            if constexpr (!std::is_const_v<std::remove_reference_t<Arg>>) {
                if (value.is_const) {
                    throw std::invalid_argument(
                        "registered definition '<provider adapter>' cannot bind a "
                        "const configuration object to a mutable reference");
                }
                return static_cast<Arg>(*static_cast<T*>(value.data));
            } else if (value.is_const) {
                return static_cast<Arg>(*static_cast<T const*>(value.data));
            } else {
                return static_cast<Arg>(*static_cast<T*>(value.data));
            }
        } else if constexpr (std::is_rvalue_reference_v<Arg>) {
            if (value.is_const || !value.is_rvalue) {
                throw std::invalid_argument(
                    "registered definition '<provider adapter>' cannot bind this "
                    "configuration object to an rvalue reference");
            }
            return static_cast<Arg>(std::move(*static_cast<T*>(value.data)));
        } else {
            if (value.is_const || !value.is_rvalue) {
                if constexpr (std::copy_constructible<T>) {
                    return T(*static_cast<T const*>(value.data));
                } else {
                    throw std::invalid_argument(
                        "registered definition '<provider adapter>' cannot copy "
                        "this non-copyable configuration object from an lvalue");
                }
            }
            return T(std::move(*static_cast<T*>(value.data)));
        }
    }

    template<class Invoker, class... Prefix>
    static constexpr bool accepts_prefix = requires(GraphBuilder& builder) {
        Invoker::template invoke<Prefix...>(
            builder, std::declval<Prefix>()...);
    };

    template<class Invoker, std::size_t... Index>
    static consteval bool accepts_prefix_impl(std::index_sequence<Index...>)
    {
        return accepts_prefix<Invoker, std::tuple_element_t<Index, Tuple>...>;
    }

    template<class Invoker, std::size_t Count>
    static consteval bool accepts_prefix_count()
    {
        return accepts_prefix_impl<Invoker>(std::make_index_sequence<Count>{});
    }

    template<class Invoker, std::size_t Count = 0>
    static consteval std::size_t first_accepted_prefix()
    {
        if constexpr (Count > sizeof...(Args)) {
            return sizeof...(Args) + 1;
        } else if constexpr (accepts_prefix_count<Invoker, Count>()) {
            return Count;
        } else {
            return first_accepted_prefix<Invoker, Count + 1>();
        }
    }

    template<class Invoker, std::size_t... Index>
    static decltype(auto) invoke_prefix(
        GraphBuilder& builder,
        std::span<ConfigurationArgument> arguments,
        std::index_sequence<Index...>)
    {
        return Invoker::template invoke<std::tuple_element_t<Index, Tuple>...>(
            builder,
            restore<std::tuple_element_t<Index, Tuple>>(arguments[Index])...);
    }

    template<class Invoker, class First, std::size_t... Index>
    static decltype(auto) invoke_prefix_with(
        GraphBuilder& builder,
        First&& first,
        std::span<ConfigurationArgument> arguments,
        std::index_sequence<Index...>)
    {
        return Invoker::template invoke<std::tuple_element_t<Index, Tuple>...>(
            builder,
            std::forward<First>(first),
            restore<std::tuple_element_t<Index, Tuple>>(arguments[Index])...);
    }
};

template<auto Function, class DirectInvoker>
struct IvModuleConfigureAdapter;

template<class Return, class... Args, Return (*Function)(GraphBuilder&, Args...),
         class DirectInvoker>
struct IvModuleConfigureAdapter<Function, DirectInvoker> {
    static_assert(std::is_void_v<Return>,
        "an IV module configuration function must return void");
    using Erased = ErasedConfigurationArguments<Args...>;
    static constexpr auto required_argument_count =
        Erased::template first_accepted_prefix<DirectInvoker>();
    static_assert(required_argument_count <= sizeof...(Args),
        "IV_MODULE has no valid provider-side configuration call");

    static RegisteredSignature const* signature()
    {
        return std::addressof(
            RegisteredSignatureStorage<required_argument_count, Args...>::value);
    }

    template<std::size_t Count = 0>
    static void dispatch(
        GraphBuilder& builder,
        std::span<ConfigurationArgument> arguments)
    {
        if (arguments.size() == Count) {
            if constexpr (Erased::template accepts_prefix_count<DirectInvoker, Count>()) {
                Erased::template invoke_prefix<DirectInvoker>(
                    builder, arguments, std::make_index_sequence<Count>{});
                return;
            } else {
                throw_configuration_argument_count_mismatch(
                    "<provider adapter>", *signature(), arguments.size());
            }
        }
        if constexpr (Count < sizeof...(Args)) {
            dispatch<Count + 1>(builder, arguments);
        } else {
            throw_configuration_argument_count_mismatch(
                "<provider adapter>", *signature(), arguments.size());
        }
    }

    static void configure(
        GraphBuilder& builder,
        std::span<ConfigurationArgument> arguments)
    {
        dispatch(builder, arguments);
    }
};

template<class Node, class... Args>
struct NodeTypeConfigureAdapter {
    struct DirectInvoker {
        template<class... ConstructorArgs>
        requires std::constructible_from<Node, ConstructorArgs...>
        static NodeRef invoke(GraphBuilder& builder, ConstructorArgs&&... arguments)
        {
            auto node = configure_concrete_node<Node>(
                builder,
                std::forward<ConstructorArgs>(arguments)...);
            return node.node_ref();
        }
    };

    struct TiledInvoker {
        template<class... ConstructorArgs>
        requires std::constructible_from<Node, ConstructorArgs...>
        static NodeRef invoke(
            GraphBuilder& builder,
            ChannelLayout layout,
            ConstructorArgs&&... arguments)
        {
            return configure_concrete_node_tiled<Node>(
                builder,
                layout,
                std::forward<ConstructorArgs>(arguments)...);
        }
    };

    using Erased = ErasedConfigurationArguments<Args...>;
    static constexpr auto required_argument_count =
        Erased::template first_accepted_prefix<DirectInvoker>();
    static_assert(required_argument_count <= sizeof...(Args),
        "IV_NODE constructor has no valid provider-side configuration call");

    static RegisteredSignature const* signature()
    {
        return std::addressof(
            RegisteredSignatureStorage<required_argument_count, Args...>::value);
    }

    template<std::size_t Count = 0>
    static NodeRef dispatch(
        GraphBuilder& builder,
        std::span<ConfigurationArgument> arguments)
    {
        if (arguments.size() == Count) {
            if constexpr (Erased::template accepts_prefix_count<DirectInvoker, Count>()) {
                return Erased::template invoke_prefix<DirectInvoker>(
                    builder, arguments, std::make_index_sequence<Count>{});
            } else {
                throw_configuration_argument_count_mismatch(
                    "<provider adapter>", *signature(), arguments.size());
            }
        }
        if constexpr (Count < sizeof...(Args)) {
            return dispatch<Count + 1>(builder, arguments);
        } else {
            throw_configuration_argument_count_mismatch(
                "<provider adapter>", *signature(), arguments.size());
        }
    }

    static NodeRef configure(
        GraphBuilder& builder,
        std::span<ConfigurationArgument> arguments,
        ChannelLayout const* tiled_layout)
    {
        if (!tiled_layout) return dispatch(builder, arguments);
        return dispatch_tiled(builder, arguments, *tiled_layout);
    }

    template<std::size_t Count = 0>
    static NodeRef dispatch_tiled(
        GraphBuilder& builder,
        std::span<ConfigurationArgument> arguments,
        ChannelLayout layout)
    {
        if (arguments.size() == Count) {
            if constexpr (Erased::template accepts_prefix_count<DirectInvoker, Count>()) {
                return Erased::template invoke_prefix_with<TiledInvoker>(
                    builder, layout, arguments, std::make_index_sequence<Count>{});
            } else {
                throw_configuration_argument_count_mismatch(
                    "<provider adapter>", *signature(), arguments.size());
            }
        }
        if constexpr (Count < sizeof...(Args)) {
            return dispatch_tiled<Count + 1>(builder, arguments, layout);
        } else {
            throw_configuration_argument_count_mismatch(
                "<provider adapter>", *signature(), arguments.size());
        }
    }
};

// These templates are selected by the IV Clang plugin after it has identified
// IV_NODE's one visible constructor. Keeping the generated call in the
// provider package lets normal C++ default arguments be evaluated there.
template<class Node, class... Args>
NodeRef configure_node_constructor(
    GraphBuilder& builder,
    std::span<ConfigurationArgument> arguments,
    ChannelLayout const* tiled_layout)
{
    return NodeTypeConfigureAdapter<Node, Args...>::configure(
        builder, arguments, tiled_layout);
}

template<class Node, class... Args>
RegisteredSignature const* node_constructor_signature()
{
    return NodeTypeConfigureAdapter<Node, Args...>::signature();
}
} // namespace details
} // namespace iv

#if defined(__clang__)
#define IV_PACKAGE_DEFINITION_RECORD \
    __attribute__((used, section(IV_PACKAGE_DEFINITION_SECTION_NAME)))
#else
#error "IV package definition requires Clang"
#endif

#define IV_PACKAGE_CONCAT_INNER(a, b) a##b
#define IV_PACKAGE_CONCAT(a, b) IV_PACKAGE_CONCAT_INNER(a, b)

#define IV_MODULE(Id, Function) \
    IV_MODULE_IMPL(Id, Function, __COUNTER__)
#define IV_MODULE_IMPL(Id, Function, Unique) \
    namespace { \
    struct IV_PACKAGE_CONCAT(iv_package_module_invoker_, Unique) { \
        template<class... Args> \
        requires requires(::iv::GraphBuilder& builder, Args&&... arguments) { \
            Function(builder, std::forward<Args>(arguments)...); \
        } \
        static void invoke(::iv::GraphBuilder& builder, Args&&... arguments) { \
            Function(builder, std::forward<Args>(arguments)...); \
        } \
    }; \
    IV_PACKAGE_DEFINITION_RECORD constinit const \
        ::iv::details::PackageDefinition \
        IV_PACKAGE_CONCAT(iv_package_module_definition_, Unique){ \
            ::iv::details::PackageDefinitionKind::module, \
            Id, sizeof(Id) - 1, \
            __FILE__, sizeof(__FILE__) - 1, \
            "", 0, \
            &::iv::details::IvModuleConfigureAdapter< \
                Function, IV_PACKAGE_CONCAT(iv_package_module_invoker_, Unique)>::configure, \
            nullptr, nullptr, \
            &::iv::details::IvModuleConfigureAdapter< \
                Function, IV_PACKAGE_CONCAT(iv_package_module_invoker_, Unique)>::signature}; \
    }

#define IV_NODE(Id, Node) \
    IV_NODE_IMPL(Id, Node, __COUNTER__)

#define IV_NODE_VALIDATE_DECLARATION(Node) \
    static_assert(::iv::details::has_constexpr_port_configs<Node>, \
        "IV_NODE requires inputs() and outputs() to return static constexpr arrays of InputConfig and OutputConfig. " \
        "Each array carries both sample and event ports through its config variant. " \
        "Dynamic-arity or configuration-dependent nodes must remain internal lowering nodes."); \
    static_assert(::iv::details::replay_declaration_is_valid_v<Node>, \
        "IV_NODE intrinsically replayable nodes must author tick() (not tick_block()), " \
        "have no State/TockState or RandomAccess inputs, and declare only " \
        "pointwise Sequential inputs and Tick outputs with zero history/latency."); \
    static_assert(::iv::details::background_state_type_is_valid_v<Node>, \
        "IV_NODE Node::TockState must be a mutable, non-volatile, default-constructible object type."); \
    static_assert(!::iv::details::declares_tock_outputs_v<Node> \
            || ::iv::details::has_tock_coverage<Node>, \
        "IV_NODE node type declares a Tock output port; define tock_coverage(TockCoverageContext<Node>&)."); \
    static_assert(!::iv::details::has_tock_coverage<Node> \
            || ::iv::details::declares_tock_outputs_v<Node>, \
        "IV_NODE node type defines tock_coverage but declares no Tock output port."); \
    static_assert(!::iv::details::declares_tock_outputs_v<Node> \
            || ::iv::details::has_propagate_forward_coverage<Node>, \
        "IV_NODE node type declares a Tock output port; define exact propagate_forward_coverage(PropagateForwardCoverageContext<Node>&)."); \
    static_assert(!::iv::details::has_propagate_forward_coverage<Node> \
            || ::iv::details::declares_tock_outputs_v<Node>, \
        "IV_NODE node type defines propagate_forward_coverage but declares no Tock output port."); \
    static_assert(!::iv::details::has_propagate_reverse_coverage<Node> \
            || (::iv::details::declares_tock_outputs_v<Node> \
                && ::iv::details::declares_random_access_inputs_v<Node>), \
        "IV_NODE node type defines propagate_reverse_coverage but does not declare both random-access input and Tock output ports.")

#define IV_NODE_IMPL(Id, Node, Unique) \
    namespace { \
    IV_NODE_VALIDATE_DECLARATION(Node); \
    ::iv::NodeRef IV_PACKAGE_CONCAT(iv_package_node_configuration_pending_, Unique)( \
        ::iv::GraphBuilder&, std::span<::iv::details::ConfigurationArgument>, \
        ::iv::ChannelLayout const*) { \
        throw std::logic_error("IV_NODE constructor adapter was not generated by the IV Clang plugin"); \
    } \
    ::iv::details::RegisteredSignature const* \
        IV_PACKAGE_CONCAT(iv_package_node_signature_pending_, Unique)() { \
        throw std::logic_error("IV_NODE signature was not generated by the IV Clang plugin"); \
    } \
    IV_PACKAGE_DEFINITION_RECORD constinit const \
        ::iv::details::PackageDefinition \
        IV_PACKAGE_CONCAT(iv_package_node_definition_, Unique){ \
            ::iv::details::PackageDefinitionKind::node, \
            Id, sizeof(Id) - 1, \
            __FILE__, sizeof(__FILE__) - 1, \
            "", 0, \
            nullptr, &IV_PACKAGE_CONCAT(iv_package_node_configuration_pending_, Unique), \
            &::iv::details::node_compiler_record<Node>, \
            &IV_PACKAGE_CONCAT(iv_package_node_signature_pending_, Unique)}; \
    }
