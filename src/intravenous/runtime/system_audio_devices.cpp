#include <intravenous/runtime/system_audio_devices.h>

#include <intravenous/runtime/project_persistence_builder.h>
#include <intravenous/runtime/graph_executor.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/socket_rpc_server.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <ranges>
#include <stdexcept>

namespace iv {
namespace {
thread_local void* current_input_binding = nullptr;
thread_local void* current_output_binding = nullptr;
thread_local std::span<Sample const> current_input_samples{};
thread_local std::span<Sample> current_output_samples{};

class FallbackAudioOutputDevice {
    RenderConfig config_{};
    std::vector<Sample> buffer_{};
    std::mutex mutex_{};
    std::condition_variable cv_{};
    std::chrono::steady_clock::time_point next_request_ =
        std::chrono::steady_clock::now();
    bool shutdown_ = false;

public:
    explicit FallbackAudioOutputDevice(RenderConfig config)
        : config_(std::move(config))
        , buffer_(config_.preferred_block_size * config_.num_channels)
    {
    }

    [[nodiscard]] RenderConfig const& config() const noexcept
    {
        return config_;
    }

    std::span<Sample> wait_for_block_request()
    {
        std::unique_lock lock(mutex_);
        cv_.wait_until(lock, next_request_, [&] { return shutdown_; });
        return shutdown_ ? std::span<Sample>{} : std::span{buffer_};
    }

    void submit_response()
    {
        auto const block_duration = std::chrono::duration<double>{
            static_cast<double>(config_.preferred_block_size)
            / static_cast<double>(config_.sample_rate)};
        std::scoped_lock lock(mutex_);
        next_request_ = std::max(
            next_request_ + std::chrono::duration_cast<
                std::chrono::steady_clock::duration>(block_duration),
            std::chrono::steady_clock::now());
    }

    void request_shutdown()
    {
        {
            std::scoped_lock lock(mutex_);
            shutdown_ = true;
        }
        cv_.notify_all();
    }
};

std::optional<AudioDeviceDescriptor> find_device_descriptor(
    std::vector<AudioDeviceDescriptor> const &devices,
    std::string const &device_id)
{
    auto const it = std::ranges::find(
        devices,
        device_id,
        &AudioDeviceDescriptor::device_id);
    if (it == devices.end()) {
        return std::nullopt;
    }
    return *it;
}
} // namespace

SystemAudioDevices::SystemAudioDevices(
    size_t sample_rate,
    size_t block_size,
    SystemAudioDevicesBackend backend,
    std::optional<std::string> selected_output_device_id,
    std::optional<std::string> selected_input_device_id)
    : sample_rate_(sample_rate)
    , block_size_(block_size)
    , backend_(std::move(backend))
    , output_render_config_(RenderConfig{
          .sample_rate = sample_rate,
          .num_channels = 2,
          .max_block_frames = std::max<size_t>(block_size * 4, 4096),
          .preferred_block_size = block_size,
      })
    , input_render_config_(RenderConfig{
          .sample_rate = sample_rate,
          .num_channels = 2,
          .max_block_frames = std::max<size_t>(block_size * 4, 4096),
          .preferred_block_size = block_size,
      })
    , selected_output_device_id_(std::move(selected_output_device_id))
    , selected_input_device_id_(std::move(selected_input_device_id))
    , input_render_buffer_(
          block_size * channel_count(ChannelLayout{
              .channel_type = ChannelTypeId::stereo,
              .sample_layout = SampleStreamLayout::interleaved,
          }))
{
    static_assert(std::atomic<GraphExecutor*>::is_always_lock_free);
    static_assert(std::atomic<ActiveInputDevice*>::is_always_lock_free);
    static_assert(std::atomic<LogicalInputBinding*>::is_always_lock_free);
    static_assert(std::atomic<LogicalOutputBinding*>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);
    if (sample_rate_ == 0 || block_size_ == 0) {
        throw std::invalid_argument(
            "system audio devices require non-zero sample rate and block size");
    }
    if (!backend_.list_output_devices || !backend_.list_input_devices
        || !backend_.make_output_device || !backend_.make_input_device) {
        throw std::invalid_argument("system audio devices backend is incomplete");
    }

    (void)set_selected_devices(selected_output_device_id_, selected_input_device_id_);
}

SystemAudioDevices::~SystemAudioDevices()
{
    stop();
}

std::vector<AudioDeviceDescriptor> SystemAudioDevices::list_output_devices_unlocked() const
{
    return backend_.list_output_devices();
}

std::vector<AudioDeviceDescriptor> SystemAudioDevices::list_input_devices_unlocked() const
{
    return backend_.list_input_devices();
}

std::shared_ptr<SystemAudioDevices::ActiveOutputDevice>
SystemAudioDevices::create_output_device(std::optional<std::string> const &device_id, std::optional<std::string>* error) const
{
    auto descriptor = device_id
        ? find_device_descriptor(list_output_devices_unlocked(), *device_id)
        : std::optional<AudioDeviceDescriptor>{};
    if (device_id && descriptor) {
        try {
            return std::make_shared<ActiveOutputDevice>(
                *descriptor,
                backend_.make_output_device(*device_id, output_render_config_),
                true);
        } catch (std::exception const& failure) {
            if (error) *error = failure.what();
        } catch (...) {
            if (error) *error = "unknown output device activation failure";
        }
    } else if (device_id && error) {
        *error = "selected output device is unavailable";
    }
    return std::make_shared<ActiveOutputDevice>(
        descriptor.value_or(AudioDeviceDescriptor{
            .device_id = device_id.value_or(std::string{}),
        }),
        AudioOutputDevice{
            std::in_place_type<FallbackAudioOutputDevice>,
            output_render_config_},
        false);
}

std::shared_ptr<SystemAudioDevices::ActiveInputDevice>
SystemAudioDevices::create_input_device(std::optional<std::string> const &device_id, std::optional<std::string>* error) const
{
    if (!device_id.has_value()) {
        return nullptr;
    }

    auto const descriptor = find_device_descriptor(list_input_devices_unlocked(), *device_id);
    if (!descriptor.has_value()) {
        if (error) *error = "selected input device is unavailable";
        return nullptr;
    }

    try {
        auto device = backend_.make_input_device(*device_id, input_render_config_);
        auto const target_latency_frames = std::max(
            block_size_ * 2,
            device.config().preferred_block_size * 2);
        auto synchronizer = std::make_unique<AudioInputSynchronizer>(
            sample_rate_,
            block_size_,
            device.config().sample_rate,
            target_latency_frames);
        return std::make_shared<ActiveInputDevice>(
            *descriptor,
            std::move(device),
            std::move(synchronizer));
    } catch (std::exception const& failure) {
        if (error) *error = failure.what();
        return nullptr;
    } catch (...) {
        if (error) *error = "unknown input device activation failure";
        return nullptr;
    }
}

void SystemAudioDevices::start_input_capture_thread(
    std::shared_ptr<ActiveInputDevice> input_device)
{
    if (!input_device) {
        return;
    }
    input_capture_thread_ = std::thread([this, input_device = std::move(input_device)] {
        input_capture_loop(input_device);
    });
}

void SystemAudioDevices::start_output_render_thread(
    std::shared_ptr<ActiveOutputDevice> output_device)
{
    if (!output_device) return;
    output_render_thread_ = std::thread(
        [this, output_device = std::move(output_device)] {
            output_render_loop(output_device);
        });
}

void SystemAudioDevices::input_capture_loop(std::shared_ptr<ActiveInputDevice> input_device)
{
    while (!shutdown_requested_.load()) {
        try {
            auto block = input_device->device.wait_for_captured_block();
            if (!block.samples.empty()) {
                input_device->synchronizer->push_captured_block(block);
            }
            input_device->device.release_captured_block();
        } catch (std::exception const &) {
            input_device->failed.store(true, std::memory_order_release);
            return;
        }
    }
}

void SystemAudioDevices::output_render_loop(
    std::shared_ptr<ActiveOutputDevice> output_device) noexcept
{
    try {
        for (;;) {
            auto output = output_device->device.wait_for_block_request();
            if (shutdown_requested_.load(std::memory_order_acquire)
                || output_device->stop_requested.load(
                    std::memory_order_acquire)) {
                return;
            }
            auto const channels = output_device->device.config().num_channels;
            if (channels != 2 || output.size() % channels != 0) {
                throw std::runtime_error(
                    "system audio output returned a non-stereo partial frame");
            }
            for (auto& sample : output) sample = Sample{};
            auto const frames = output.size() / channels;
            for (std::size_t offset = 0; offset < frames;) {
                auto const count = std::min(block_size_, frames - offset);
                auto const sample_offset = offset * channels;
                auto const sample_count = count * channels;
                auto output_slice = output.subspan(sample_offset, sample_count);
                auto input_slice = std::span{input_render_buffer_}.first(sample_count);
                for (auto& sample : input_slice) sample = Sample{};

                ActiveInputDevice* input_device = nullptr;
                do {
                    input_device = input_render_device_.load();
                    input_render_hazard_.store(input_device);
                } while (input_device != input_render_device_.load());
                auto* input_binding =
                    active_input_binding_.load(std::memory_order_acquire);
                try {
                    if (input_device && input_binding
                        && !input_device->failed.load(
                            std::memory_order_acquire)) {
                        input_device->synchronizer->render_timeline_block(input_slice);
                    }
                } catch (...) {
                    input_render_hazard_.store(nullptr);
                    throw;
                }
                input_render_hazard_.store(nullptr);

                current_input_binding = input_binding;
                current_output_binding =
                    active_output_binding_.load(std::memory_order_acquire);
                current_input_samples = input_slice;
                current_output_samples = output_slice;

                auto* executor = graph_executor_.load(std::memory_order_acquire);
                if (executor) {
                    (void)executor->activate_pending(realtime_cursor_);
                    if (executor->active_generation()) {
                        executor->tick_block(realtime_cursor_, count);
                    }
                }

                current_input_binding = nullptr;
                current_output_binding = nullptr;
                current_input_samples = {};
                current_output_samples = {};
                realtime_cursor_ = realtime_cursor_.advanced_by(count);
                offset += count;
            }
            output_device->device.submit_response();
        }
    } catch (...) {
        current_input_binding = nullptr;
        current_output_binding = nullptr;
        current_input_samples = {};
        current_output_samples = {};
        if (!shutdown_requested_.load(std::memory_order_acquire)
            && !output_device->stop_requested.load(std::memory_order_acquire)) {
            output_render_failed_.store(true, std::memory_order_release);
        }
    }
}

void SystemAudioDevices::refresh_active_bindings_locked()
{
    LogicalInputBinding* input = nullptr;
    LogicalOutputBinding* output = nullptr;
    if (active_input_device_) {
        auto const found = input_bindings_.find(
            active_input_device_->descriptor.device_id);
        if (found != input_bindings_.end()) input = found->second.get();
    }
    if (active_output_device_ && selected_output_device_id_) {
        auto const found = output_bindings_.find(
            active_output_device_->descriptor.device_id);
        if (found != output_bindings_.end()) output = found->second.get();
    }
    active_input_binding_.store(input, std::memory_order_release);
    active_output_binding_.store(output, std::memory_order_release);
}

SystemAudioInputBinding SystemAudioDevices::resolve_input_binding(
    std::string_view device_id)
{
    std::scoped_lock lock(mutex_);
    auto [found, inserted] = input_bindings_.try_emplace(
        std::string{device_id}, nullptr);
    if (inserted) {
        found->second = std::make_unique<LogicalInputBinding>(LogicalInputBinding{
            .devices = this,
            .device_id = found->first,
        });
    }
    refresh_active_bindings_locked();
    return SystemAudioInputBinding{
        found->second.get(),
        +[](void* opaque) noexcept -> std::span<Sample const> {
            auto* binding = static_cast<LogicalInputBinding*>(opaque);
            if (current_input_binding != binding) {
                return {};
            }
            return current_input_samples;
        }};
}

SystemAudioOutputBinding SystemAudioDevices::resolve_output_binding(
    std::string_view device_id)
{
    std::scoped_lock lock(mutex_);
    auto [found, inserted] = output_bindings_.try_emplace(
        std::string{device_id}, nullptr);
    if (inserted) {
        found->second = std::make_unique<LogicalOutputBinding>(LogicalOutputBinding{
            .devices = this,
            .device_id = found->first,
        });
    }
    refresh_active_bindings_locked();
    return SystemAudioOutputBinding{
        found->second.get(),
        +[](void* opaque) noexcept -> std::span<Sample> {
            auto* binding = static_cast<LogicalOutputBinding*>(opaque);
            if (current_output_binding != binding) return {};
            return current_output_samples;
        }};
}

void SystemAudioDevices::request_shutdown()
{
    if (shutdown_requested_.exchange(true)) {
        return;
    }

    std::shared_ptr<ActiveOutputDevice> output_device;
    std::shared_ptr<ActiveInputDevice> input_device;
    {
        std::scoped_lock lock(mutex_);
        output_device = std::move(active_output_device_);
        input_device = std::move(active_input_device_);
        input_render_device_.store(nullptr);
        refresh_active_bindings_locked();
    }
    wait_until_input_device_is_unused(input_device.get());
    if (output_device) {
        output_device->stop_requested.store(true, std::memory_order_release);
        output_device->device.request_shutdown();
    }
    if (input_device) {
        input_device->device.request_shutdown();
    }
}

void SystemAudioDevices::stop()
{
    request_shutdown();
    if (output_render_thread_.joinable()) {
        output_render_thread_.join();
    }
    if (input_capture_thread_.joinable()) {
        input_capture_thread_.join();
    }
}

AudioDeviceSelectionState SystemAudioDevices::selection_state_for(
    std::optional<std::string> const &selected_id,
    std::shared_ptr<ActiveOutputDevice> const &active_device,
    std::vector<AudioDeviceDescriptor> const &available_devices) const
{
    AudioDeviceSelectionState selection;
    selection.device_id = selected_id;
    selection.available = active_device && active_device->hardware_available
        && selected_id
        && active_device->descriptor.device_id == *selected_id;
    if (selection.available) {
        selection.name = active_device->descriptor.name;
    } else if (selected_id.has_value()) {
        if (auto descriptor = find_device_descriptor(available_devices, *selected_id)) {
            selection.name = descriptor->name;
        }
    }
    return selection;
}

AudioDeviceSelectionState SystemAudioDevices::selection_state_for(
    std::optional<std::string> const &selected_id,
    std::shared_ptr<ActiveInputDevice> const &active_device,
    std::vector<AudioDeviceDescriptor> const &available_devices) const
{
    AudioDeviceSelectionState selection;
    selection.device_id = selected_id;
    selection.available = active_device
        && selected_id
        && active_device->descriptor.device_id == *selected_id
        && !active_device->failed.load(std::memory_order_acquire);
    if (selection.available) {
        selection.name = active_device->descriptor.name;
    } else if (selected_id.has_value()) {
        if (auto descriptor = find_device_descriptor(available_devices, *selected_id)) {
            selection.name = descriptor->name;
        }
    }
    return selection;
}

ProjectAudioDeviceSelection SystemAudioDevices::selected_devices() const
{
    std::scoped_lock lock(mutex_);
    return {selected_output_device_id_, selected_input_device_id_};
}

AudioDevicesSnapshot SystemAudioDevices::audio_devices_snapshot() const
{
    std::shared_ptr<ActiveOutputDevice> active_output;
    std::shared_ptr<ActiveInputDevice> active_input;
    ProjectAudioDeviceSelection selection;
    std::optional<std::string> output_error;
    std::optional<std::string> input_error;
    {
        std::scoped_lock lock(mutex_);
        active_output = active_output_device_;
        active_input = active_input_device_;
        selection = {selected_output_device_id_, selected_input_device_id_};
        output_error = output_activation_error_;
        input_error = input_activation_error_;
    }
    AudioDevicesSnapshot snapshot;
    // Status reads and saves must remain usable if backend enumeration fails.
    try { snapshot.output_devices = list_output_devices_unlocked(); }
    catch (std::exception const& error) { if (!output_error) output_error = error.what(); }
    catch (...) { if (!output_error) output_error = "cannot enumerate output devices"; }
    try { snapshot.input_devices = list_input_devices_unlocked(); }
    catch (std::exception const& error) { if (!input_error) input_error = error.what(); }
    catch (...) { if (!input_error) input_error = "cannot enumerate input devices"; }
    snapshot.selected_output = selection_state_for(selection.output_device_id, active_output, snapshot.output_devices);
    snapshot.selected_input = selection_state_for(selection.input_device_id, active_input, snapshot.input_devices);
    if (output_render_failed_.load(std::memory_order_acquire)) {
        snapshot.selected_output.available = false;
        if (!output_error) output_error = "output rendering failed";
    }
    if (active_input && active_input->failed.load(std::memory_order_acquire) && !input_error)
        input_error = "input capture failed";
    snapshot.selected_output.error = std::move(output_error);
    snapshot.selected_input.error = std::move(input_error);
    return snapshot;
}

void SystemAudioDevices::replace_output_device(
    std::shared_ptr<ActiveOutputDevice> output_device)
{
    std::shared_ptr<ActiveOutputDevice> old_output;
    std::shared_ptr<ActiveOutputDevice> rejected_output;
    std::exception_ptr start_failure;
    std::thread old_thread;
    {
        std::scoped_lock lock(mutex_);
        if (shutdown_requested_.load(std::memory_order_acquire)) {
            rejected_output = std::move(output_device);
        } else {
            old_output = std::move(active_output_device_);
            if (output_render_thread_.joinable()) {
                old_thread = std::move(output_render_thread_);
            }
            refresh_active_bindings_locked();
        }
    }
    if (rejected_output) {
        rejected_output->stop_requested.store(true, std::memory_order_release);
        rejected_output->device.request_shutdown();
        return;
    }
    if (old_output) {
        old_output->stop_requested.store(true, std::memory_order_release);
        old_output->device.request_shutdown();
    }
    if (old_thread.joinable()) old_thread.join();

    {
        std::scoped_lock lock(mutex_);
        if (shutdown_requested_.load(std::memory_order_acquire)) {
            rejected_output = std::move(output_device);
        } else {
            active_output_device_ = output_device;
            refresh_active_bindings_locked();
            output_render_failed_.store(false, std::memory_order_release);
            try {
                start_output_render_thread(output_device);
            } catch (...) {
                start_failure = std::current_exception();
                rejected_output = std::move(active_output_device_);
                refresh_active_bindings_locked();
            }
        }
    }
    if (rejected_output) {
        rejected_output->stop_requested.store(true, std::memory_order_release);
        rejected_output->device.request_shutdown();
    }
    if (start_failure) std::rethrow_exception(start_failure);
}

void SystemAudioDevices::replace_input_device(
    std::shared_ptr<ActiveInputDevice> input_device)
{
    std::shared_ptr<ActiveInputDevice> old_input;
    std::shared_ptr<ActiveInputDevice> rejected_input;
    std::exception_ptr start_failure;
    std::thread old_thread;
    {
        std::scoped_lock lock(mutex_);
        if (shutdown_requested_.load(std::memory_order_acquire)) {
            rejected_input = std::move(input_device);
        } else {
            old_input = std::move(active_input_device_);
            input_render_device_.store(nullptr);
            if (input_capture_thread_.joinable()) {
                old_thread = std::move(input_capture_thread_);
            }
            refresh_active_bindings_locked();
        }
    }
    if (rejected_input) {
        rejected_input->device.request_shutdown();
        return;
    }

    if (old_input) {
        old_input->device.request_shutdown();
    }
    wait_until_input_device_is_unused(old_input.get());
    if (old_thread.joinable()) {
        old_thread.join();
    }

    {
        std::scoped_lock lock(mutex_);
        if (shutdown_requested_.load(std::memory_order_acquire)) {
            rejected_input = std::move(input_device);
        } else {
            active_input_device_ = input_device;
            input_render_device_.store(input_device.get());
            refresh_active_bindings_locked();
            try {
                start_input_capture_thread(input_device);
            } catch (...) {
                start_failure = std::current_exception();
                input_render_device_.store(nullptr);
                rejected_input = std::move(active_input_device_);
                refresh_active_bindings_locked();
            }
        }
    }
    if (rejected_input) {
        rejected_input->device.request_shutdown();
        wait_until_input_device_is_unused(rejected_input.get());
    }
    if (start_failure) std::rethrow_exception(start_failure);
}

void SystemAudioDevices::wait_until_input_device_is_unused(
    ActiveInputDevice const* device) const noexcept
{
    if (!device) return;
    static_assert(std::atomic<ActiveInputDevice*>::is_always_lock_free);
    while (input_render_hazard_.load() == device) {
        std::this_thread::yield();
    }
}

void SystemAudioDevices::attach_graph_executor(GraphExecutor& executor) noexcept
{
    graph_executor_.store(&executor, std::memory_order_release);
}

void SystemAudioDevices::detach_graph_executor(GraphExecutor& executor) noexcept
{
    auto* expected = &executor;
    (void)graph_executor_.compare_exchange_strong(
        expected, nullptr, std::memory_order_acq_rel);
}

bool SystemAudioDevices::output_render_failed() const noexcept
{
    return output_render_failed_.load(std::memory_order_acquire);
}

ResourceContext SystemAudioDevices::resources() noexcept
{
    ResourceContext result;
    result.system_audio = {
        .owner = this,
        .resolve_input_fn = +[](void* owner, std::string_view device_id) {
            return static_cast<SystemAudioDevices*>(owner)
                ->resolve_input_binding(device_id);
        },
        .resolve_output_fn = +[](void* owner, std::string_view device_id) {
            return static_cast<SystemAudioDevices*>(owner)
                ->resolve_output_binding(device_id);
        },
    };
    return result;
}

AudioDeviceSelectionTransaction::AudioDeviceSelectionTransaction(SystemAudioDevices& owner, EventModuleVisit visit)
    : owner_(&owner), visit_(visit), lock_(owner.selection_mutex_)
{}

ProjectAudioDeviceSelection AudioDeviceSelectionTransaction::record() const
{
    if (!owner_ || !lock_.owns_lock()) throw std::logic_error("audio selection transaction is not open");
    EventModuleVisitScope entry(visit_);
    return owner_->selected_devices();
}

AudioDevicesSnapshot AudioDeviceSelectionTransaction::commit(ProjectAudioDeviceSelection selection)
{
    if (!owner_ || !lock_.owns_lock() || committed_) throw std::logic_error("audio selection transaction commits once");
    EventModuleVisitScope entry(visit_);
    committed_ = true;
    return owner_->apply_selected_devices_locked(std::move(selection));
}

void SystemAudioDevices::handle_project_graph_audio_selection(AudioDeviceSelectionTransactionRequest& request)
{
    if (request.handled) throw std::logic_error("audio selection transaction was handled twice");
    request.handled = true;
    request.transaction = AudioDeviceSelectionTransaction(*this, EventModuleVisit::current<SystemAudioDevices>());
}

AudioDevicesSnapshot SystemAudioDevices::set_selected_devices(
    std::optional<std::string> output_device_id, std::optional<std::string> input_device_id)
{
    std::scoped_lock selection_lock(selection_mutex_);
    return apply_selected_devices_locked({std::move(output_device_id), std::move(input_device_id)});
}

AudioDevicesSnapshot SystemAudioDevices::apply_selected_devices_locked(ProjectAudioDeviceSelection selection)
{
    auto const previous = selected_devices();
    bool const output_changed = previous.output_device_id != selection.output_device_id || !selection_initialized_;
    bool const input_changed = previous.input_device_id != selection.input_device_id || !selection_initialized_;
    {
        std::scoped_lock lock(mutex_);
        selected_output_device_id_ = selection.output_device_id;
        selected_input_device_id_ = selection.input_device_id;
        if (output_changed) output_activation_error_.reset();
        if (input_changed) input_activation_error_.reset();
    }
    selection_initialized_ = true;
    std::optional<std::string> output_error;
    std::optional<std::string> input_error;
    if (output_changed) {
        bool failed = false;
        try { replace_output_device(create_output_device(selection.output_device_id, &output_error)); }
        catch (std::exception const& error) { output_error = error.what(); failed = true; }
        catch (...) { output_error = "unknown output device activation failure"; failed = true; }
        if (failed) {
            // A failed hardware activation must not stop Tick's fallback clock.
            try { replace_output_device(create_output_device(std::nullopt)); }
            catch (std::exception const& error) { *output_error += std::string("; fallback failed: ") + error.what(); }
            catch (...) { *output_error += "; fallback failed"; }
        }
    }
    if (input_changed) {
        bool failed = false;
        try { replace_input_device(create_input_device(selection.input_device_id, &input_error)); }
        catch (std::exception const& error) { input_error = error.what(); failed = true; }
        catch (...) { input_error = "unknown input device activation failure"; failed = true; }
        if (failed) {
            try { replace_input_device(nullptr); }
            catch (...) { *input_error += "; input shutdown failed"; }
        }
    }
    {
        std::scoped_lock lock(mutex_);
        if (output_changed) output_activation_error_ = std::move(output_error);
        if (input_changed) input_activation_error_ = std::move(input_error);
    }
    return audio_devices_snapshot();
}

void SystemAudioDevices::handle_socket_rpc_get_audio_devices(
    GetAudioDevicesRequest const &,
    SocketRpcAudioDevicesResultBuilder &builder) const
{
    builder.succeed(audio_devices_snapshot());
}

void SystemAudioDevices::handle_project_override_settings(ProjectOverrideSettingsRequest const& request)
{
    bool const output = request.output_device_id.has_value() || request.specifies(ProjectSettingField::output_device_id);
    bool const input = request.input_device_id.has_value() || request.specifies(ProjectSettingField::input_device_id);
    if (!output && !input) return;
    auto const current = selected_devices();
    (void)set_selected_devices(output ? request.output_device_id : current.output_device_id,
        input ? request.input_device_id : current.input_device_id);
    // Replay restores accepted intent; it does not append a user-history entry.
}

void SystemAudioDevices::handle_project_persistence_collect_state(
    ProjectPersistenceBuilder &builder) const
{
    auto const selection = selected_devices();
    builder.add_project_audio_device_selection(selection.output_device_id, selection.input_device_id);
}

} // namespace iv
