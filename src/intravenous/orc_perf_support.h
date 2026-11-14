#pragma once

#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/Debugging/PerfSupportPlugin.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/SelfExecutorProcessControl.h>
#include <llvm/ExecutionEngine/Orc/TargetProcess/JITLoaderPerf.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/Error.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace iv::details {

// LLVM's target-process jitdump support keeps one process-wide state. Each
// PerfSupportPlugin starts that state when constructed, ends it when
// destroyed, and numbers its code records from zero. One plugin per LLJIT
// therefore reopens the dump and loses the records already written, stops the
// dump for every JIT when any plugin is destroyed, and repeats code indices,
// which perf inject uses to name the extracted code objects. Every IV LLJIT
// shares one process-lifetime plugin instead.
inline llvm::Expected<std::shared_ptr<llvm::orc::PerfSupportPlugin>>
process_orc_perf_plugin()
{
    struct ProcessPerfPlugin {
        std::shared_ptr<llvm::orc::PerfSupportPlugin> plugin{};
        std::string error{};
    };
    static ProcessPerfPlugin const* const process = [] {
        auto* result = new ProcessPerfPlugin{};
        auto control = llvm::orc::SelfExecutorProcessControl::Create();
        if (!control) {
            result->error = llvm::toString(control.takeError());
            return result;
        }
        // The plugin calls the start and end functions through this
        // session's in-process executor and reports record-collection errors
        // to it. Neither is ever destroyed, so the dump stays open until the
        // process exits.
        auto* session = new llvm::orc::ExecutionSession(std::move(*control));
        result->plugin = std::make_shared<llvm::orc::PerfSupportPlugin>(
            session->getExecutorProcessControl(),
            llvm::orc::ExecutorAddr::fromPtr(
                &llvm_orc_registerJITLoaderPerfStart),
            llvm::orc::ExecutorAddr::fromPtr(
                &llvm_orc_registerJITLoaderPerfEnd),
            llvm::orc::ExecutorAddr::fromPtr(
                &llvm_orc_registerJITLoaderPerfImpl),
            true,
            true);
        return result;
    }();
    if (!process->plugin) {
        return llvm::make_error<llvm::StringError>(
            process->error, llvm::inconvertibleErrorCode());
    }
    return process->plugin;
}

// LLVM's perf plugin writes Linux jitdump records for every object linked after
// the plugin is installed. Keep it opt-in: debug-line collection and jitdump I/O
// belong to a profiling run, not package loading or graph compilation normally.
inline llvm::Error enable_orc_perf_support_if_requested(llvm::orc::LLJIT& jit)
{
    auto const* requested = std::getenv("IV_ORC_PERF_JITDUMP");
    if (!requested || std::string_view(requested) != "1") {
        return llvm::Error::success();
    }

    auto* object_layer =
        llvm::dyn_cast<llvm::orc::ObjectLinkingLayer>(&jit.getObjLinkingLayer());
    if (!object_layer) {
        return llvm::make_error<llvm::StringError>(
            "IV_ORC_PERF_JITDUMP requires LLVM's JITLink ObjectLinkingLayer",
            llvm::inconvertibleErrorCode());
    }

    auto plugin = process_orc_perf_plugin();
    if (!plugin) return plugin.takeError();
    object_layer->addPlugin(std::move(*plugin));
    return llvm::Error::success();
}

} // namespace iv::details
