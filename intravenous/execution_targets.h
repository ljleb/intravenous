#pragma once

#include "devices/audio_device.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace iv {
    class TypeErasedExecutionTarget {
        std::shared_ptr<void> _target;
        size_t (*_preferred_block_size_fn)(void const*);
        void (*_begin_block_fn)(void*, size_t, size_t);
        void (*_end_block_fn)(void*, size_t, size_t);

    public:
        template<typename Target>
        explicit TypeErasedExecutionTarget(std::shared_ptr<Target> target)
        : _target(std::move(target))
        , _preferred_block_size_fn([](void const* target_ptr) {
            return static_cast<Target const*>(target_ptr)->preferred_block_size();
        })
        , _begin_block_fn([](void* target_ptr, size_t block_index, size_t block_size) {
            static_cast<Target*>(target_ptr)->begin_block(block_index, block_size);
        })
        , _end_block_fn([](void* target_ptr, size_t block_index, size_t block_size) {
            static_cast<Target*>(target_ptr)->end_block(block_index, block_size);
        })
        {}

        size_t preferred_block_size() const
        {
            return _preferred_block_size_fn(_target.get());
        }

        void begin_block(size_t block_index, size_t block_size) const
        {
            _begin_block_fn(_target.get(), block_index, block_size);
        }

        void end_block(size_t block_index, size_t block_size) const
        {
            _end_block_fn(_target.get(), block_index, block_size);
        }
    };

    class AudioDeviceExecutionTarget {
    private:
        // TODO: This target should eventually open/manage a concrete input/output device selected
        // from a device registry rather than being constructed from a bootstrap AudioDevice object.
        AudioDevicePlayback _playback;
        size_t _device_id;
        size_t _channel;
        std::vector<std::span<Sample>> _sinks;

    public:
        AudioDeviceExecutionTarget(
            size_t device_id,
            size_t channel,
            AudioDevice& device
        )
        : _playback(device.make_playback())
        , _device_id(std::move(device_id))
        , _channel(channel)
        {}

        size_t device_id() const
        {
            return _device_id;
        }

        size_t channel() const
        {
            return _channel;
        }

        size_t preferred_block_size() const
        {
            return _playback.preferred_block_size();
        }

        std::span<Sample const> output_block() const
        {
            return _playback.output_block(_channel);
        }

        bool is_shutdown_requested() const
        {
            return _playback.is_shutdown_requested();
        }

        void request_shutdown()
        {
            _playback.request_shutdown();
        }

        void register_sink(std::span<Sample> buffer)
        {
            for (auto const& sink : _sinks) {
                if (same_span(sink, buffer)) {
                    return;
                }
            }
            _sinks.push_back(buffer);
        }

        void update_sink(std::span<Sample> previous_buffer, std::span<Sample> buffer)
        {
            for (auto& sink : _sinks) {
                if (same_span(sink, previous_buffer)) {
                    sink = buffer;
                    return;
                }
            }

            register_sink(buffer);
        }

        void unregister_sink(std::span<Sample> buffer)
        {
            _sinks.erase(
                std::remove_if(
                    _sinks.begin(),
                    _sinks.end(),
                    [&](std::span<Sample> const& sink) {
                        return same_span(sink, buffer);
                    }
                ),
                _sinks.end()
            );
        }

        template<typename A>
        static bool same_span(std::span<A> const& a, std::span<A> const& b)
        {
            return a.data() == b.data() && a.size() == b.size();
        }

        void begin_block(size_t block_index, size_t)
        {
            _playback.begin_block(block_index);

            if (block_index != _playback.active_block_start()) {
                return;
            }

            clear_sinks(_playback.active_block_start(), _playback.active_block_frames());
        }

        void end_block(size_t block_index, size_t block_size)
        {
            size_t const block_end = block_index + block_size;
            if (block_end == _playback.active_block_end()) {
                mix_sinks(_playback.active_block_start(), _playback.active_block_frames());
            }
            _playback.finish_tick(block_end - 1);
        }

    private:
        void clear_sinks(size_t block_start, size_t frames)
        {
            for (auto const& sink : _sinks) {
                if (sink.empty()) {
                    continue;
                }
                for (size_t frame = 0; frame < frames; ++frame) {
                    sink[(block_start + frame) & (sink.size() - 1)] = 0.0f;
                }
            }
        }

        void mix_sinks(size_t block_start, size_t frames)
        {
            for (auto const& sink : _sinks) {
                if (sink.empty()) {
                    continue;
                }
                _playback.mix_sink_block(_channel, sink, block_start, frames);
            }
        }
    };

    class ExecutionTargets {
    public:
        struct AudioDeviceProvider {
            void* owner;
            AudioDevice* (*device_fn)(void* owner, size_t device_id);
            size_t (*preferred_block_size_fn)(void* owner) = nullptr;
        };

    private:
        AudioDeviceProvider _audio_device_provider;
        std::vector<std::shared_ptr<AudioDeviceExecutionTarget>> _audio_device_targets;

    public:
        explicit ExecutionTargets(AudioDeviceProvider audio_device_provider)
        : _audio_device_provider(audio_device_provider)
        {
            if (!_audio_device_provider.device_fn) {
                throw std::logic_error("ExecutionTargets requires an audio device provider");
            }
        }

        size_t preferred_block_size_hint() const
        {
            if (_audio_device_provider.preferred_block_size_fn) {
                return _audio_device_provider.preferred_block_size_fn(_audio_device_provider.owner);
            }
            return MAX_BLOCK_SIZE;
        }

        AudioDeviceExecutionTarget& audio_device(size_t device_id, size_t channel)
        {
            for (auto const& target : _audio_device_targets) {
                if (target->device_id() == device_id && target->channel() == channel) {
                    return *target;
                }
            }

            AudioDevice* device = _audio_device_provider.device_fn(_audio_device_provider.owner, device_id);
            if (!device) {
                throw std::logic_error("ExecutionTargets audio device provider returned null");
            }

            auto target = std::make_shared<AudioDeviceExecutionTarget>(
                std::move(device_id),
                channel,
                *device
            );
            _audio_device_targets.push_back(target);
            return *target;
        }

        std::vector<TypeErasedExecutionTarget> all() const
        {
            std::vector<TypeErasedExecutionTarget> targets;
            targets.reserve(_audio_device_targets.size());
            for (auto const& target : _audio_device_targets) {
                targets.emplace_back(target);
            }
            return targets;
        }

        bool is_shutdown_requested() const
        {
            return std::any_of(_audio_device_targets.begin(), _audio_device_targets.end(), [](auto const& target) {
                return target->is_shutdown_requested();
            });
        }

        void request_shutdown()
        {
            for (auto const& target : _audio_device_targets) {
                target->request_shutdown();
            }
        }
    };
}
