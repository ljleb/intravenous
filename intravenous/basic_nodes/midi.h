#pragma once

#include "../fast_bitset.h"
#include "../node_lifecycle.h"
#include "../note_number_lookup_table.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace iv {
    namespace details {
        inline bool midi_is_note_on(MidiEvent const& midi)
        {
            return midi.size >= 3 && (midi.bytes[0] & 0xF0) == 0x90 && midi.bytes[2] != 0;
        }

        inline bool midi_is_note_off(MidiEvent const& midi)
        {
            if (midi.size < 3) {
                return false;
            }
            std::uint8_t const status = midi.bytes[0] & 0xF0;
            return status == 0x80 || (status == 0x90 && midi.bytes[2] == 0);
        }

        inline bool midi_is_pitch_bend(MidiEvent const& midi)
        {
            return midi.size >= 3 && (midi.bytes[0] & 0xF0) == 0xE0;
        }

        inline std::uint8_t midi_note_number(MidiEvent const& midi)
        {
            return midi.bytes[1];
        }

        inline std::uint16_t midi_pitch_bend_value(MidiEvent const& midi)
        {
            return static_cast<std::uint16_t>(midi.bytes[1]) | (static_cast<std::uint16_t>(midi.bytes[2]) << 7);
        }

        inline Sample midi_velocity_to_amplitude(MidiEvent const& midi)
        {
            return midi.size >= 3
                ? static_cast<Sample>(midi.bytes[2]) / static_cast<Sample>(127.0f)
                : static_cast<Sample>(0.0f);
        }
    }

    class MidiPitch {
        Sample _pitch_bend_range_semitones = 2.0f;

        static Sample bend_multiplier(std::uint16_t bend_value, Sample bend_range)
        {
            double const normalized = (static_cast<int>(bend_value) - 8192) / 8192.0;
            double const semitones = normalized * static_cast<double>(bend_range);
            return static_cast<Sample>(std::exp2(semitones / 12.0));
        }

    public:
        struct State {
            std::array<std::uint16_t, 128> note_counts {};
            std::array<std::uint8_t, 128> note_stack {};
            size_t note_stack_size = 0;
            std::uint16_t pitch_bend = 8192;
            std::span<Sample> block;
        };

        explicit MidiPitch(Sample pitch_bend_range_semitones = 2.0f) :
            _pitch_bend_range_semitones(pitch_bend_range_semitones)
        {}

        auto event_inputs() const
        {
            return std::array<EventInputConfig, 1> {{
                { .name = "midi", .type = EventTypeId::midi }
            }};
        }

        auto outputs() const
        {
            return std::array<OutputConfig, 1> {{
                { .name = "frequency" }
            }};
        }

        void declare(DeclarationContext<MidiPitch> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.local_array(state.block, ctx.max_block_size());
        }

        static void push_note(State& state, std::uint8_t note)
        {
            if (state.note_stack_size < state.note_stack.size()) {
                state.note_stack[state.note_stack_size++] = note;
            }
            ++state.note_counts[note];
        }

        static void release_note(State& state, std::uint8_t note)
        {
            if (state.note_counts[note] == 0) {
                return;
            }

            --state.note_counts[note];
            for (size_t i = state.note_stack_size; i > 0; --i) {
                if (state.note_stack[i - 1] == note) {
                    std::move(
                        state.note_stack.begin() + static_cast<std::ptrdiff_t>(i),
                        state.note_stack.begin() + static_cast<std::ptrdiff_t>(state.note_stack_size),
                        state.note_stack.begin() + static_cast<std::ptrdiff_t>(i - 1)
                    );
                    --state.note_stack_size;
                    return;
                }
            }
        }

        Sample current_frequency(State const& state) const
        {
            if (state.note_stack_size == 0) {
                return 0.0f;
            }

            std::uint8_t const note = state.note_stack[state.note_stack_size - 1];
            return static_cast<Sample>(NOTE_NUMBER_TO_FREQUENCY[note]) * bend_multiplier(state.pitch_bend, _pitch_bend_range_semitones);
        }

        void apply_midi(State& state, MidiEvent const& midi) const
        {
            if (details::midi_is_note_on(midi)) {
                push_note(state, details::midi_note_number(midi));
            } else if (details::midi_is_note_off(midi)) {
                release_note(state, details::midi_note_number(midi));
            } else if (details::midi_is_pitch_bend(midi)) {
                state.pitch_bend = details::midi_pitch_bend_value(midi);
            }
        }

        void tick_block(TickBlockContext<MidiPitch> const& ctx) const
        {
            auto& state = ctx.state();
            size_t cursor = 0;
            Sample value = current_frequency(state);

            auto const events = ctx.event_inputs[0].get_block(ctx.index, ctx.block_size);
            for (TimedEvent const& event : events) {
                size_t const event_offset = event.time - ctx.index;
                size_t const next = std::min(event_offset, ctx.block_size);
                std::fill(state.block.begin() + static_cast<std::ptrdiff_t>(cursor), state.block.begin() + static_cast<std::ptrdiff_t>(next), value);
                cursor = next;

                if (auto const* midi = std::get_if<MidiEvent>(&event.value)) {
                    apply_midi(state, *midi);
                    value = current_frequency(state);
                }
            }

            std::fill(state.block.begin() + static_cast<std::ptrdiff_t>(cursor), state.block.begin() + static_cast<std::ptrdiff_t>(ctx.block_size), value);
            ctx.outputs[0].push_block(std::span<Sample const>(state.block.data(), ctx.block_size));
        }
    };

    class MidiGate {
    public:
        struct State {
            std::array<std::uint16_t, 128> note_counts {};
            size_t active_note_count = 0;
            std::span<Sample> block;
        };

        auto event_inputs() const
        {
            return std::array<EventInputConfig, 1> {{
                { .name = "midi", .type = EventTypeId::midi }
            }};
        }

        auto outputs() const
        {
            return std::array<OutputConfig, 1> {{
                { .name = "gate" }
            }};
        }

        void declare(DeclarationContext<MidiGate> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.local_array(state.block, ctx.max_block_size());
        }

        static Sample current_gate(State const& state)
        {
            return state.active_note_count == 0 ? 0.0f : 1.0f;
        }

        static void apply_midi(State& state, MidiEvent const& midi)
        {
            if (details::midi_is_note_on(midi)) {
                ++state.note_counts[details::midi_note_number(midi)];
                ++state.active_note_count;
            } else if (details::midi_is_note_off(midi)) {
                auto& count = state.note_counts[details::midi_note_number(midi)];
                if (count == 0) {
                    return;
                }
                --count;
                --state.active_note_count;
            }
        }

        void tick_block(TickBlockContext<MidiGate> const& ctx) const
        {
            auto& state = ctx.state();
            size_t cursor = 0;
            Sample value = current_gate(state);

            auto const events = ctx.event_inputs[0].get_block(ctx.index, ctx.block_size);
            for (TimedEvent const& event : events) {
                size_t const event_offset = event.time - ctx.index;
                size_t const next = std::min(event_offset, ctx.block_size);
                std::fill(state.block.begin() + static_cast<std::ptrdiff_t>(cursor), state.block.begin() + static_cast<std::ptrdiff_t>(next), value);
                cursor = next;

                if (auto const* midi = std::get_if<MidiEvent>(&event.value)) {
                    apply_midi(state, *midi);
                    value = current_gate(state);
                }
            }

            std::fill(state.block.begin() + static_cast<std::ptrdiff_t>(cursor), state.block.begin() + static_cast<std::ptrdiff_t>(ctx.block_size), value);
            ctx.outputs[0].push_block(std::span<Sample const>(state.block.data(), ctx.block_size));
        }
    };

    template<size_t voice_id, size_t voice_count>
    class MidiVoiceAllocator {
        static_assert(voice_count > 0, "MidiVoiceAllocator requires at least one voice");
        static_assert(voice_id < voice_count, "MidiVoiceAllocator voice id must be within voice count");

        Sample _pitch_bend_range_semitones = 2.0f;

        static Sample bend_multiplier(std::uint16_t bend_value, Sample bend_range)
        {
            double const normalized = (static_cast<int>(bend_value) - 8192) / 8192.0;
            double const semitones = normalized * static_cast<double>(bend_range);
            return static_cast<Sample>(std::exp2(semitones / 12.0));
        }

        static size_t find_voice_for_note(FastBitset<voice_count> const& allocated, std::array<std::uint8_t, voice_count> const& notes, std::uint8_t note)
        {
            for (size_t voice : allocated) {
                if (notes[voice] == note) {
                    return voice;
                }
            }
            return voice_count;
        }

        static size_t find_oldest_voice(FastBitset<voice_count> const& allocated, std::array<std::uint32_t, voice_count> const& ages)
        {
            size_t selected = voice_count;
            std::uint32_t selected_age = 0;
            for (size_t voice : allocated) {
                if (selected == voice_count || ages[voice] < selected_age) {
                    selected = voice;
                    selected_age = ages[voice];
                }
            }
            return selected;
        }

        Sample current_frequency(std::uint8_t note, std::uint16_t pitch_bend) const
        {
            return static_cast<Sample>(NOTE_NUMBER_TO_FREQUENCY[note]) * bend_multiplier(pitch_bend, _pitch_bend_range_semitones);
        }

    public:
        struct State {
            FastBitset<voice_count> allocated {};
            std::array<std::uint8_t, voice_count> notes {};
            std::array<std::uint32_t, voice_count> ages {};
            std::uint32_t next_age = 1;
            std::uint16_t pitch_bend = 8192;
            Sample amplitude = 0;
            Sample frequency = 0;
        };

        explicit MidiVoiceAllocator(Sample pitch_bend_range_semitones = 2) :
            _pitch_bend_range_semitones(pitch_bend_range_semitones)
        {}

        auto event_inputs() const
        {
            return std::array<EventInputConfig, 1> {{
                { .name = "midi", .type = EventTypeId::midi }
            }};
        }

        auto outputs() const
        {
            return std::array<OutputConfig, 2> {{
                { .name = "amplitude" },
                { .name = "frequency" },
            }};
        }

        auto event_outputs() const
        {
            return std::array<EventOutputConfig, 1> {{
                { .name = "trigger", .type = EventTypeId::trigger }
            }};
        }

        void tick_block(TickBlockContext<MidiVoiceAllocator> const& ctx) const
        {
            auto& state = ctx.state();

            auto push_until = [&](size_t until, size_t& cursor) {
                while (cursor < until) {
                    ctx.outputs[0].push(state.amplitude);
                    ctx.outputs[1].push(state.frequency);
                    ++cursor;
                }
            };

            auto assign_voice = [&](size_t voice, std::uint8_t note, Sample amplitude, size_t event_time) {
                bool const was_allocated = state.allocated.test(voice);
                bool const assignment_changed = !was_allocated || state.notes[voice] != note;
                state.allocated.set(voice);
                state.notes[voice] = note;
                state.ages[voice] = state.next_age++;
                if constexpr (voice_id < voice_count) {
                    if (voice == voice_id) {
                        state.frequency = current_frequency(note, state.pitch_bend);
                        state.amplitude = amplitude;
                        if (assignment_changed) {
                            ctx.event_outputs[0].push(TriggerEvent {}, event_time, ctx.index, ctx.block_size);
                        }
                    }
                }
            };

            auto release_voice = [&](size_t voice) {
                state.allocated.reset(voice);
                if constexpr (voice_id < voice_count) {
                    if (voice == voice_id) {
                        state.amplitude = 0.0f;
                    }
                }
            };

            size_t cursor = 0;
            auto const events = ctx.event_inputs[0].get_block(ctx.index, ctx.block_size);
            for (TimedEvent const& event : events) {
                size_t const event_offset = event.time - ctx.index;
                size_t const next = std::min(event_offset, ctx.block_size);
                push_until(next, cursor);

                auto const* midi = std::get_if<MidiEvent>(&event.value);
                if (!midi) {
                    continue;
                }

                if (details::midi_is_note_on(*midi)) {
                    std::uint8_t const note = details::midi_note_number(*midi);
                    size_t target_voice = find_voice_for_note(state.allocated, state.notes, note);
                    if (target_voice == voice_count) {
                        target_voice = (~state.allocated).first_set();
                    }
                    if (target_voice == voice_count) {
                        target_voice = find_oldest_voice(state.allocated, state.ages);
                    }
                    assign_voice(target_voice, note, details::midi_velocity_to_amplitude(*midi), event_offset);
                } else if (details::midi_is_note_off(*midi)) {
                    size_t const target_voice = find_voice_for_note(
                        state.allocated,
                        state.notes,
                        details::midi_note_number(*midi)
                    );
                    if (target_voice != voice_count) {
                        release_voice(target_voice);
                    }
                } else if (details::midi_is_pitch_bend(*midi)) {
                    state.pitch_bend = details::midi_pitch_bend_value(*midi);
                    if (state.allocated.test(voice_id)) {
                        state.frequency = current_frequency(state.notes[voice_id], state.pitch_bend);
                    }
                }
            }

            push_until(ctx.block_size, cursor);
        }
    };
}
