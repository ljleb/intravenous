#pragma once
#include "devices/audio_device.h"
#include <iostream>
#include <thread>

namespace iv {
    struct SystemWrappedNode {
        AudioRenderSession render_session;
        TypeErasedNode root;

        struct SinkBinding {
            size_t channel = 0;
            std::span<Sample> buffer;
        };

        struct State {
            std::span<std::byte> root_buffer;
            std::span<SinkBinding> sink_bindings;
            bool has_active_sinks = false;
        };

        std::string make_init_buffer_id(std::string_view suffix) const
        {
            return "__system_wrap_init:" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ":" + std::string(suffix);
        }

        template<typename Allocator>
        void init_buffer(Allocator& allocator, InitBufferContext& context) const
        {
            size_t const num_channels = render_session.config().num_channels;
            State& system_state = allocator.template new_object<State>();

            allocator.assign(system_state.root_buffer, do_init_buffer(root, allocator, context));

            auto used_channels = context.register_init_buffer<size_t>(make_init_buffer_id("used_channels"), num_channels);
            auto used_count = context.register_init_buffer<size_t>(make_init_buffer_id("used_count"), 1);

            size_t count = 0;
            for (size_t channel = 0; channel < num_channels; ++channel) {
                if (!context.has_tick_buffer(render_session.sink_id(channel))) {
                    continue;
                }
                used_channels[count++] = channel;
            }
            used_count[0] = count;

            allocator.assign(system_state.sink_bindings, allocator.template new_array<SinkBinding>(count));

            size_t const sink_buffer_size = render_session.sink_buffer_size();
            for (size_t i = 0; i < count; ++i) {
                auto const channel = used_channels[i];
                auto buffer = allocator.template new_array<Sample>(sink_buffer_size);
                context.register_tick_buffer(render_session.sink_id(channel), buffer);
                allocator.assign(allocator.at(system_state.sink_bindings, i).channel, channel);
                allocator.assign(allocator.at(system_state.sink_bindings, i).buffer, buffer);
            }

            allocator.assign(system_state.has_active_sinks, count != 0);
        }

        size_t internal_latency() const
        {
            return root.internal_latency();
        }

        void tick(TickState const& state)
        {
            auto& system_state = state.get_state<State>();
            render_session.prepare_tick(state.index);
            if (render_session.is_shutdown_requested()) {
                return;
            }

            if (system_state.has_active_sinks && state.index == render_session.active_block_start()) {
                size_t const block_start = render_session.active_block_start();
                for (auto const& binding : system_state.sink_bindings) {
                    auto buffer = binding.buffer;
                    if (buffer.empty()) {
                        continue;
                    }
                    for (size_t frame = 0; frame < render_session.active_block_frames(); ++frame) {
                        size_t const global_index = block_start + frame;
                        buffer[global_index & (buffer.size() - 1)] = 0.0f;
                    }
                }
            }

            root.tick({
                NodeState{
                    .inputs = state.inputs,
                    .outputs = state.outputs,
                    .buffer = system_state.root_buffer,
                },
                state.midi,
                state.index,
            });

            if (system_state.has_active_sinks && state.index + 1 == render_session.active_block_end()) {
                for (auto const& binding : system_state.sink_bindings) {
                    render_session.mix_sink_buffer(binding.channel, binding.buffer);
                }
            }

            render_session.finish_tick(state.index);
        }
    };

    class System {
        AudioDevice _audio_device;
        bool _close_on_enter = true;
        std::jthread _close_thread;

    public:
        explicit System(RenderConfig config = {}, bool open_backend = true, bool close_on_enter = true) :
            _audio_device(config, open_backend),
            _close_on_enter(close_on_enter)
        {
            if (_close_on_enter) {
                _close_thread = std::jthread([this](std::stop_token) {
                    std::string line;
                    std::getline(std::cin, line);
                    _audio_device.request_shutdown();
                });
            }
        }

        ~System()
        {
            request_shutdown();
        }

        System(System const&) = delete;
        System& operator=(System const&) = delete;

        AudioDevice& audio_device()
        {
            return _audio_device;
        }

        AudioDevice const& audio_device() const
        {
            return _audio_device;
        }

        RenderConfig const& render_config() const
        {
            return _audio_device.config();
        }

        Sample& sample_period()
        {
            return _audio_device.sample_period();
        }

        bool is_shutdown_requested() const
        {
            return _audio_device.is_shutdown_requested();
        }

        TypeErasedNode wrap_root(TypeErasedNode root, bool uses_audio_device = false)
        {
            if (!uses_audio_device) {
                return root;
            }

            return TypeErasedNode(SystemWrappedNode {
                .render_session = AudioRenderSession(_audio_device),
                .root = std::move(root),
            });
        }

        void request_shutdown()
        {
            _audio_device.request_shutdown();
        }

        void activate_root(bool uses_audio_device)
        {
            (void)uses_audio_device;
        }
    };
}
