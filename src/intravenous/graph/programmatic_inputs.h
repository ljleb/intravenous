#pragma once

#include <intravenous/graph/builder.h>

namespace iv::details {

template<class Builder>
concept ModuleInputBuilder =
    std::same_as<Builder, GraphBuilder> || std::same_as<Builder, SubgraphBuilder>;

struct ProgrammaticInputAccess {
    static auto create(ModuleInputBuilder auto& builder, auto config) {
        return builder.create_input(std::move(config));
    }
};

template<class ChannelType>
constexpr ChannelLayout module_input_layout() {
    return {ChannelTypeTraits<ChannelType>::id, SampleStreamLayout::planar};
}

inline PortNames boundary_input_names(std::string_view identifier, PortDisplayName label)
{
    if (identifier.empty() && label.short_name.empty() && label.long_name.empty())
        return PortNames::generated();
    return {identifier, label.short_name, label.long_name};
}

} // namespace iv::details

namespace iv::programmatic {

template<class ChannelType = mono, details::ModuleInputBuilder Builder>
    requires requires { ChannelTypeTraits<ChannelType>::id; }
TypedPublicSampleInputRef<ChannelType> declare_input(Builder& builder, std::string_view name, PortDisplayName label) {
    return TypedPublicSampleInputRef<ChannelType>{details::ProgrammaticInputAccess::create(
        builder, details::make_default_boundary_input_config(
            details::boundary_input_names(name, label), details::module_input_layout<ChannelType>()))};
}

template<class ChannelType = mono, details::ModuleInputBuilder Builder>
    requires requires { ChannelTypeTraits<ChannelType>::id; }
TypedPublicSampleInputRef<ChannelType> declare_input(Builder& builder, std::string_view name, PortDisplayName label,
    Sample value, std::optional<Sample> min = {}, std::optional<Sample> max = {}) {
    return TypedPublicSampleInputRef<ChannelType>{details::ProgrammaticInputAccess::create(
        builder, details::make_sample_boundary_input_config(
            details::boundary_input_names(name, label), details::module_input_layout<ChannelType>(), value, min, max))};
}

template<RegisteredContinuousValue... Types, details::ModuleInputBuilder Builder>
    requires(sizeof...(Types) != 0)
PublicSampleInputRef declare_input(Builder& builder, std::string_view name, PortDisplayName label) {
    return details::ProgrammaticInputAccess::create(
        builder, details::make_boundary_input_config<Types...>(details::boundary_input_names(name, label)));
}

template<RegisteredContinuousValue... Types, details::ModuleInputBuilder Builder>
    requires(sizeof...(Types) != 0)
PublicSampleInputRef declare_input(Builder& builder, std::string_view name, PortDisplayName label,
    typename ContinuousValueType<Types>::BoundaryInputOptions... options) {
    return details::ProgrammaticInputAccess::create(
        builder, details::make_boundary_input_config<Types...>(details::boundary_input_names(name, label), std::move(options)...));
}

template<class ChannelType = mono, details::ModuleInputBuilder Builder, details::ModuleInputAccess Access>
    requires requires { ChannelTypeTraits<ChannelType>::id; }
TypedPublicSampleInputRef<ChannelType> declare_input(Builder& builder, std::string_view name, PortDisplayName label, Access access) {
    return TypedPublicSampleInputRef<ChannelType>{details::ProgrammaticInputAccess::create(
        builder, details::make_default_boundary_input_config(
            details::boundary_input_names(name, label), details::module_input_layout<ChannelType>(), access))};
}

template<RegisteredContinuousValue... Types, details::ModuleInputBuilder Builder, details::ModuleInputAccess Access>
    requires(sizeof...(Types) != 0)
PublicSampleInputRef declare_input(Builder& builder, std::string_view name, PortDisplayName label, Access access) {
    return details::ProgrammaticInputAccess::create(
        builder, details::make_boundary_input_config<Types...>(details::boundary_input_names(name, label), access));
}

template<RegisteredContinuousValue... Types, details::ModuleInputBuilder Builder, details::ModuleInputAccess Access>
    requires(sizeof...(Types) != 0)
PublicSampleInputRef declare_input(Builder& builder, std::string_view name, PortDisplayName label, Access access,
    details::module_input_options_t<Types, Access>... options) {
    return details::ProgrammaticInputAccess::create(builder,
        details::make_boundary_input_config<Types...>(details::boundary_input_names(name, label), access, std::move(options)...));
}

template<details::ModuleInputBuilder Builder, details::ModuleInputAccess Access>
PublicEventInputRef declare_event_input(Builder& builder, std::string_view name, PortDisplayName label, EventTypeId type,
    Access) {
    auto const names = details::boundary_input_names(name, label);
    return details::ProgrammaticInputAccess::create(builder, EventInputConfig{
        .name = std::string(names.identifier), .type = type, .access = details::module_input_access_kind<Access>(),
        .display_name = std::string(names.display), .short_display_name = std::string(names.short_display),
    });
}

template<details::ModuleInputBuilder Builder>
PublicEventInputRef declare_event_input(Builder& builder, std::string_view name, PortDisplayName label, EventTypeId type) {
    return declare_event_input(builder, name, label, type, InputAccess::sequential);
}

} // namespace iv::programmatic
