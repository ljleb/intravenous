#pragma once

#include <intravenous/ports.h>

#include <concepts>
#include <cstddef>
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

    struct AudioInputBlock {
        std::span<Sample const> samples {};
        double capture_timestamp_seconds = 0.0;
        bool discontinuous = false;
    };

    template<typename Device>
    concept AudioOutputDeviceBackend = requires(Device& device, Device const& const_device) {
        { const_device.config() } -> std::same_as<RenderConfig const&>;
        { device.wait_for_block_request() } -> std::same_as<std::span<Sample>>;
        { device.submit_response() } -> std::same_as<void>;
    };

    class AudioOutputDevice {
        void* _state;
        void (*_destroy)(void*) noexcept;
        RenderConfig const& (*_config)(void const*) noexcept;
        std::span<Sample> (*_wait_for_block_request)(void*);
        void (*_submit_response)(void*);

        template<AudioOutputDeviceBackend Device>
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
        AudioOutputDevice() = default;

        template<AudioOutputDeviceBackend Device>
            requires (!std::same_as<std::remove_cvref_t<Device>, AudioOutputDevice> &&
                      std::move_constructible<std::remove_cvref_t<Device>>)
        explicit AudioOutputDevice(Device&& device)
        : _state(new std::remove_cvref_t<Device>(std::forward<Device>(device)))
        {
            bind_device<std::remove_cvref_t<Device>>();
        }

        template<AudioOutputDeviceBackend Device, typename... Args>
        explicit AudioOutputDevice(std::in_place_type_t<Device>, Args&&... args)
        : _state(new Device(std::forward<Args>(args)...))
        {
            bind_device<Device>();
        }

        ~AudioOutputDevice()
        {
            if (_state && _destroy) {
                _destroy(_state);
            }
        }

        AudioOutputDevice(AudioOutputDevice&& other) noexcept
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

        AudioOutputDevice& operator=(AudioOutputDevice&& other) noexcept
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

        AudioOutputDevice(AudioOutputDevice const&) = delete;
        AudioOutputDevice& operator=(AudioOutputDevice const&) = delete;

        explicit operator bool() const noexcept
        {
            return _state != nullptr;
        }

        RenderConfig const& config() const
        {
            if (!operator bool()) throw std::logic_error("AudioOutputDevice is empty");
            return _config(_state);
        }

        std::span<Sample> wait_for_block_request()
        {
            if (!operator bool()) throw std::logic_error("AudioOutputDevice is empty");
            return _wait_for_block_request(_state);
        }

        void submit_response()
        {
            if (!operator bool()) return;
            _submit_response(_state);
        }
    };

    template<typename Device>
    concept AudioInputDeviceBackend = requires(Device& device, Device const& const_device) {
        { const_device.config() } -> std::same_as<RenderConfig const&>;
        { device.wait_for_captured_block() } -> std::same_as<AudioInputBlock>;
        { device.release_captured_block() } -> std::same_as<void>;
    };

    class AudioInputDevice {
        void* _state = nullptr;
        void (*_destroy)(void*) noexcept = nullptr;
        RenderConfig const& (*_config)(void const*) noexcept = nullptr;
        AudioInputBlock (*_wait_for_captured_block)(void*) = nullptr;
        void (*_release_captured_block)(void*) = nullptr;

        template<AudioInputDeviceBackend Device>
        void bind_device() noexcept
        {
            _destroy = +[](void* self) noexcept {
                delete static_cast<Device*>(self);
            };
            _config = +[](void const* self) noexcept -> RenderConfig const& {
                return static_cast<Device const*>(self)->config();
            };
            _wait_for_captured_block = +[](void* self) -> AudioInputBlock {
                return static_cast<Device*>(self)->wait_for_captured_block();
            };
            _release_captured_block = +[](void* self) {
                static_cast<Device*>(self)->release_captured_block();
            };
        }

    public:
        AudioInputDevice() = default;

        template<AudioInputDeviceBackend Device>
            requires (!std::same_as<std::remove_cvref_t<Device>, AudioInputDevice> &&
                      std::move_constructible<std::remove_cvref_t<Device>>)
        explicit AudioInputDevice(Device&& device)
        : _state(new std::remove_cvref_t<Device>(std::forward<Device>(device)))
        {
            bind_device<std::remove_cvref_t<Device>>();
        }

        template<AudioInputDeviceBackend Device, typename... Args>
        explicit AudioInputDevice(std::in_place_type_t<Device>, Args&&... args)
        : _state(new Device(std::forward<Args>(args)...))
        {
            bind_device<Device>();
        }

        ~AudioInputDevice()
        {
            if (_state && _destroy) {
                _destroy(_state);
            }
        }

        AudioInputDevice(AudioInputDevice&& other) noexcept
        : _state(other._state)
        , _destroy(other._destroy)
        , _config(other._config)
        , _wait_for_captured_block(other._wait_for_captured_block)
        , _release_captured_block(other._release_captured_block)
        {
            other._state = nullptr;
            other._destroy = nullptr;
            other._config = nullptr;
            other._wait_for_captured_block = nullptr;
            other._release_captured_block = nullptr;
        }

        AudioInputDevice& operator=(AudioInputDevice&& other) noexcept
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
            _wait_for_captured_block = other._wait_for_captured_block;
            _release_captured_block = other._release_captured_block;
            other._state = nullptr;
            other._destroy = nullptr;
            other._config = nullptr;
            other._wait_for_captured_block = nullptr;
            other._release_captured_block = nullptr;
            return *this;
        }

        AudioInputDevice(AudioInputDevice const&) = delete;
        AudioInputDevice& operator=(AudioInputDevice const&) = delete;

        explicit operator bool() const noexcept
        {
            return _state != nullptr;
        }

        RenderConfig const& config() const
        {
            if (!operator bool()) throw std::logic_error("AudioInputDevice is empty");
            return _config(_state);
        }

        AudioInputBlock wait_for_captured_block()
        {
            if (!operator bool()) throw std::logic_error("AudioInputDevice is empty");
            return _wait_for_captured_block(_state);
        }

        void release_captured_block()
        {
            if (!operator bool()) return;
            _release_captured_block(_state);
        }
    };
}
