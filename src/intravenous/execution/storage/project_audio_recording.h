#pragma once

#include <intravenous/node/resources.h>

#include <filesystem>
#include <memory>

namespace iv {

// Owns project file handles and a single disk worker. Its bindings outlive the
// executor generations that reference them.
class ProjectAudioRecording {
    class Impl;
    std::unique_ptr<Impl> impl_;

public:
    explicit ProjectAudioRecording(std::filesystem::path project_root);
    ~ProjectAudioRecording();

    [[nodiscard]] ResourceContext::SampleRecordingResources resources() noexcept;
};

} // namespace iv
