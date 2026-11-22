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

} // namespace iv::details

namespace iv::programmatic {

template<class ChannelType = mono, details::ModuleInputBuilder Builder>
    requires requires { ChannelTypeTraits<ChannelType>::id; }
TypedPublicSampleInputRef<ChannelType> declare_input(Builder& builder, std::string_view name) {
    return TypedPublicSampleInputRef<ChannelType>{details::ProgrammaticInputAccess::create(
        builder, details::make_default_boundary_input_config(
            name, details::module_input_layout<ChannelType>()))};
}

template<class ChannelType = mono, details::ModuleInputBuilder Builder>
    requires requires { ChannelTypeTraits<ChannelType>::id; }
TypedPublicSampleInputRef<ChannelType> declare_input(Builder& builder, std::string_view name,
    Sample value, std::optional<Sample> min = {}, std::optional<Sample> max = {}) {
    return TypedPublicSampleInputRef<ChannelType>{details::ProgrammaticInputAccess::create(
        builder, details::make_sample_boundary_input_config(
            name, details::module_input_layout<ChannelType>(), value, min, max))};
}

template<RegisteredContinuousValue... Types, details::ModuleInputBuilder Builder>
    requires(sizeof...(Types) != 0)
PublicSampleInputRef declare_input(Builder& builder, std::string_view name) {
    return details::ProgrammaticInputAccess::create(
        builder, details::make_boundary_input_config<Types...>(name));
}

template<RegisteredContinuousValue... Types, details::ModuleInputBuilder Builder>
    requires(sizeof...(Types) != 0)
PublicSampleInputRef declare_input(Builder& builder, std::string_view name,
    typename ContinuousValueType<Types>::BoundaryInputOptions... options) {
    return details::ProgrammaticInputAccess::create(
        builder, details::make_boundary_input_config<Types...>(name, std::move(options)...));
}

template<class ChannelType = mono, details::ModuleInputBuilder Builder, details::ModuleInputAccess Access>
    requires requires { ChannelTypeTraits<ChannelType>::id; }
TypedPublicSampleInputRef<ChannelType> declare_input(Builder& builder, std::string_view name, Access access) {
    return TypedPublicSampleInputRef<ChannelType>{details::ProgrammaticInputAccess::create(
        builder, details::make_default_boundary_input_config(
            name, details::module_input_layout<ChannelType>(), access))};
}

template<RegisteredContinuousValue... Types, details::ModuleInputBuilder Builder, details::ModuleInputAccess Access>
    requires(sizeof...(Types) != 0)
PublicSampleInputRef declare_input(Builder& builder, std::string_view name, Access access) {
    return details::ProgrammaticInputAccess::create(
        builder, details::make_boundary_input_config<Types...>(name, access));
}

template<RegisteredContinuousValue... Types, details::ModuleInputBuilder Builder, details::ModuleInputAccess Access>
    requires(sizeof...(Types) != 0)
PublicSampleInputRef declare_input(Builder& builder, std::string_view name, Access access,
    details::module_input_options_t<Types, Access>... options) {
    return details::ProgrammaticInputAccess::create(builder,
        details::make_boundary_input_config<Types...>(name, access, std::move(options)...));
}

template<details::ModuleInputBuilder Builder, details::ModuleInputAccess Access = InputAccess::Sequential>
PublicEventInputRef declare_event_input(Builder& builder, std::string_view name, EventTypeId type,
    Access = {}) {
    return details::ProgrammaticInputAccess::create(builder, EventInputConfig{
        .name = std::string(name), .type = type, .access = details::module_input_access_kind<Access>(),
    });
}

} // namespace iv::programmatic
