#pragma once

#include <intravenous/graph/builder/port_refs.h>
#include <intravenous/ports.h>

#include <concepts>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace iv {

struct PortDisplayName {
    constexpr PortDisplayName() = default;
    template<std::convertible_to<std::string_view> Label>
    constexpr PortDisplayName(Label const& short_name) : short_name(short_name) {}
    constexpr PortDisplayName(std::string_view short_name, std::string_view long_name)
        : short_name(short_name), long_name(long_name) {}

    std::string_view short_name{};
    std::string_view long_name{};
};

namespace details {

template<class Access>
concept ModuleInputAccess = std::same_as<Access, InputAccess::Sequential>
    || std::same_as<Access, InputAccess::Region>
    || std::same_as<Access, InputAccess::Coverage>;

template<class Value, ModuleInputAccess Access>
struct ModuleInputOptions;

template<class Value>
struct ModuleInputOptions<Value, InputAccess::Sequential> {
    using type = typename ContinuousValueType<Value>::BoundaryInputOptions;
};
template<class Value>
struct ModuleInputOptions<Value, InputAccess::Region> {
    using type = typename ContinuousValueType<Value>::RegionInputOptions;
};
template<class Value>
struct ModuleInputOptions<Value, InputAccess::Coverage> {
    using type = typename ContinuousValueType<Value>::CoverageInputOptions;
};

template<class Value, ModuleInputAccess Access>
using module_input_options_t = typename ModuleInputOptions<Value, Access>::type;

template<ModuleInputAccess Access>
[[nodiscard]] constexpr InputAccessKind module_input_access_kind() noexcept
{
    if constexpr (std::same_as<Access, InputAccess::Sequential>)
        return InputAccessKind::sequential;
    else if constexpr (std::same_as<Access, InputAccess::Region>)
        return InputAccessKind::region;
    else
        return InputAccessKind::coverage;
}

template<RegisteredContinuousValue... Types, ModuleInputAccess Access>
[[nodiscard]] ContinuousInputConfig make_boundary_input_config(
    PortNames names, Access access,
    module_input_options_t<Types, Access>... options)
    requires(sizeof...(Types) != 0)
{
    auto authored = [&] {
        if constexpr (std::same_as<Access, InputAccess::Sequential>) {
            return iv::input<Types...>(names, access,
                ContinuousValueType<Types>::boundary_input_sequential_options(options)...);
        } else {
            return iv::input<Types...>(names, access, std::move(options)...);
        }
    }();
    return materialize_continuous_config(erase_port_config(authored));
}

template<RegisteredContinuousValue... Types, ModuleInputAccess Access>
[[nodiscard]] ContinuousInputConfig make_boundary_input_config(
    PortNames names, Access access)
    requires(sizeof...(Types) != 0)
{
    return make_boundary_input_config<Types...>(
        names, access, module_input_options_t<Types, Access>{}...);
}

template<RegisteredContinuousValue... Types>
[[nodiscard]] ContinuousInputConfig make_boundary_input_config(
    PortNames names,
    typename ContinuousValueType<Types>::BoundaryInputOptions... options)
    requires(sizeof...(Types) != 0)
{
    return make_boundary_input_config<Types...>(
        names, InputAccess::sequential, std::move(options)...);
}

template<RegisteredContinuousValue... Types>
[[nodiscard]] ContinuousInputConfig make_boundary_input_config(PortNames names)
    requires(sizeof...(Types) != 0)
{
    return make_boundary_input_config<Types...>(names, InputAccess::sequential);
}

template<ModuleInputAccess Access>
[[nodiscard]] ContinuousInputConfig make_default_boundary_input_config(
    PortNames names, ChannelLayout layout, Access access)
{
    module_input_options_t<DefaultContinuousValueType, Access> options{};
    options.channel_layout = layout;
    return make_boundary_input_config<DefaultContinuousValueType>(names, access, options);
}

[[nodiscard]] inline ContinuousInputConfig make_default_boundary_input_config(
    PortNames names, ChannelLayout layout =
        continuous_value_details::default_audio_channel_layout)
{
    return make_default_boundary_input_config(names, layout, InputAccess::sequential);
}

[[nodiscard]] inline ContinuousInputConfig make_sample_boundary_input_config(
    PortNames names, ChannelLayout layout, Sample value,
    std::optional<Sample> min, std::optional<Sample> max)
{
    return make_boundary_input_config<Sample>(names, {
        .channel_layout = layout,
        .default_value = value,
        .min = min.value_or(-std::numeric_limits<Sample::storage>::infinity()),
        .max = max.value_or(std::numeric_limits<Sample::storage>::infinity()),
    });
}

// The package compiler replaces these calls before they reach CodeGen.
[[noreturn]] inline void uninstrumented_module_input()
{
    error("Module input declarations require the IV Clang plugin. For programmatic graphs, use iv::programmatic::declare_input or declare_event_input.");
}

class ModuleInputDeclarations {
public:
    // The Clang plugin lowers each spelling to a PortDisplayName and uses
    // the receiving variable as the port identifier.
#define IV_MODULE_INPUT_EXPAND(...) __VA_ARGS__
#define IV_MODULE_INPUT_COMMA(...) __VA_OPT__(,) __VA_ARGS__
#define IV_MODULE_INPUT_NAMES(Function, Result, Template, Constraint, ...)         \
    IV_MODULE_INPUT_EXPAND Template                                               \
    Result Function(__VA_ARGS__) IV_MODULE_INPUT_EXPAND Constraint               \
    { uninstrumented_module_input(); }                                           \
    IV_MODULE_INPUT_EXPAND Template                                               \
    Result Function([[maybe_unused]] PortDisplayName label IV_MODULE_INPUT_COMMA(__VA_ARGS__)) \
        IV_MODULE_INPUT_EXPAND Constraint                                         \
    { uninstrumented_module_input(); }                                           \
    IV_MODULE_INPUT_EXPAND Template                                               \
    Result Function([[maybe_unused]] std::string_view short_name,                  \
        [[maybe_unused]] std::convertible_to<std::string_view> auto const& long_name \
        IV_MODULE_INPUT_COMMA(__VA_ARGS__)) IV_MODULE_INPUT_EXPAND Constraint     \
    { uninstrumented_module_input(); }
#define IV_MODULE_INPUT_CHANNEL (requires requires { ChannelTypeTraits<ChannelType>::id; })
#define IV_MODULE_INPUT_TYPES (requires(sizeof...(Types) != 0))

    IV_MODULE_INPUT_NAMES(input, TypedPublicSampleInputRef<ChannelType>,
        (template<class ChannelType = mono>), IV_MODULE_INPUT_CHANNEL)
    IV_MODULE_INPUT_NAMES(input, TypedPublicSampleInputRef<ChannelType>,
        (template<class ChannelType = mono>), IV_MODULE_INPUT_CHANNEL,
        Sample, std::optional<Sample> = {}, std::optional<Sample> = {})
    IV_MODULE_INPUT_NAMES(input, PublicSampleInputRef, (template<RegisteredContinuousValue... Types>), IV_MODULE_INPUT_TYPES)
    IV_MODULE_INPUT_NAMES(input, PublicSampleInputRef, (template<RegisteredContinuousValue... Types>),
        IV_MODULE_INPUT_TYPES, typename ContinuousValueType<Types>::BoundaryInputOptions...)
    IV_MODULE_INPUT_NAMES(input, TypedPublicSampleInputRef<ChannelType>,
        (template<class ChannelType = mono, ModuleInputAccess Access>), IV_MODULE_INPUT_CHANNEL, Access)
    IV_MODULE_INPUT_NAMES(input, PublicSampleInputRef,
        (template<RegisteredContinuousValue... Types, ModuleInputAccess Access>), IV_MODULE_INPUT_TYPES, Access)
    IV_MODULE_INPUT_NAMES(input, PublicSampleInputRef,
        (template<RegisteredContinuousValue... Types, ModuleInputAccess Access>), IV_MODULE_INPUT_TYPES,
        Access, module_input_options_t<Types, Access>...)
    IV_MODULE_INPUT_NAMES(event_input, PublicEventInputRef, (), (), EventTypeId)
    IV_MODULE_INPUT_NAMES(event_input, PublicEventInputRef, (template<ModuleInputAccess Access>), (), EventTypeId, Access)

#undef IV_MODULE_INPUT_TYPES
#undef IV_MODULE_INPUT_CHANNEL
#undef IV_MODULE_INPUT_NAMES
#undef IV_MODULE_INPUT_COMMA
#undef IV_MODULE_INPUT_EXPAND
};

struct ProgrammaticInputAccess;

} // namespace details
} // namespace iv
