#pragma once

#include <intravenous/node/lifecycle.h>

#include <array>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace iv {

class SampleProjectionNode {
public:
    struct InputChannel {
        size_t port = 0;
        size_t channel = 0;

        bool operator==(InputChannel const&) const = default;
    };

    struct OutputChannel {
        size_t port = 0;
        size_t channel = 0;

        bool operator==(OutputChannel const&) const = default;
    };

    struct Route {
        ChannelTypeId source_type = ChannelTypeId::mono;
        ChannelTypeId target_type = ChannelTypeId::mono;
        std::vector<InputChannel> sources {};
        std::vector<OutputChannel> targets {};

        // Filled by the constructor when the route reads one complete concrete
        // input port in channel order. In that case no sparse gather is needed.
        std::optional<size_t> ordered_source_port {};
    };

    SampleProjectionNode(
        std::vector<InputConfig> inputs,
        std::vector<OutputConfig> outputs,
        std::vector<Route> routes
    ) :
        _inputs(std::move(inputs)),
        _outputs(std::move(outputs)),
        _routes(std::move(routes))
    {
        prepare_routes();
    }

    std::span<InputConfig const> inputs() const
    {
        return _inputs;
    }

    std::span<OutputConfig const> outputs() const
    {
        return _outputs;
    }

    void tick_block(TickBlockContext<SampleProjectionNode> const& ctx) const
    {
        for (size_t frame = 0; frame < ctx.block_size; ++frame) {
            for (auto const& route : _routes) {
                std::array<Sample, 2> gathered {};
                if (!route.ordered_source_port) {
                    for (size_t channel = 0; channel < route.sources.size(); ++channel) {
                        auto const source = route.sources[channel];
                        gathered[channel] =
                            ctx.inputs[source.port].get_frame(frame, source.channel);
                    }
                }

                auto source_sample = [&](size_t channel) -> Sample {
                    if (route.ordered_source_port) {
                        return ctx.inputs[*route.ordered_source_port]
                            .get_frame(frame, channel);
                    }
                    return gathered[channel];
                };

                auto write_target = [&](size_t channel, Sample value) {
                    auto const target = route.targets[channel];
                    ctx.outputs[target.port].write_frame(
                        frame, target.channel, value);
                };

                if (route.source_type == ChannelTypeId::mono &&
                    route.target_type == ChannelTypeId::mono) {
                    write_target(0, source_sample(0));
                } else if (route.source_type == ChannelTypeId::mono &&
                           route.target_type == ChannelTypeId::stereo) {
                    auto const value = source_sample(0);
                    write_target(0, value);
                    write_target(1, value);
                } else if (route.source_type == ChannelTypeId::stereo &&
                           route.target_type == ChannelTypeId::mono) {
                    write_target(
                        0, (source_sample(0) + source_sample(1)) * 0.5f);
                } else if (route.source_type == ChannelTypeId::stereo &&
                           route.target_type == ChannelTypeId::stereo) {
                    write_target(0, source_sample(0));
                    write_target(1, source_sample(1));
                } else {
                    throw std::logic_error(
                        "unsupported sample projection channel conversion");
                }
            }
        }
    }

private:
    std::vector<InputConfig> _inputs {};
    std::vector<OutputConfig> _outputs {};
    std::vector<Route> _routes {};

    void prepare_routes()
    {
        std::vector<std::vector<bool>> written;
        written.reserve(_outputs.size());
        for (auto const& output : _outputs) {
            written.emplace_back(
                channel_count(output.channel_layout.channel_type), false);
        }

        for (auto& route : _routes) {
            auto const source_channels = channel_count(route.source_type);
            auto const target_channels = channel_count(route.target_type);
            if (route.sources.size() != source_channels ||
                route.targets.size() != target_channels) {
                throw std::logic_error(
                    "sample projection route does not match its channel types");
            }

            bool ordered = !route.sources.empty();
            size_t ordered_port = ordered ? route.sources.front().port : 0;
            if (ordered) {
                if (ordered_port >= _inputs.size() ||
                    _inputs[ordered_port].channel_layout.channel_type !=
                        route.source_type) {
                    ordered = false;
                }
            }

            for (size_t channel = 0; channel < route.sources.size(); ++channel) {
                auto const source = route.sources[channel];
                if (source.port >= _inputs.size() ||
                    source.channel >= channel_count(
                        _inputs[source.port].channel_layout.channel_type)) {
                    throw std::logic_error(
                        "sample projection source channel is out of bounds");
                }
                ordered =
                    ordered && source.port == ordered_port &&
                    source.channel == channel;
            }
            route.ordered_source_port =
                ordered ? std::optional<size_t>(ordered_port) : std::nullopt;

            for (auto const target : route.targets) {
                if (target.port >= _outputs.size() ||
                    target.channel >= written[target.port].size()) {
                    throw std::logic_error(
                        "sample projection target channel is out of bounds");
                }
                if (written[target.port][target.channel]) {
                    throw std::logic_error(
                        "sample projection target channel has multiple routes");
                }
                written[target.port][target.channel] = true;
            }
        }

        for (size_t port = 0; port < written.size(); ++port) {
            bool any_written = false;
            for (size_t channel = 0; channel < written[port].size(); ++channel) {
                if (written[port][channel]) {
                    any_written = true;
                }
            }
            if (!any_written) {
                throw std::logic_error(
                    "sample projection output has no written channels");
            }
        }
    }
};

} // namespace iv
