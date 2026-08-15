#pragma once

#include <intravenous/graph/node.h>
#include <intravenous/juce/vst_wrapper.h>

#include <mutex>
#include <memory>

namespace iv {
#if IV_ENABLE_JUCE_VST
    class JuceVstRuntimeManager;

    class JuceVstRuntimeSupport {
        JuceVstRuntimeManager* _manager = nullptr;
        double _sample_rate = 0.0;
        ResourceContext::VstResources _vst_resources;
        ResourceContext _resources;

    public:
        JuceVstRuntimeSupport() = default;
        JuceVstRuntimeSupport(JuceVstRuntimeManager& manager, double sample_rate);

        explicit operator bool() const
        {
            return _manager != nullptr;
        }

        ResourceContext const& resources() const
        {
            return _resources;
        }
    };

    class JuceVstRuntimeManager {
    public:
        struct Impl;
        JuceVstRuntimeManager();
        ~JuceVstRuntimeManager();
        JuceVstRuntimeManager(JuceVstRuntimeManager const&) = delete;
        JuceVstRuntimeManager& operator=(JuceVstRuntimeManager const&) = delete;

        struct LiveInstance;
        UniqueResource create_instance(
            JuceVstWrapperSpec const& spec,
            double sample_rate
        );

    private:
        friend class JuceVstRuntimeSupport;
        std::unique_ptr<Impl> _impl;
        std::mutex _mutex;
    };

    // Builds the process-wide VST search cache once.
    // Subsequent probes reuse the cached index.
    void warmup_juce_vst_scan_cache();

#endif
}
