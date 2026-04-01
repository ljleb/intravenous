#pragma once

#include "ports.h"
#include "compat.h"
#include <condition_variable>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>

namespace iv {
    using RequestNotification = std::shared_ptr<std::condition_variable>;

    struct RenderConfig {
        size_t sample_rate = 48000;
        size_t num_channels = 2;
        size_t max_block_frames = 4096;
        size_t preferred_block_size = 256;
    };

    inline void validate_render_config(RenderConfig const& config)
    {
        if (config.sample_rate == 0 || config.num_channels == 0 || config.max_block_frames == 0 || config.preferred_block_size == 0) {
            throw std::invalid_argument("render config values must be non-zero");
        }
    }

    inline Sample sample_period(RenderConfig const& config)
    {
        return static_cast<Sample>(1) / static_cast<Sample>(config.sample_rate);
    }

    class LogicalAudioDevice {
        using StatePtr = std::shared_ptr<void>;

        RenderConfig _config;
        StatePtr _state;
        bool (*_is_shutdown_requested_fn)(void const*) = nullptr;
        void (*_request_shutdown_fn)(void*) = nullptr;
        std::optional<std::pair<size_t, size_t>> (*_requested_block_fn)(void const*) = nullptr;
        std::optional<std::pair<size_t, size_t>> (*_wait_for_requested_block_fn)(void*) = nullptr;
        void (*_begin_requested_block_fn)(void*, size_t, size_t) = nullptr;
        void (*_submit_mixed_block_fn)(void*, size_t, std::span<Sample const>, size_t, size_t) = nullptr;
        bool (*_wait_until_block_ready_fn)(void*) = nullptr;
        std::span<Sample const> (*_current_mixed_block_fn)(void const*) = nullptr;
        void (*_finish_requested_block_fn)(void*) = nullptr;

        LogicalAudioDevice(
            RenderConfig config,
            StatePtr state,
            bool (*is_shutdown_requested_fn)(void const*),
            void (*request_shutdown_fn)(void*),
            std::optional<std::pair<size_t, size_t>> (*requested_block_fn)(void const*),
            std::optional<std::pair<size_t, size_t>> (*wait_for_requested_block_fn)(void*),
            void (*begin_requested_block_fn)(void*, size_t, size_t),
            void (*submit_mixed_block_fn)(void*, size_t, std::span<Sample const>, size_t, size_t),
            bool (*wait_until_block_ready_fn)(void*),
            std::span<Sample const> (*current_mixed_block_fn)(void const*),
            void (*finish_requested_block_fn)(void*)
        )
        : _config(std::move(config))
        , _state(std::move(state))
        , _is_shutdown_requested_fn(is_shutdown_requested_fn)
        , _request_shutdown_fn(request_shutdown_fn)
        , _requested_block_fn(requested_block_fn)
        , _wait_for_requested_block_fn(wait_for_requested_block_fn)
        , _begin_requested_block_fn(begin_requested_block_fn)
        , _submit_mixed_block_fn(submit_mixed_block_fn)
        , _wait_until_block_ready_fn(wait_until_block_ready_fn)
        , _current_mixed_block_fn(current_mixed_block_fn)
        , _finish_requested_block_fn(finish_requested_block_fn)
        {}

    public:
        LogicalAudioDevice() = default;

        template<typename State>
        static LogicalAudioDevice from_state(RenderConfig config, std::shared_ptr<State> state)
        {
            return LogicalAudioDevice(
                std::move(config),
                std::move(state),
                +[](void const* ptr) -> bool {
                    return static_cast<State const*>(ptr)->is_shutdown_requested();
                },
                +[](void* ptr) {
                    static_cast<State*>(ptr)->request_shutdown();
                },
                +[](void const* ptr) -> std::optional<std::pair<size_t, size_t>> {
                    return static_cast<State const*>(ptr)->requested_block();
                },
                +[](void* ptr) -> std::optional<std::pair<size_t, size_t>> {
                    return static_cast<State*>(ptr)->wait_for_requested_block();
                },
                +[](void* ptr, size_t frame_index, size_t frame_count) {
                    static_cast<State*>(ptr)->begin_requested_block(frame_index, frame_count);
                },
                +[](void* ptr, size_t frame_index, std::span<Sample const> interleaved, size_t frames, size_t channels) {
                    static_cast<State*>(ptr)->submit_mixed_block(frame_index, interleaved, frames, channels);
                },
                +[](void* ptr) -> bool {
                    return static_cast<State*>(ptr)->wait_until_block_ready();
                },
                +[](void const* ptr) -> std::span<Sample const> {
                    return static_cast<State const*>(ptr)->current_mixed_block();
                },
                +[](void* ptr) {
                    static_cast<State*>(ptr)->finish_requested_block();
                }
            );
        }

        RenderConfig const& config() const
        {
            return _config;
        }

        bool is_shutdown_requested() const
        {
            return _is_shutdown_requested_fn(static_cast<void const*>(_state.get()));
        }

        void request_shutdown()
        {
            _request_shutdown_fn(_state.get());
        }

        std::optional<std::pair<size_t, size_t>> requested_block() const
        {
            return _requested_block_fn(static_cast<void const*>(_state.get()));
        }

        std::optional<std::pair<size_t, size_t>> wait_for_requested_block()
        {
            return _wait_for_requested_block_fn(_state.get());
        }

        void begin_requested_block(size_t frame_index, size_t frame_count)
        {
            _begin_requested_block_fn(_state.get(), frame_index, frame_count);
        }

        void submit_mixed_block(
            size_t frame_index,
            std::span<Sample const> interleaved,
            size_t frames,
            size_t channels
        )
        {
            _submit_mixed_block_fn(_state.get(), frame_index, interleaved, frames, channels);
        }

        bool wait_until_block_ready()
        {
            return _wait_until_block_ready_fn(_state.get());
        }

        std::span<Sample const> current_mixed_block() const
        {
            return _current_mixed_block_fn(static_cast<void const*>(_state.get()));
        }

        void finish_requested_block()
        {
            _finish_requested_block_fn(_state.get());
        }
    };
}
