#include <intravenous/basic_nodes/system_audio.h>

void system_audio_loopback(iv::GraphBuilder& graph)
{
    auto input = graph.node<iv::SystemAudioInput>();
    auto output = graph.node<iv::SystemAudioOutput>();
    output(input);
    graph.outputs();
}

IV_MODULE("iv.test.graph_jit.system_audio_loopback",
    system_audio_loopback, "System Audio Loopback");
