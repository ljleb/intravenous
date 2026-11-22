#pragma once

#include <intravenous/graph/builder/port_refs.h>
#include <intravenous/ports.h>

#include <concepts>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace iv::details {

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
    std::string_view name, Access access,
    module_input_options_t<Types, Access>... options)
    requires(sizeof...(Types) != 0)
{
    auto authored = [&] {
        if constexpr (std::same_as<Access, InputAccess::Sequential>) {
            return iv::input<Types...>(std::string(name), access,
                ContinuousValueType<Types>::boundary_input_sequential_options(options)...);
        } else {
            return iv::input<Types...>(std::string(name), access, std::move(options)...);
        }
    }();
    return materialize_continuous_config(erase_port_config(authored));
}

template<RegisteredContinuousValue... Types, ModuleInputAccess Access>
[[nodiscard]] ContinuousInputConfig make_boundary_input_config(
    std::string_view name, Access access)
    requires(sizeof...(Types) != 0)
{
    return make_boundary_input_config<Types...>(
        name, access, module_input_options_t<Types, Access>{}...);
}

template<RegisteredContinuousValue... Types>
[[nodiscard]] ContinuousInputConfig make_boundary_input_config(
    std::string_view name,
    typename ContinuousValueType<Types>::BoundaryInputOptions... options)
    requires(sizeof...(Types) != 0)
{
    return make_boundary_input_config<Types...>(
        name, InputAccess::sequential, std::move(options)...);
}

template<RegisteredContinuousValue... Types>
[[nodiscard]] ContinuousInputConfig make_boundary_input_config(std::string_view name)
    requires(sizeof...(Types) != 0)
{
    return make_boundary_input_config<Types...>(name, InputAccess::sequential);
}

template<ModuleInputAccess Access>
[[nodiscard]] ContinuousInputConfig make_default_boundary_input_config(
    std::string_view name, ChannelLayout layout, Access access)
{
    module_input_options_t<DefaultContinuousValueType, Access> options{};
    options.channel_layout = layout;
    return make_boundary_input_config<DefaultContinuousValueType>(name, access, options);
}

[[nodiscard]] inline ContinuousInputConfig make_default_boundary_input_config(
    std::string_view name, ChannelLayout layout =
        continuous_value_details::default_audio_channel_layout)
{
    return make_default_boundary_input_config(name, layout, InputAccess::sequential);
}

[[nodiscard]] inline ContinuousInputConfig make_sample_boundary_input_config(
    std::string_view name, ChannelLayout layout, Sample value,
    std::optional<Sample> min, std::optional<Sample> max)
{
    return make_boundary_input_config<Sample>(name, {
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
    template<class ChannelType = mono>
        requires requires { ChannelTypeTraits<ChannelType>::id; }
    TypedPublicSampleInputRef<ChannelType> input() {
        uninstrumented_module_input();
    }

    template<class ChannelType = mono>
        requires requires { ChannelTypeTraits<ChannelType>::id; }
    TypedPublicSampleInputRef<ChannelType> input(
        Sample, std::optional<Sample> = {}, std::optional<Sample> = {}) {
        uninstrumented_module_input();
    }

    template<RegisteredContinuousValue... Types>
        requires(sizeof...(Types) != 0)
    PublicSampleInputRef input() {
        uninstrumented_module_input();
    }

    template<RegisteredContinuousValue... Types>
        requires(sizeof...(Types) != 0)
    PublicSampleInputRef input(typename ContinuousValueType<Types>::BoundaryInputOptions...) {
        uninstrumented_module_input();
    }

    template<class ChannelType = mono, ModuleInputAccess Access>
        requires requires { ChannelTypeTraits<ChannelType>::id; }
    TypedPublicSampleInputRef<ChannelType> input(Access) {
        uninstrumented_module_input();
    }

    template<RegisteredContinuousValue... Types, ModuleInputAccess Access>
        requires(sizeof...(Types) != 0)
    PublicSampleInputRef input(Access) {
        uninstrumented_module_input();
    }

    template<RegisteredContinuousValue... Types, ModuleInputAccess Access>
        requires(sizeof...(Types) != 0)
    PublicSampleInputRef input(Access, module_input_options_t<Types, Access>...) {
        uninstrumented_module_input();
    }

    PublicEventInputRef event_input(EventTypeId) {
        uninstrumented_module_input();
    }

    template<ModuleInputAccess Access>
    PublicEventInputRef event_input(EventTypeId, Access) {
        uninstrumented_module_input();
    }
};

struct ProgrammaticInputAccess;

} // namespace iv::details
