#pragma once
#include "graph_node.h"
#include "node.h"
#include "fast_bitset.h"
#include "note_number_lookup_table.h"
#include <memory>
#include <cmath>
#include <vector>


namespace iv {
    class MidiNode {
        Graph _graph_node;
        size_t _internal_latency_cache;
        Sample _silence_threshold;

    public:
        static constexpr size_t const MAX_CHANNELS = 16;
        static constexpr size_t const MIDI_NOTES_PER_CHANNEL = 128;
        static constexpr size_t const MAX_MIDI_NOTES = MIDI_NOTES_PER_CHANNEL * MAX_CHANNELS;
        static_assert(MAX_MIDI_NOTES > 0);
        static constexpr size_t const MAX_MIDI_NOTES_UINT64 = (MAX_MIDI_NOTES-1) / (sizeof(uint64_t)*CHAR_BIT) + 1;
        using Bitset = FastBitset<MAX_MIDI_NOTES_UINT64>;
        static constexpr size_t const MIN_INPUTS = 0;
        static constexpr size_t const MIN_GRAPH_INPUTS = 3;
        static constexpr size_t const MIN_GRAPH_OUTPUTS = 1;

        struct MidiNoteState : public NodeState {
            size_t ttl{ 0 };
            size_t amplitude{ 0 };
        };

        struct MidiChannelState {
            float bend_semitones{ 0.f };
        };

        struct MidiState : public NodeState {
            Bitset active_notes;
            std::array<MidiNoteState, MAX_MIDI_NOTES> note_states;
            std::array<MidiChannelState, MAX_CHANNELS> channel_states;
        };

        explicit MidiNode(
            Graph voice_node,
            Sample silence_threshold = std::pow(10.0, -60.0 / 20.0)  // -60db
        ) :
            _graph_node(std::move(voice_node)),
            _internal_latency_cache(get_internal_latency(_graph_node)),
            _silence_threshold(silence_threshold)
        {
            assert(get_num_inputs(_graph_node) >= MIN_GRAPH_INPUTS && "the voice graph should have at least 3 inputs");
            assert(get_num_outputs(_graph_node) == MIN_GRAPH_OUTPUTS && "the voice graph should have exactly 1 output");
        }

        constexpr auto inputs() const
        {
            return std::vector<InputConfig>(get_num_inputs(_graph_node) - MIN_GRAPH_INPUTS + MIN_INPUTS);
        }

        constexpr auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick(TickState const& state)
        {
            MidiState& midi_state = get_midi_state(state);

            for (auto const& midi_message : state.midi)
            {
                if (midi_message.type == MidiMessageType::NOTE_ON)
                {
                    size_t note_index = midi_message.note_on.note_number + MIDI_NOTES_PER_CHANNEL * midi_message.note_on.channel;
                    MidiNoteState& note_state = midi_state.note_states[note_index];

                    // reset state
                    FixedBufferAllocator alloc { note_state.buffer };
                    _graph_node.init_buffer(alloc);

                    note_state.amplitude = midi_message.note_on.amplitude;
                    note_state.ttl = _internal_latency_cache;
                    midi_state.active_notes.set(note_index);
                }
                else if (midi_message.type == MidiMessageType::NOTE_OFF)
                {
                    size_t note_index = midi_message.note_off.note_number + MIDI_NOTES_PER_CHANNEL * midi_message.note_off.channel;
                    MidiNoteState& note_state = midi_state.note_states[note_index];
                    note_state.amplitude = 0;
                }
                else if (midi_message.type == MidiMessageType::PITCH_WHEEL)
                {
                    MidiChannelState& note_state = midi_state.channel_states[midi_message.pitch_wheel.channel];
                    float normalized_pitch = (midi_message.pitch_wheel.pitch_value - 8192) / 8192.0f;
                    note_state.bend_semitones = normalized_pitch * 64.f;
                }
            }

            Sample result = 0.0;
            for (size_t note_number : midi_state.active_notes)
            {
                size_t channel_note_number = note_number % MIDI_NOTES_PER_CHANNEL;
                size_t channel = note_number / MIDI_NOTES_PER_CHANNEL;
                MidiNoteState& note_state = midi_state.note_states[note_number];
                MidiChannelState& channel_state = midi_state.channel_states[channel];

                if (note_state.amplitude)
                {
                    note_state.ttl = _internal_latency_cache;
                }
                else if (note_state.ttl)
                {
                    --note_state.ttl;
                }
                else if (
                    auto last_output = note_state.outputs[0].get();
                    last_output <= _silence_threshold && last_output >= -_silence_threshold)
                {
                    midi_state.active_notes.reset(note_number);
                    continue;
                }

                Sample pitch = Sample(NOTE_NUMBER_TO_FREQUENCY[channel_note_number]);
                pitch *= std::pow(2.0, channel_state.bend_semitones / 12.0);

                note_state.inputs[0].push(Sample(note_state.amplitude / 127.0));
                note_state.inputs[1].push(pitch);
                for (size_t extra_i = 0; extra_i < midi_state.inputs.size() - MIN_GRAPH_INPUTS; ++extra_i)
                {
                    note_state.inputs[extra_i + MIN_GRAPH_INPUTS].push(state.inputs[extra_i + MIN_INPUTS].get());
                }
                _graph_node.tick({ static_cast<NodeState&>(note_state), state.midi, state.index });
                result += note_state.outputs[0].get();
            }

            auto& out_mix = state.outputs[0];
            out_mix.push(result);
        }

        constexpr size_t internal_latency() const
        {
            return _internal_latency_cache;
        }

        template<typename Allocator>
        void init_buffer(Allocator& allocator, GraphInitContext& ctx) const
        {
            /*
            * struct MemoryLayout {
            *     State;              // MidiNode state
            *     SharedPortData[i];
            *     InputPort[i];       // voice graph inputs (shared)
            *     Sample[s1];          // voice graph input samples (shared)
            *     struct {
            *         std::byte[b];   // voice graph data
            *     }[n];
            *     SharedPortData[1];
            *     OutputPort[1];      // voice graph output (shared)
            *     Sample[s2];          // voice graph output samples (shared)
            * };
            */

            MidiState& midi_state = allocator.template new_object<MidiState>();

            auto input_configs = get_inputs(_graph_node);

            std::span<SharedPortData> all_input_data = allocator.template allocate_array<SharedPortData>(input_configs.size());
            allocator.assign(midi_state.inputs, allocator.template allocate_array<InputPort>(input_configs.size()));

            for (size_t input_i = 0; input_i < input_configs.size(); ++input_i)
            {
                size_t num_samples = calculate_port_buffer_size(0, input_configs[input_i].history, 0);
                std::span<Sample> samples = allocator.template new_array<Sample>(num_samples);
                allocator.fill_n(samples, input_configs[input_i].default_value);

                SharedPortData& input_data = allocator.at(all_input_data, input_i);
                InputPort& input_port = allocator.at(midi_state.inputs, input_i);
                allocator.construct_at(&input_data, samples, 0);
                allocator.construct_at(&input_port, input_data, input_configs[input_i].history);
            }

            for (size_t note = 0; note < MAX_MIDI_NOTES; ++note)
            {
                MidiNoteState& midi_note_state = allocator.at(midi_state.note_states, note);
                allocator.assign(midi_note_state.buffer, do_init_buffer(_graph_node, allocator, ctx));
                allocator.assign(midi_note_state.inputs, midi_state.inputs);
            }

            auto output_configs = get_outputs(_graph_node);

            std::span<SharedPortData> all_output_data = allocator.template allocate_array<SharedPortData>(1);
            allocator.assign(midi_state.outputs, allocator.template allocate_array<OutputPort>(1));
            size_t num_output_samples = calculate_port_buffer_size(output_configs[0].latency, 0, output_configs[0].history);
            std::span<Sample> output_samples = allocator.template new_array<Sample>(num_output_samples);

            SharedPortData& output_data = allocator.at(all_output_data, 0);
            OutputPort& output_port = allocator.at(midi_state.outputs, 0);

            allocator.construct_at(&output_data, output_samples, output_configs[0].latency);
            allocator.construct_at(&output_port, output_data, 0);

            for (size_t note = 0; note < MAX_MIDI_NOTES; ++note)
            {
                MidiNoteState& midi_note_state = allocator.at(midi_state.note_states, note);
                allocator.assign(midi_note_state.outputs, midi_state.outputs);
            }

            for (size_t channel = 0; channel < MAX_CHANNELS; ++channel)
            {
                MidiChannelState& midi_channel_state = allocator.at(midi_state.channel_states, channel);
                allocator.assign(midi_channel_state.bend_semitones, 0.f);
            }
        }

        MidiState& get_midi_state(NodeState const& state) const
        {
            void* object = state.buffer.data();
            size_t size = state.buffer.size();
            return *reinterpret_cast<MidiState*>(std::align(alignof(MidiState), sizeof(MidiState), object, size));
        }
    };
}
