#pragma once

#include "block_rate_buffer.h"
#include "devices/audio_device.h"

#include <algorithm>
#include <functional>
#include <span>
#include <stdexcept>
#include <vector>

namespace iv {
    class OrchestratorBuilder;

    class OutputDeviceMixer {
        class OutputChannelSinks {
            std::vector<std::span<Sample>> _sinks;

            static bool same_span(std::span<Sample> a, std::span<Sample> b)
            {
                return a.data() == b.data() && a.size() == b.size();
            }

        public:
            void register_sink(std::span<Sample> buffer)
            {
                auto it = std::find_if(_sinks.begin(), _sinks.end(), [&](std::span<Sample> existing) {
                    return same_span(existing, buffer);
                });
                if (it == _sinks.end()) {
                    _sinks.push_back(buffer);
                }
            }

            void update_sink(std::span<Sample> previous_buffer, std::span<Sample> buffer)
            {
                auto it = std::find_if(_sinks.begin(), _sinks.end(), [&](std::span<Sample> existing) {
                    return same_span(existing, previous_buffer);
                });
                if (it != _sinks.end()) {
                    *it = buffer;
                    return;
                }
                register_sink(buffer);
            }

            void unregister_sink(std::span<Sample> buffer)
            {
                auto it = std::find_if(_sinks.begin(), _sinks.end(), [&](std::span<Sample> existing) {
                    return same_span(existing, buffer);
                });
                if (it != _sinks.end()) {
                    _sinks.erase(it);
                }
            }

            std::span<std::span<Sample> const> sinks() const
            {
                return _sinks;
            }

            bool empty() const
            {
                return _sinks.empty();
            }
        };

        LogicalAudioDevice _device;
        RenderConfig _config;
        std::vector<OutputChannelSinks> _channels;
        BlockRateBuffer<Sample> _rate_buffer;
        std::span<Sample> _pending_request;
        std::function<void(OrchestratorBuilder&, OutputDeviceMixer&&)> _move_to_builder;

        void mix_block(BlockView<Sample> dst, size_t block_index, size_t block_size)
        {
            std::fill(dst.first.begin(), dst.first.end(), 0.0f);
            std::fill(dst.second.begin(), dst.second.end(), 0.0f);

            size_t const channels = _config.num_channels;
            for (size_t frame = 0; frame < block_size; ++frame) {
                size_t const frame_index = block_index + frame;
                size_t const base = frame * channels;
                for (size_t channel = 0; channel < channels; ++channel) {
                    if (channel >= _channels.size()) {
                        continue;
                    }
                    Sample mixed = 0.0f;
                    for (auto sink : _channels[channel].sinks()) {
                        if (sink.empty()) {
                            continue;
                        }
                        mixed += sink[frame_index & (sink.size() - 1)];
                    }
                    dst[base + channel] = mixed;
                }
            }
        }

        void submit_ready_response()
        {
            if (_pending_request.empty()) {
                throw std::logic_error("OutputDeviceMixer cannot submit a response without a pending request");
            }

            auto block = _rate_buffer.read_block(_pending_request.size());
            block.copy_to(make_block_view(_pending_request, 0, _pending_request.size()));
            _device.submit_response();
            _pending_request = {};
        }

        bool has_registered_sinks() const
        {
            return std::any_of(_channels.begin(), _channels.end(), [](OutputChannelSinks const& channel) {
                return !channel.empty();
            });
        }

    public:
        OutputDeviceMixer() = default;

        OutputDeviceMixer(
            LogicalAudioDevice device,
            std::function<void(OrchestratorBuilder&, OutputDeviceMixer&&)> move_to_builder
        )
        : _device(std::move(device))
        , _config(_device.config())
        , _channels(_config.num_channels)
        , _rate_buffer(_config.max_block_frames * _config.num_channels)
        , _move_to_builder(std::move(move_to_builder))
        {}

        OutputDeviceMixer(OutputDeviceMixer&&) noexcept = default;
        OutputDeviceMixer& operator=(OutputDeviceMixer&&) noexcept = default;
        OutputDeviceMixer(OutputDeviceMixer const&) = delete;
        OutputDeviceMixer& operator=(OutputDeviceMixer const&) = delete;

        RenderConfig const& config() const
        {
            return _config;
        }

        void register_sink(size_t channel, std::span<Sample> buffer)
        {
            if (channel >= _config.num_channels) {
                return;
            }
            _channels[channel].register_sink(buffer);
        }

        void update_sink(size_t channel, std::span<Sample> previous_buffer, std::span<Sample> buffer)
        {
            if (channel >= _config.num_channels) {
                return;
            }
            _channels[channel].update_sink(previous_buffer, buffer);
        }

        void unregister_sink(size_t channel, std::span<Sample> buffer)
        {
            if (channel >= _config.num_channels) {
                return;
            }
            _channels[channel].unregister_sink(buffer);
        }

        void wait_until_more_samples_needed()
        {
            if (!has_registered_sinks()) {
                return;
            }

            if (!_pending_request.empty()) {
                if (!_rate_buffer.can_read(_pending_request.size())) {
                    return;
                }
                submit_ready_response();
            }

            _pending_request = _device.wait_for_block_request();
            if (_pending_request.size() % _config.num_channels != 0) {
                throw std::logic_error("OutputDeviceMixer request size is not aligned to device channels");
            }
        }

        void publish_completed_block(size_t block_index, size_t block_size)
        {
            if (!has_registered_sinks()) {
                return;
            }

            if (_pending_request.empty()) {
                throw std::logic_error("OutputDeviceMixer requires a pending device request before publishing a block");
            }

            size_t const sample_count = block_size * _config.num_channels;
            if (!_rate_buffer.can_allocate_write(sample_count)) {
                throw std::logic_error("OutputDeviceMixer rate buffer does not have enough free capacity");
            }

            auto block = _rate_buffer.allocate_write(sample_count);
            mix_block(block, block_index, block_size);

            if (_rate_buffer.can_read(_pending_request.size())) {
                submit_ready_response();
            }
        }

        void move_to_builder(OrchestratorBuilder& builder) &&
        {
            auto move_to_builder = _move_to_builder;
            if (!move_to_builder) {
                throw std::logic_error("OutputDeviceMixer cannot move back to builder without a builder callback");
            }
            move_to_builder(builder, std::move(*this));
        }
    };

    class DeviceOrchestrator {
    protected:
        std::vector<OutputDeviceMixer> _mixers;
        bool _shutdown_requested = false;

    public:
        DeviceOrchestrator() = default;

        explicit DeviceOrchestrator(std::vector<OutputDeviceMixer> mixers)
        : _mixers(std::move(mixers))
        {}

        DeviceOrchestrator(DeviceOrchestrator&&) noexcept = default;
        DeviceOrchestrator& operator=(DeviceOrchestrator&&) noexcept = default;
        DeviceOrchestrator(DeviceOrchestrator const&) = delete;
        DeviceOrchestrator& operator=(DeviceOrchestrator const&) = delete;

        void wait_for_block()
        {
            if (_shutdown_requested) {
                return;
            }

            for (auto& mixer : _mixers) {
                mixer.wait_until_more_samples_needed();
            }
        }

        bool wait_for_block([[maybe_unused]] size_t block_index, [[maybe_unused]] size_t block_size)
        {
            wait_for_block();
            return true;
        }

        void sync_block(size_t block_index, size_t block_size)
        {
            for (auto& mixer : _mixers) {
                mixer.publish_completed_block(block_index, block_size);
            }

            if (!_shutdown_requested) {
                wait_for_block();
            }
        }

        void request_shutdown()
        {
            _shutdown_requested = true;
        }

        OrchestratorBuilder to_builder() &&;
    };

    using ExecutionTargetRegistry = DeviceOrchestrator;
    using ExecutionTargetSession = DeviceOrchestrator;
}
