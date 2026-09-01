#pragma once

#include <intravenous/graph/static_storage.hpp>
#include <intravenous/graph/wiring.h>
#include <intravenous/node/lifecycle.h>

#include <algorithm>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace iv {
    // Graph port-data nodes are execution storage, not reflective ports.  By
    // the time a graph reaches this representation its input name, range, and
    // other authoring-only attributes have already been consumed.  Keeping a
    // full StaticInputConfig here duplicated every private input in the
    // generated object and inflated constexpr static promotion substantially.
    struct GraphPortStorageConfig {
        ChannelLayout channel_layout {
            .channel_type = ChannelTypeId::mono,
            .sample_layout = SampleStreamLayout::planar,
        };
        size_t history = 0;
        Sample default_value = 0.0f;
    };

    struct GraphPortDataNode {
        StaticString _port_data_id;
        GraphPortStorageConfig _storage;
        InputPortPlan _input_plan;
        StaticSpan<StaticString> _aliases {};
        bool _owns_storage = true;
        bool _is_static_constant = false;

        static consteval StaticSpan<StaticString> freeze_aliases(
            std::span<std::string const> aliases)
        {
            std::vector<StaticString> result;
            result.reserve(aliases.size());
            for (auto const& alias : aliases) {
                result.push_back(details::define_static_string(alias));
            }
            return details::define_static_span(result);
        }

        consteval explicit GraphPortDataNode(
            std::string port_data_id,
            GraphPortStorageConfig storage,
            InputPortPlan input_plan,
            std::vector<std::string> aliases = {},
            bool owns_storage = true,
            bool is_static_constant = false
        ) :
            _port_data_id(details::define_static_string(port_data_id)),
            _storage(storage),
            _input_plan(input_plan),
            _aliases(freeze_aliases(aliases)),
            _owns_storage(owns_storage),
            _is_static_constant(is_static_constant)
        {}

        struct State {
            std::span<SharedPortData> port_data;
            std::span<Sample> samples;
        };

        void declare(DeclarationContext<GraphPortDataNode> const& ctx) const
        {
            if (!_owns_storage) return;
            auto const& state = ctx.state();
            ctx.local_array(state.port_data, 1);
            ctx.local_array(
                state.samples,
                sample_storage_size(
                    _storage.channel_layout,
                    calculate_port_buffer_size(ctx.max_block_size(), _input_plan.storage)
                )
            );
            ctx.export_array(
                std::string(_port_data_id.view()), state.port_data);
            for (auto const& alias : _aliases) {
                ctx.export_array(std::string(alias.view()), state.port_data);
            }
        }

        void initialize(InitializationContext<GraphPortDataNode> const& ctx) const
        {
            if (!_owns_storage) return;
            auto& state = ctx.state();
            std::fill(
                state.samples.begin(), state.samples.end(),
                _storage.default_value);
            auto const layout = _storage.channel_layout;
            std::construct_at(
                &state.port_data[0],
                state.samples,
                0,
                layout,
                state.samples.size() / channel_count(layout)
            );
        }
    };
}
