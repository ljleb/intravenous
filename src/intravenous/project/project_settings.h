#pragma once

#include <intravenous/devices/audio_device.h>
#include <intravenous/linker_event.h>

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

namespace iv {
enum class ProjectSettingField {
    c_compiler, cxx_compiler, cmake_program, cmake_generator, make_program,
    juce_dir, iv_package_pch, output_device_id, input_device_id,
};

struct ProjectAudioDeviceSelection {
    std::optional<std::string> output_device_id{};
    std::optional<std::string> input_device_id{};
    bool operator==(ProjectAudioDeviceSelection const&) const = default;
};

// Only the fields written by ProjectPersistenceBuilder. Runtime diagnostics
// flags stay with PackageJit and are not replaced by a project-history record.
struct ProjectToolchainSettings {
    std::optional<std::filesystem::path> c_compiler{};
    std::optional<std::filesystem::path> cxx_compiler{};
    std::optional<std::filesystem::path> cmake_program{};
    std::optional<std::string> cmake_generator{};
    std::optional<std::filesystem::path> make_program{};
    std::optional<std::filesystem::path> juce_dir{};
    std::optional<std::filesystem::path> iv_package_pch{};
    bool operator==(ProjectToolchainSettings const&) const = default;
};

class SystemAudioDevices;
class PackageJit;

// Read and commit within one owner event visit. The selection lock prevents
// another control-side writer changing the record between validation and commit.
class AudioDeviceSelectionTransaction {
    SystemAudioDevices* owner_ = nullptr;
    EventModuleVisit visit_{};
    std::unique_lock<std::mutex> lock_{};
    bool committed_ = false;
public:
    AudioDeviceSelectionTransaction() = default;
    AudioDeviceSelectionTransaction(SystemAudioDevices& owner, EventModuleVisit visit);
    [[nodiscard]] ProjectAudioDeviceSelection record() const;
    AudioDevicesSnapshot commit(ProjectAudioDeviceSelection selection);
};
struct AudioDeviceSelectionTransactionRequest {
    AudioDeviceSelectionTransaction transaction{};
    bool handled = false;
};

class ProjectToolchainTransaction {
    PackageJit* owner_ = nullptr;
    EventModuleVisit visit_{};
    std::unique_lock<std::mutex> lock_{};
    bool committed_ = false;
public:
    ProjectToolchainTransaction() = default;
    ProjectToolchainTransaction(PackageJit& owner, EventModuleVisit visit);
    [[nodiscard]] ProjectToolchainSettings record() const;
    void commit(ProjectToolchainSettings const& settings);
};
struct ProjectToolchainTransactionRequest {
    ProjectToolchainTransaction transaction{};
    bool handled = false;
};
} // namespace iv
