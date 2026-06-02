#pragma once

#include "ports.h"

#include <concepts>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace iv {
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

    template<typename Device>
    concept LogicalAudioDeviceBackend = requires(Device& device, Device const& const_device) {
        { const_device.config() } -> std::same_as<RenderConfig const&>;
        { device.wait_for_block_request() } -> std::same_as<std::span<Sample>>;
        { device.submit_response() } -> std::same_as<void>;
    };

    class LogicalAudioDevice {
        void* _state;
        void (*_destroy)(void*) noexcept;
        RenderConfig const& (*_config)(void const*) noexcept;
        std::span<Sample> (*_wait_for_block_request)(void*);
        void (*_submit_response)(void*);

        template<LogicalAudioDeviceBackend Device>
        void bind_device() noexcept
        {
            _destroy = +[](void* self) noexcept {
                delete static_cast<Device*>(self);
            };
            _config = +[](void const* self) noexcept -> RenderConfig const& {
                return static_cast<Device const*>(self)->config();
            };
            _wait_for_block_request = +[](void* self) -> std::span<Sample> {
                return static_cast<Device*>(self)->wait_for_block_request();
            };
            _submit_response = +[](void* self) {
                static_cast<Device*>(self)->submit_response();
            };
        }

    public:
        LogicalAudioDevice() = default;

        template<LogicalAudioDeviceBackend Device>
            requires (!std::same_as<std::remove_cvref_t<Device>, LogicalAudioDevice> &&
                      std::move_constructible<std::remove_cvref_t<Device>>)
        explicit LogicalAudioDevice(Device&& device)
        : _state(new std::remove_cvref_t<Device>(std::forward<Device>(device)))
        {
            bind_device<std::remove_cvref_t<Device>>();
        }

        template<LogicalAudioDeviceBackend Device, typename... Args>
        explicit LogicalAudioDevice(std::in_place_type_t<Device>, Args&&... args)
        : _state(new Device(std::forward<Args>(args)...))
        {
            bind_device<Device>();
        }

        ~LogicalAudioDevice()
        {
            if (_state && _destroy) {
                _destroy(_state);
            }
        }

        LogicalAudioDevice(LogicalAudioDevice&& other) noexcept
        : _state(other._state)
        , _destroy(other._destroy)
        , _config(other._config)
        , _wait_for_block_request(other._wait_for_block_request)
        , _submit_response(other._submit_response)
        {
            other._state = nullptr;
            other._destroy = nullptr;
            other._config = nullptr;
            other._wait_for_block_request = nullptr;
            other._submit_response = nullptr;
        }

        LogicalAudioDevice& operator=(LogicalAudioDevice&& other) noexcept
        {
            if (this == &other) {
                return *this;
            }

            if (_state && _destroy) {
                _destroy(_state);
            }
            _state = other._state;
            _destroy = other._destroy;
            _config = other._config;
            _wait_for_block_request = other._wait_for_block_request;
            _submit_response = other._submit_response;
            other._state = nullptr;
            other._destroy = nullptr;
            other._config = nullptr;
            other._wait_for_block_request = nullptr;
            other._submit_response = nullptr;
            return *this;
        }

        LogicalAudioDevice(LogicalAudioDevice const&) = delete;
        LogicalAudioDevice& operator=(LogicalAudioDevice const&) = delete;

        explicit operator bool() const noexcept
        {
            return _state != nullptr;
        }

        RenderConfig const& config() const
        {
            if (!operator bool()) throw std::logic_error("LogicalAudioDevice is empty");
            return _config(_state);
        }

        std::span<Sample> wait_for_block_request()
        {
            if (!operator bool()) throw std::logic_error("LogicalAudioDevice is empty");
            return _wait_for_block_request(_state);
        }

        void submit_response()
        {
            if (!operator bool()) return;
            _submit_response(_state);
        }
    };
}
