#define IV_INTERNAL_TRANSLATION_UNIT

#include "vst_runtime.h"

#if IV_ENABLE_JUCE_VST

#include "midi_input.h"
#include "vst_wrapper.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace iv {
    namespace {
        uint64_t fnv1a_append(uint64_t hash, std::string_view text)
        {
            for (unsigned char c : text) {
                hash ^= c;
                hash *= 1099511628211ull;
            }
            return hash;
        }

        uint64_t make_schema_fingerprint(size_t audio_inputs, size_t audio_outputs, std::span<JuceVstParameterSpec const> parameters)
        {
            uint64_t hash = 1469598103934665603ull;
            hash = fnv1a_append(hash, std::to_string(audio_inputs));
            hash = fnv1a_append(hash, std::to_string(audio_outputs));
            for (auto const& parameter : parameters) {
                hash = fnv1a_append(hash, parameter.id);
                hash = fnv1a_append(hash, parameter.name);
                hash = fnv1a_append(hash, std::to_string(parameter.default_value));
            }
            return hash;
        }

        std::string canonical_plugin_path(std::filesystem::path const& path)
        {
            std::error_code ec;
            auto canonical = std::filesystem::weakly_canonical(path, ec);
            if (!ec) {
                return canonical.generic_string();
            }
            return std::filesystem::absolute(path).lexically_normal().generic_string();
        }

        ::juce::String to_juce_string(std::filesystem::path const& path)
        {
            return ::juce::String(path.string());
        }

        std::runtime_error make_probe_error(std::string const& message, JuceVstPluginConfig const& config)
        {
            return std::runtime_error(message + ": " + canonical_plugin_path(config.plugin_path));
        }

        std::string lowercase_ascii(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return text;
        }

        char search_path_separator()
        {
#if defined(_WIN32)
            return ';';
#else
            return ':';
#endif
        }

        std::vector<std::filesystem::path> parse_vst_search_roots_from_env()
        {
            std::vector<std::filesystem::path> roots;
            char const* raw = std::getenv("IV_VST3_PATH");
            if (!raw || !*raw) {
                return roots;
            }

            std::string_view remaining(raw);
            char const separator = search_path_separator();
            while (!remaining.empty()) {
                size_t split = remaining.find(separator);
                std::string_view token = remaining.substr(0, split);
                if (!token.empty()) {
                    roots.emplace_back(std::string(token));
                }
                if (split == std::string_view::npos) {
                    break;
                }
                remaining.remove_prefix(split + 1);
            }
            return roots;
        }

        std::vector<std::filesystem::path> default_vst_search_roots()
        {
            std::vector<std::filesystem::path> roots;
#if defined(_WIN32)
            if (char const* common = std::getenv("COMMONPROGRAMFILES"); common && *common) {
                roots.emplace_back(std::filesystem::path(common) / "VST3");
            }
            if (char const* pf = std::getenv("ProgramFiles"); pf && *pf) {
                roots.emplace_back(std::filesystem::path(pf) / "Common Files" / "VST3");
            }
#else
            if (char const* home = std::getenv("HOME"); home && *home) {
                roots.emplace_back(std::filesystem::path(home) / ".vst3");
            }
            roots.emplace_back("/usr/lib/vst3");
            roots.emplace_back("/usr/local/lib/vst3");
#endif
            return roots;
        }

        struct VstSearchCache {
            std::vector<std::filesystem::path> entries;
            std::unordered_multimap<std::string, size_t> by_name;
        };

        std::filesystem::path canonical_or_absolute(std::filesystem::path const& path)
        {
            std::error_code ec;
            auto canonical = std::filesystem::weakly_canonical(path, ec);
            if (!ec) {
                return canonical;
            }
            return std::filesystem::absolute(path).lexically_normal();
        }

        void index_vst_entry(VstSearchCache& cache, std::filesystem::path const& entry)
        {
            std::filesystem::path normalized = canonical_or_absolute(entry);
            size_t const index = cache.entries.size();
            cache.entries.push_back(normalized);

            std::string const filename = lowercase_ascii(normalized.filename().string());
            std::string const stem = lowercase_ascii(normalized.stem().string());
            if (!filename.empty()) {
                cache.by_name.emplace(filename, index);
            }
            if (!stem.empty() && stem != filename) {
                cache.by_name.emplace(stem, index);
            }
        }

        VstSearchCache build_vst_search_cache()
        {
            VstSearchCache cache;

            std::vector<std::filesystem::path> roots = parse_vst_search_roots_from_env();
            if (roots.empty()) {
                roots = default_vst_search_roots();
            }

            for (auto const& root : roots) {
                std::error_code ec;
                if (!std::filesystem::exists(root, ec)) {
                    continue;
                }

                if (root.extension() == ".vst3") {
                    index_vst_entry(cache, root);
                    continue;
                }

                if (!std::filesystem::is_directory(root, ec)) {
                    continue;
                }

                for (
                    std::filesystem::recursive_directory_iterator it(
                        root,
                        std::filesystem::directory_options::skip_permission_denied
                    ), end;
                    it != end;
                    it.increment(ec)
                ) {
                    if (ec) {
                        ec.clear();
                        continue;
                    }

                    std::filesystem::path const path = it->path();
                    if (path.extension() != ".vst3") {
                        continue;
                    }
                    index_vst_entry(cache, path);
                }
            }

            return cache;
        }

        VstSearchCache const& vst_search_cache()
        {
            static VstSearchCache cache = build_vst_search_cache();
            return cache;
        }

        std::filesystem::path resolve_plugin_path(std::filesystem::path const& query_path)
        {
            if (query_path.empty()) {
                throw std::runtime_error("plugin path cannot be empty");
            }

            std::error_code ec;
            if (
                std::filesystem::exists(query_path, ec)
                || (!query_path.has_parent_path() && query_path.extension() == ".vst3" && std::filesystem::exists(std::filesystem::current_path() / query_path, ec))
            ) {
                return canonical_or_absolute(query_path);
            }

            // Treat bare names as lookup keys in startup scan cache.
            if (query_path.has_parent_path()) {
                throw std::runtime_error("plugin file was not found: " + canonical_or_absolute(query_path).string());
            }

            std::string key = lowercase_ascii(query_path.string());
            if (!key.ends_with(".vst3")) {
                key += ".vst3";
            }

            auto const& cache = vst_search_cache();
            std::vector<size_t> matches;
            auto range = cache.by_name.equal_range(key);
            for (auto it = range.first; it != range.second; ++it) {
                matches.push_back(it->second);
            }

            if (matches.empty()) {
                // Retry by stem (name without extension)
                std::filesystem::path key_path(key);
                std::string const stem_key = lowercase_ascii(key_path.stem().string());
                auto stem_range = cache.by_name.equal_range(stem_key);
                for (auto it = stem_range.first; it != stem_range.second; ++it) {
                    matches.push_back(it->second);
                }
            }

            std::sort(matches.begin(), matches.end());
            matches.erase(std::unique(matches.begin(), matches.end()), matches.end());

            if (matches.empty()) {
                throw std::runtime_error(
                    "plugin '" + query_path.string() + "' was not found in scanned VST3 paths. "
                    "Set IV_VST3_PATH to a directory or path list."
                );
            }

            if (matches.size() > 1) {
                std::string message = "plugin name '" + query_path.string() + "' is ambiguous";
                size_t const shown = std::min<size_t>(matches.size(), 8);
                for (size_t i = 0; i < shown; ++i) {
                    message += "\nmatch: " + cache.entries[matches[i]].string();
                }
                if (matches.size() > shown) {
                    message += "\nmatch: ...";
                }
                throw std::runtime_error(message);
            }

            return cache.entries[matches.front()];
        }

        std::string make_audio_port_name(size_t channel)
        {
            return std::string(channel % 2 == 0 ? "l" : "r") + std::to_string(channel / 2);
        }

        MidiEvent make_iv_midi_event(::juce::MidiMessage const& message)
        {
            MidiEvent midi {};
            auto const* raw = message.getRawData();
            int const raw_size = message.getRawDataSize();
            if (raw == nullptr || raw_size <= 0 || raw_size > static_cast<int>(midi.bytes.size())) {
                return {};
            }

            std::copy_n(raw, static_cast<size_t>(raw_size), midi.bytes.begin());
            midi.size = static_cast<std::uint8_t>(raw_size);
            return midi;
        }

        size_t saturating_stream_channels(size_t streams)
        {
            if (streams > std::numeric_limits<size_t>::max() / 2) {
                return std::numeric_limits<size_t>::max();
            }
            return streams * 2;
        }

        ::juce::PluginDescription resolve_plugin_description(
            ::juce::AudioPluginFormatManager& format_manager,
            JuceVstPluginConfig const& config
        )
        {
            ::juce::OwnedArray<::juce::PluginDescription> descriptions;
            ::juce::String const file_path = to_juce_string(config.plugin_path);
            for (auto* format : format_manager.getFormats()) {
                if (!format->fileMightContainThisPluginType(file_path)) {
                    continue;
                }
                format->findAllTypesForFile(descriptions, file_path);
            }

            if (descriptions.isEmpty()) {
                throw make_probe_error("no JUCE-hostable plugins found in file", config);
            }

            if (!config.plugin_identifier.empty()) {
                for (auto const* description : descriptions) {
                    if (description->createIdentifierString().toStdString() == config.plugin_identifier) {
                        return *description;
                    }
                }
                throw make_probe_error("plugin identifier was not found in file", config);
            }

            if (descriptions.size() != 1) {
                throw make_probe_error("plugin file contains multiple JUCE plugin descriptions but no identifier was provided", config);
            }

            return *descriptions.getFirst();
        }

        std::unique_ptr<::juce::AudioPluginInstance> create_plugin_instance(
            ::juce::AudioPluginFormatManager& format_manager,
            JuceVstPluginConfig const& config,
            double sample_rate,
            int block_size
        )
        {
            ::juce::PluginDescription const description = resolve_plugin_description(format_manager, config);
            ::juce::String error;
            auto instance = format_manager.createPluginInstance(description, sample_rate, block_size, error);
            if (!instance) {
                throw std::runtime_error("JUCE failed to create plugin instance '" + description.createIdentifierString().toStdString() + "': " + error.toStdString());
            }
            return instance;
        }

        bool try_set_bus_layout(
            ::juce::AudioPluginInstance& instance,
            bool is_input,
            std::span<::juce::AudioChannelSet const> bus_layouts
        )
        {
            int const bus_count = instance.getBusCount(is_input);
            if (bus_layouts.size() != static_cast<size_t>(bus_count)) {
                return false;
            }

            for (int bus_index = 0; bus_index < bus_count; ++bus_index) {
                if (!instance.setChannelLayoutOfBus(is_input, bus_index, bus_layouts[bus_index])) {
                    return false;
                }
            }
            return true;
        }

        size_t count_layout_channels(std::span<::juce::AudioChannelSet const> bus_layouts)
        {
            size_t total = 0;
            for (auto const& layout : bus_layouts) {
                total += static_cast<size_t>(layout.size());
            }
            return total;
        }

        size_t configure_audio_direction(
            ::juce::AudioPluginInstance& instance,
            bool is_input,
            size_t preferred_streams
        )
        {
            int const bus_count = instance.getBusCount(is_input);
            if (bus_count <= 0) {
                return 0;
            }

            size_t const desired_channels = saturating_stream_channels(preferred_streams);
            std::vector<size_t> candidate_active_buses;
            candidate_active_buses.reserve(static_cast<size_t>(bus_count) + 1);
            auto push_candidate = [&](size_t active_buses) {
                if (active_buses > static_cast<size_t>(bus_count)) {
                    return;
                }
                if (std::find(candidate_active_buses.begin(), candidate_active_buses.end(), active_buses) == candidate_active_buses.end()) {
                    candidate_active_buses.push_back(active_buses);
                }
            };

            size_t const desired_buses = std::min(preferred_streams, static_cast<size_t>(bus_count));
            push_candidate(desired_buses);
            for (size_t delta = 1; delta <= static_cast<size_t>(bus_count); ++delta) {
                if (desired_buses >= delta) {
                    push_candidate(desired_buses - delta);
                }
                push_candidate(desired_buses + delta);
            }

            for (size_t active_buses : candidate_active_buses) {
                std::vector<::juce::AudioChannelSet> layouts(static_cast<size_t>(bus_count), ::juce::AudioChannelSet::disabled());
                for (size_t bus_index = 0; bus_index < active_buses; ++bus_index) {
                    layouts[bus_index] = ::juce::AudioChannelSet::stereo();
                }
                if (try_set_bus_layout(instance, is_input, layouts)) {
                    return count_layout_channels(layouts);
                }
            }

            std::vector<size_t> candidate_channels;
            candidate_channels.reserve(std::max<size_t>(8, desired_channels / 2 + 2));
            auto push_channel_candidate = [&](size_t channels) {
                if (std::find(candidate_channels.begin(), candidate_channels.end(), channels) == candidate_channels.end()) {
                    candidate_channels.push_back(channels);
                }
            };

            push_channel_candidate(desired_channels);
            for (size_t delta = 2; delta <= std::max<size_t>(desired_channels, 16); delta += 2) {
                if (desired_channels >= delta) {
                    push_channel_candidate(desired_channels - delta);
                }
                push_channel_candidate(desired_channels + delta);
            }
            push_channel_candidate(1);
            push_channel_candidate(0);

            for (size_t channels : candidate_channels) {
                std::vector<::juce::AudioChannelSet> layouts(static_cast<size_t>(bus_count), ::juce::AudioChannelSet::disabled());
                layouts[0] = channels == 0
                    ? ::juce::AudioChannelSet::disabled()
                    : ::juce::AudioChannelSet::discreteChannels(static_cast<int>(channels));
                if (try_set_bus_layout(instance, is_input, layouts)) {
                    return count_layout_channels(layouts);
                }
            }

            return static_cast<size_t>(is_input ? instance.getTotalNumInputChannels() : instance.getTotalNumOutputChannels());
        }

        void configure_audio_layout(::juce::AudioPluginInstance& instance, JuceVstPluginConfig const& config)
        {
            configure_audio_direction(instance, true, config.preferred_audio_input_streams);
            configure_audio_direction(instance, false, config.preferred_audio_output_streams);
        }

        JuceVstParameterSpec make_parameter_spec(::juce::AudioProcessorParameter& parameter, size_t index)
        {
            JuceVstParameterSpec spec;
            if (auto* with_id = dynamic_cast<::juce::AudioProcessorParameterWithID*>(&parameter)) {
                spec.id = with_id->paramID.toStdString();
            } else {
                spec.id = "param_" + std::to_string(index);
            }
            spec.name = parameter.getName(128).toStdString();
            if (spec.name.empty()) {
                spec.name = spec.id;
            }
            spec.default_value = parameter.getDefaultValue();
            return spec;
        }

        struct ParameterBinding {
            ::juce::AudioProcessorParameter* parameter = nullptr;
            float last_value = std::numeric_limits<float>::quiet_NaN();
        };

        ::juce::MidiDeviceInfo resolve_midi_input_device(std::string const& device_query)
        {
            auto const devices = ::juce::MidiInput::getAvailableDevices();
            if (devices.isEmpty()) {
                throw std::runtime_error("no JUCE MIDI input devices are available");
            }

            auto find_exact_identifier = [&](std::string const& query) -> std::optional<::juce::MidiDeviceInfo> {
                for (auto const& device : devices) {
                    if (device.identifier.toStdString() == query) {
                        return device;
                    }
                }
                return std::nullopt;
            };

            auto find_exact_name = [&](std::string const& query) -> std::optional<::juce::MidiDeviceInfo> {
                for (auto const& device : devices) {
                    if (device.name.toStdString() == query) {
                        return device;
                    }
                }
                return std::nullopt;
            };

            auto find_contains = [&](std::string const& query) -> std::optional<::juce::MidiDeviceInfo> {
                if (query.empty()) {
                    return std::nullopt;
                }
                auto const query_text = ::juce::String(query);
                for (auto const& device : devices) {
                    if (device.name.containsIgnoreCase(query_text) || device.identifier.containsIgnoreCase(query_text)) {
                        return device;
                    }
                }
                return std::nullopt;
            };

            if (device_query.empty()) {
                auto const default_device = ::juce::MidiInput::getDefaultDevice();
                if (!default_device.identifier.isEmpty()) {
                    return default_device;
                }
                return devices.getFirst();
            }

            if (auto match = find_exact_identifier(device_query)) {
                return *match;
            }
            if (auto match = find_exact_name(device_query)) {
                return *match;
            }
            if (auto match = find_contains(device_query)) {
                return *match;
            }

            throw std::runtime_error("JUCE MIDI input device was not found: " + device_query);
        }
    }

    struct JuceVstRuntimeManager::LiveInstance {
        std::shared_ptr<JuceVstWrapperSpec const> spec;
        std::unique_ptr<::juce::AudioPluginInstance> plugin;
        ::juce::AudioBuffer<float> audio_buffer;
        ::juce::MidiBuffer midi_buffer;
        std::vector<ParameterBinding> parameter_bindings;
        double prepared_sample_rate = 0.0;
        int prepared_block_size = 0;

        void prepare(double sample_rate, size_t block_size)
        {
            if (prepared_sample_rate == sample_rate && prepared_block_size == static_cast<int>(block_size)) {
                return;
            }

            plugin->suspendProcessing(true);
            plugin->releaseResources();
            plugin->setRateAndBufferSizeDetails(sample_rate, static_cast<int>(block_size));
            plugin->prepareToPlay(sample_rate, static_cast<int>(block_size));
            plugin->suspendProcessing(false);

            prepared_sample_rate = sample_rate;
            prepared_block_size = static_cast<int>(block_size);
            audio_buffer.setSize(
                static_cast<int>(std::max(spec->schema.audio_inputs, spec->schema.audio_outputs)),
                static_cast<int>(block_size),
                false,
                true,
                true
            );
        }
    };

    struct JuceVstRuntimeManager::Impl {
        ::juce::AudioPluginFormatManager format_manager;

        Impl()
        {
            format_manager.addDefaultFormats();
        }
    };

    struct JuceVstRuntimeManager::LiveMidiInput : ::juce::MidiInputCallback {
        struct PendingMessage {
            std::chrono::steady_clock::time_point arrival_time {};
            MidiEvent midi {};
        };

        std::unique_ptr<::juce::MidiInput> input;
        double sample_rate = 48000.0;
        std::optional<std::chrono::steady_clock::time_point> render_epoch_time;
        int64_t render_epoch_sample = 0;
        std::mutex mutex;
        std::deque<PendingMessage> pending_messages;

        void handleIncomingMidiMessage(::juce::MidiInput*, ::juce::MidiMessage const& message) override
        {
            MidiEvent const midi = make_iv_midi_event(message);
            if (midi.size == 0) {
                return;
            }

            std::lock_guard lock(mutex);
            pending_messages.push_back(PendingMessage {
                .arrival_time = std::chrono::steady_clock::now(),
                .midi = midi,
            });
        }

        void handlePartialSysexMessage(::juce::MidiInput*, ::juce::uint8 const*, int, double) override
        {
        }
    };

    JuceVstRuntimeManager::JuceVstRuntimeManager() :
        _impl(std::make_unique<Impl>())
    {}

    JuceVstRuntimeManager::~JuceVstRuntimeManager() = default;

    JuceVstRuntimeSupport::JuceVstRuntimeSupport(JuceVstRuntimeManager& manager, double sample_rate) :
        _manager(&manager),
        _sample_rate(sample_rate),
        _vst_resources{
            .owner = this,
            .create_juce_vst_fn = [](void* owner, JuceVstWrapperSpec const& spec) {
                auto& support = *static_cast<JuceVstRuntimeSupport*>(owner);
                return support._manager->create_instance(spec, support._sample_rate);
            },
        },
        _midi_input_resources{
            .owner = this,
            .create_juce_midi_input_fn = [](void* owner, JuceMidiInputSpec const& spec) {
                auto& support = *static_cast<JuceVstRuntimeSupport*>(owner);
                return support._manager->create_midi_input(spec, support._sample_rate);
            },
        },
        _resources {
            .vst = _vst_resources,
            .midi_input = _midi_input_resources,
        }
    {}

    JuceVstWrapperSpec probe_juce_vst(JuceVstPluginConfig request)
    {
        request.plugin_path = resolve_plugin_path(request.plugin_path);

        ::juce::AudioPluginFormatManager format_manager;
        format_manager.addDefaultFormats();

        ::juce::PluginDescription const description = resolve_plugin_description(format_manager, request);
        auto instance = create_plugin_instance(format_manager, request, 48000.0, 512);
        configure_audio_layout(*instance, request);

        JuceVstWrapperSpec spec;
        spec.plugin.plugin_path = std::filesystem::weakly_canonical(request.plugin_path);
        spec.plugin.plugin_identifier = description.createIdentifierString().toStdString();
        spec.plugin.preferred_audio_input_streams = request.preferred_audio_input_streams;
        spec.plugin.preferred_audio_output_streams = request.preferred_audio_output_streams;
        spec.schema.audio_inputs = static_cast<size_t>(instance->getTotalNumInputChannels());
        spec.schema.audio_outputs = static_cast<size_t>(instance->getTotalNumOutputChannels());
        spec.schema.audio_input_names.reserve(spec.schema.audio_inputs);
        for (size_t channel = 0; channel < spec.schema.audio_inputs; ++channel) {
            spec.schema.audio_input_names.push_back(make_audio_port_name(channel));
        }
        spec.schema.audio_output_names.reserve(spec.schema.audio_outputs);
        for (size_t channel = 0; channel < spec.schema.audio_outputs; ++channel) {
            spec.schema.audio_output_names.push_back(make_audio_port_name(channel));
        }
        auto& parameters = instance->getParameters();
        spec.schema.parameters.reserve(parameters.size());
        for (int index = 0; index < parameters.size(); ++index) {
            spec.schema.parameters.push_back(make_parameter_spec(*parameters[index], size_t(index)));
        }
        spec.schema.fingerprint = make_schema_fingerprint(
            spec.schema.audio_inputs,
            spec.schema.audio_outputs,
            spec.schema.parameters
        );
        return spec;
    }

    void warmup_juce_vst_scan_cache()
    {
        (void)vst_search_cache();
    }

    UniqueResource JuceVstRuntimeManager::create_instance(
        JuceVstWrapperSpec const& spec,
        double sample_rate
    )
    {
        std::lock_guard lock(_mutex);
        auto instance = create_plugin_instance(
            _impl->format_manager,
            spec.plugin,
            sample_rate > 0.0 ? sample_rate : 48000.0,
            512
        );
        configure_audio_layout(*instance, spec.plugin);

        auto live = std::make_unique<LiveInstance>();
        live->spec = std::make_shared<JuceVstWrapperSpec const>(spec);
        live->plugin = std::move(instance);
        live->parameter_bindings.reserve(live->plugin->getParameters().size());
        for (auto* parameter : live->plugin->getParameters()) {
            live->parameter_bindings.push_back(ParameterBinding{ parameter, std::numeric_limits<float>::quiet_NaN() });
        }
        return UniqueResource(
            live.release(),
            +[](void* ptr) {
                delete static_cast<LiveInstance*>(ptr);
            }
        );
    }

    UniqueResource JuceVstRuntimeManager::create_midi_input(
        JuceMidiInputSpec const& spec,
        double sample_rate
    )
    {
        auto live = std::make_unique<LiveMidiInput>();
        live->sample_rate = sample_rate > 0.0 ? sample_rate : 48000.0;

        auto const device = resolve_midi_input_device(spec.device_query);
        live->input = ::juce::MidiInput::openDevice(device.identifier, live.get());
        if (!live->input) {
            throw std::runtime_error(
                "JUCE failed to open MIDI input device '" + device.name.toStdString() +
                "' (" + device.identifier.toStdString() + ")"
            );
        }
        live->input->start();

        return UniqueResource(
            live.release(),
            +[](void* ptr) {
                if (ptr == nullptr) {
                    return;
                }
                auto* live_input = static_cast<LiveMidiInput*>(ptr);
                if (live_input->input) {
                    live_input->input->stop();
                }
                delete live_input;
            }
        );
    }

    void tick_juce_vst_wrapper(
        JuceVstWrapperSpec const& spec,
        void* live_instance_ptr,
        TickBlockContext<JuceVstWrapper> const& state
    )
    {
        if (live_instance_ptr == nullptr) {
            throw std::logic_error("JuceVstWrapper was ticked without a bound runtime instance");
        }

        auto& live_instance = *static_cast<JuceVstRuntimeManager::LiveInstance*>(live_instance_ptr);
        live_instance.prepare(
            live_instance.prepared_sample_rate > 0.0 ? live_instance.prepared_sample_rate : 48000.0,
            state.block_size
        );

        auto& buffer = live_instance.audio_buffer;

        for (size_t channel = 0; channel < spec.schema.audio_inputs; ++channel) {
            auto const block = state.inputs[channel].get_block(state.block_size);
            float* destination = buffer.getWritePointer(static_cast<int>(channel));
            for (size_t sample = 0; sample < state.block_size; ++sample) {
                destination[sample] = block[sample];
            }
        }

        size_t const parameter_offset = spec.schema.audio_inputs;
        for (size_t index = 0; index < live_instance.parameter_bindings.size(); ++index) {
            float value = state.inputs[parameter_offset + index].get();
            value = std::clamp(value, 0.0f, 1.0f);
            auto& binding = live_instance.parameter_bindings[index];
            if (binding.last_value != value) {
                binding.parameter->setValue(value);
                binding.last_value = value;
            }
        }

        live_instance.midi_buffer.clear();
        state.event_inputs[0].for_each_in_block(state.index, state.block_size, [&](TimedEvent const& event, size_t) {
            auto const* midi = std::get_if<MidiEvent>(&event.value);
            if (midi == nullptr || midi->size == 0) {
                return;
            }
            live_instance.midi_buffer.addEvent(
                midi->bytes.data(),
                midi->size,
                static_cast<int>(event.time - state.index)
            );
        });
        live_instance.plugin->processBlock(buffer, live_instance.midi_buffer);

        for (size_t channel = 0; channel < spec.schema.audio_outputs; ++channel) {
            Sample::storage const* source_storage = buffer.getReadPointer(static_cast<int>(channel));
            Sample const* source = reinterpret_cast<Sample const*>(source_storage);
            state.outputs[channel].push_block(std::span<Sample const>(source, state.block_size));
        }
    }

    void JuceVstWrapper::tick_block(TickBlockContext<JuceVstWrapper> const& ctx) const
    {
        tick_juce_vst_wrapper(*_spec, ctx.state().plugin_instance.get(), ctx);
    }

    void tick_juce_midi_input_source(
        JuceMidiInputSpec const&,
        void* live_instance_ptr,
        TickBlockContext<JuceMidiInputSource> const& ctx
    )
    {
        if (live_instance_ptr == nullptr) {
            throw std::logic_error("JuceMidiInputSource was ticked without a bound runtime input");
        }

        auto& live_input = *static_cast<JuceVstRuntimeManager::LiveMidiInput*>(live_instance_ptr);
        int64_t const block_start = static_cast<int64_t>(ctx.index);
        int64_t const block_end = static_cast<int64_t>(ctx.index + ctx.block_size);

        std::lock_guard lock(live_input.mutex);
        if (!live_input.render_epoch_time.has_value()) {
            live_input.render_epoch_time = std::chrono::steady_clock::now();
            live_input.render_epoch_sample = block_start;
        }

        auto const to_sample_time = [&](std::chrono::steady_clock::time_point arrival_time) {
            auto const elapsed = arrival_time - *live_input.render_epoch_time;
            double const seconds = std::chrono::duration<double>(elapsed).count();
            double const samples = seconds * live_input.sample_rate;
            return live_input.render_epoch_sample + static_cast<int64_t>(std::floor(samples));
        };

        while (!live_input.pending_messages.empty()) {
            auto const& pending = live_input.pending_messages.front();
            int64_t const sample_time = to_sample_time(pending.arrival_time);
            if (sample_time >= block_end) {
                break;
            }

            size_t const sample_offset = sample_time <= block_start
                ? 0
                : static_cast<size_t>(sample_time - block_start);
            ctx.event_outputs[0].push(pending.midi, sample_offset, ctx.index, ctx.block_size);
            live_input.pending_messages.pop_front();
        }
    }

    void JuceMidiInputSource::tick_block(TickBlockContext<JuceMidiInputSource> const& ctx) const
    {
        tick_juce_midi_input_source(*_spec, ctx.state().live_input.get(), ctx);
    }
}

#endif
