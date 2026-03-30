#pragma once

#include "devices/audio_device.h"
#include "compat.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <sstream>
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
            AudioDevicePlayback playback
        )
        : _playback(std::move(playback))
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

        size_t active_block_start() const
        {
            return _playback.active_block_start();
        }

        size_t active_block_end() const
        {
            return _playback.active_block_end();
        }

        size_t active_block_frames() const
        {
            return _playback.active_block_frames();
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
            for (size_t sink_i = 0; sink_i < _sinks.size(); ++sink_i) {
                auto const& sink = _sinks[sink_i];
                if (sink.empty()) {
                    continue;
                }
                trace_sink_input(sink_i, sink, block_start, frames);
                _playback.mix_sink_block(_channel, sink, block_start, frames);
            }
            trace_mixed_output(block_start, frames);
        }

        void trace_sink_input(size_t sink_index, std::span<Sample> sink, size_t block_start, size_t frames)
        {
            constexpr std::string_view prefix = "trace.sink.input";
            if (!sample_trace_matches(prefix)) {
                return;
            }

            Sample max_abs = 0.0f;
            std::ostringstream oss;
            oss << prefix << ": device=" << _device_id
                << " channel=" << _channel
                << " sink=" << sink_index
                << " start=" << block_start
                << " frames=" << frames
                << " samples=[";

            size_t emitted = 0;
            for (size_t frame = 0; frame < frames; ++frame) {
                Sample sample = sink[(block_start + frame) & (sink.size() - 1)];
                max_abs = std::max(max_abs, Sample(std::abs(sample)));
                if (emitted < 4) {
                    if (emitted != 0) {
                        oss << ", ";
                    }
                    oss << sample;
                    ++emitted;
                }
            }
            if (frames > 4) {
                oss << ", ...";
            }
            oss << "] max=" << max_abs;
            debug_log(oss.str());
        }

        void trace_mixed_output(size_t block_start, size_t frames)
        {
            constexpr std::string_view prefix = "trace.audio.mix";
            if (!sample_trace_matches(prefix)) {
                return;
            }

            auto block = _playback.output_block(_channel);
            Sample max_abs = 0.0f;
            size_t emitted = 0;

            std::ostringstream oss;
            oss << prefix << ": device=" << _device_id
                << " channel=" << _channel
                << " start=" << block_start
                << " frames=" << frames
                << " sinks=" << _sinks.size()
                << " samples=[";

            for (size_t frame = 0; frame < std::min(frames, block.size()); ++frame) {
                Sample sample = block[frame];
                max_abs = std::max(max_abs, Sample(std::abs(sample)));
                if (emitted < 4) {
                    if (emitted != 0) {
                        oss << ", ";
                    }
                    oss << sample;
                    ++emitted;
                }
            }
            if (frames > 4) {
                oss << ", ...";
            }
            oss << "] max=" << max_abs;
            debug_log(oss.str());
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
        struct DevicePlaybackEntry {
            size_t device_id;
            AudioDevicePlayback playback;
        };
        std::vector<DevicePlaybackEntry> _device_playbacks;
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

            AudioDevicePlayback playback;
            for (auto const& entry : _device_playbacks) {
                if (entry.device_id == device_id) {
                    playback = entry.playback;
                    break;
                }
            }
            if (!playback) {
                playback = device->make_playback();
                _device_playbacks.push_back({
                    .device_id = device_id,
                    .playback = playback,
                });
            }

            auto target = std::make_shared<AudioDeviceExecutionTarget>(
                std::move(device_id),
                channel,
                std::move(playback)
            );
            _audio_device_targets.push_back(target);
            return *target;
        }

        size_t active_block_frames_or(size_t fallback) const
        {
            if (_audio_device_targets.empty()) {
                return fallback;
            }

            size_t const start = _audio_device_targets.front()->active_block_start();
            size_t const end = _audio_device_targets.front()->active_block_end();
            for (auto const& target : _audio_device_targets) {
                if (
                    target->active_block_start() != start ||
                    target->active_block_end() != end
                ) {
                    throw std::logic_error("ExecutionTargets audio targets disagree on active block window");
                }
            }
            return end - start;
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
