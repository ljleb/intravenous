#pragma once
#include "devices/audio_device.h"
#include <iostream>
#include <thread>

namespace iv {
    struct SystemWrappedNode {
        AudioDevice* audio_device = nullptr;
        TypeErasedNode root;

        template<typename Allocator>
        void init_buffer(Allocator& allocator, GraphInitContext& context) const
        {
            do_init_buffer(root, allocator, context);
        }

        size_t internal_latency() const
        {
            return root.internal_latency();
        }

        void tick(TickState const& state)
        {
            audio_device->prepare_tick(state.index);
            if (audio_device->is_shutdown_requested()) {
                return;
            }

            root.tick(state);
            audio_device->finish_tick(state.index);
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
                .audio_device = &_audio_device,
                .root = std::move(root),
            });
        }

        void request_shutdown()
        {
            _audio_device.request_shutdown();
        }

        void activate_root(bool uses_audio_device)
        {
            _audio_device.set_sink_active(uses_audio_device);
        }
    };
}
