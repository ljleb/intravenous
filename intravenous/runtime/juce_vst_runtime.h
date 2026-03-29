#pragma once

#include "../graph_node.h"
#include "../juce_vst_wrapper.h"

#include <mutex>
#include <memory>

namespace iv {
#if IV_ENABLE_JUCE_VST
    class JuceVstRuntimeManager;

    class JuceVstRuntimeSupport {
        JuceVstRuntimeManager* _manager = nullptr;
        double _sample_rate = 0.0;
        ResourceContext _resources;
        ResourceContext::VstResources _vst_resources;

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

        void register_runtime_buffers(TypeErasedAllocator allocator, NodeLayoutBuilder& builder);
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
        friend void tick_juce_vst_wrapper(
            JuceVstWrapperSpec const& spec,
            void* live_instance,
            TickBlockContext<JuceVstWrapper> const& state
        );

        std::unique_ptr<Impl> _impl;
        std::mutex _mutex;
    };

    void tick_juce_vst_wrapper(
        JuceVstWrapperSpec const& spec,
        void* live_instance,
        TickBlockContext<JuceVstWrapper> const& state
    );
#endif
}
