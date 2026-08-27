#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace iv {
    struct JuceVstWrapperSpec;
    struct RuntimeSampleInputBinding;
    struct RuntimeEventInputBinding;
    struct RuntimeOutputBinding;

    using UniqueResource = std::unique_ptr<void, void(*)(void*)>;

    inline constexpr std::uint32_t RUNTIME_BINDING_RESOURCE_ABI_VERSION = 1;

    struct RuntimeBindingResources {
        std::uint32_t abi_version = 0;
        std::uint32_t struct_size = 0;
        void* owner = nullptr;
        RuntimeSampleInputBinding const* (*resolve_sample_input)(
            void*, char const*, size_t) = nullptr;
        RuntimeEventInputBinding const* (*resolve_event_input)(
            void*, char const*, size_t) = nullptr;
        RuntimeOutputBinding const* (*resolve_output)(
            void*, char const*, size_t) = nullptr;

        void validate() const
        {
            if (!owner) return;
            if (abi_version != RUNTIME_BINDING_RESOURCE_ABI_VERSION ||
                struct_size < sizeof(RuntimeBindingResources)) {
                throw std::logic_error(
                    "incompatible runtime binding resource ABI");
            }
        }

        RuntimeSampleInputBinding const* sample_input(
            std::string_view semantic_key) const
        {
            validate();
            return owner && resolve_sample_input
                ? resolve_sample_input(
                    owner, semantic_key.data(), semantic_key.size())
                : nullptr;
        }

        RuntimeEventInputBinding const* event_input(
            std::string_view semantic_key) const
        {
            validate();
            return owner && resolve_event_input
                ? resolve_event_input(
                    owner, semantic_key.data(), semantic_key.size())
                : nullptr;
        }

        RuntimeOutputBinding const* output(
            std::string_view semantic_key) const
        {
            validate();
            return owner && resolve_output
                ? resolve_output(
                    owner, semantic_key.data(), semantic_key.size())
                : nullptr;
        }
    };

    struct ResourceContext {
        struct VstResources {
            void* owner = nullptr;
            UniqueResource (*create_juce_vst_fn)(void*, JuceVstWrapperSpec const&) = nullptr;

            UniqueResource create(JuceVstWrapperSpec const& descriptor) const
            {
                if (!create_juce_vst_fn) {
                    throw std::logic_error("VST resource callback is unavailable");
                }
                if (!owner) {
                    throw std::logic_error("VST resource owner is null");
                }
                return create_juce_vst_fn(owner, descriptor);
            }
        };

        VstResources vst {};
        RuntimeBindingResources runtime_bindings {};
    };
}
