#pragma once
#include "node.h"
#include "graph_node.h"
#include <atomic>


namespace iv {
    NodeProcessor* init_graph(
        double* frequency_value,
        Sample* write_buffer[2],
        std::atomic<float>* uniform_noise_level,
        std::atomic<float>* gaussian_noise_level,
        std::atomic<float>* harmonics_noise_ratio,
        std::atomic<float>* noise_lo_pass,
        std::atomic<float>* noise_hi_pass
    );
    void tick(NodeProcessor* processor, std::span<MidiMessage const> midi, size_t index);
    void free_graph(NodeProcessor* processor);
}
