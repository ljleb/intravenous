#pragma once

#include "../graph_node.h"
#include "../juce_vst_wrapper.h"

#include <mutex>
#include <memory>
#include <string>
#include <unordered_map>

namespace iv {
#if IV_ENABLE_JUCE_VST
    class JuceVstRuntimeManager;

    class JuceVstRuntimeSupport {
        JuceVstRuntimeManager* _manager = nullptr;
        double _sample_rate = 0.0;
        std::shared_ptr<void> _session;

    public:
        JuceVstRuntimeSupport() = default;
        JuceVstRuntimeSupport(JuceVstRuntimeManager& manager, double sample_rate);

        explicit operator bool() const
        {
            return _manager != nullptr;
        }

        void register_runtime_buffers(TypeErasedAllocator allocator, InitBufferContext& context);
    };

    class JuceVstRuntimeManager {
    public:
        struct Impl;
        JuceVstRuntimeManager();
        ~JuceVstRuntimeManager();
        JuceVstRuntimeManager(JuceVstRuntimeManager const&) = delete;
        JuceVstRuntimeManager& operator=(JuceVstRuntimeManager const&) = delete;

        struct LiveInstance;
        struct Session;

        std::shared_ptr<Session> make_session(double sample_rate);

    private:
        friend class JuceVstRuntimeSupport;
        friend void tick_juce_vst_wrapper(
            JuceVstWrapperSpec const& spec,
            void* live_instance,
            BlockTickState const& state
        );

        std::unique_ptr<Impl> _impl;
        std::mutex _mutex;
        std::unordered_map<std::string, std::unique_ptr<LiveInstance>> _instances;

        LiveInstance* acquire_instance(
            Session& session,
            std::shared_ptr<JuceVstWrapperSpec const> const& spec,
            size_t block_size
        );
    };

    void tick_juce_vst_wrapper(
        JuceVstWrapperSpec const& spec,
        void* live_instance,
        BlockTickState const& state
    );
#endif
}
