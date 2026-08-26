#pragma once

#include <intravenous/sample.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace iv {
struct JuceVstParameterSpec {
    std::string id;
    std::string name;
    Sample default_value = 0.0f;
    Sample min = 0.0f;
    Sample max = 1.0f;
};

struct JuceVstSchema {
    size_t audio_inputs = 0;
    size_t audio_outputs = 0;
    std::vector<std::string> audio_input_names;
    std::vector<std::string> audio_output_names;
    std::vector<JuceVstParameterSpec> parameters;
    uint64_t fingerprint = 0;
};

struct JuceVstPluginConfig {
    std::filesystem::path plugin_path;
    std::string plugin_identifier;
    size_t preferred_audio_input_streams = 1;
    size_t preferred_audio_output_streams = 1;
};

struct JuceVstWrapperSpec {
    JuceVstPluginConfig plugin;
    JuceVstSchema schema;
};

JuceVstWrapperSpec probe_juce_vst(JuceVstPluginConfig request);
} // namespace iv
