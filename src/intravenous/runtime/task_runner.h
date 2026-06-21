#pragma once

#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace iv {
struct TaskCallback {
    void (*invoke)(void *) = nullptr;
    void *context = nullptr;
};

struct TaskRecord {
    std::string id {};
    std::vector<std::string> depends_on {};
    TaskCallback callback {};
};

struct TaskUpdateRecord {
    std::string id {};
    std::optional<std::vector<std::string>> depends_on {};
    std::optional<TaskCallback> callback {};
};

struct TaskGraphUpdate {
    std::vector<TaskRecord> to_create {};
    std::vector<std::string> to_delete {};
    std::vector<TaskUpdateRecord> to_update {};
};

struct VersionedTaskGraphUpdate {
    std::uint64_t version_index = 0;
    TaskGraphUpdate update {};
};

class TasksRunner {
public:
    struct DeclaredTask;
    struct ExecutionGroup;
    struct CompiledPlan;
    struct GraphVersion;
    struct PassState;

private:
    std::size_t worker_count_ = 1;
    mutable std::mutex state_mutex_;
    std::condition_variable state_cv_;
    std::mutex update_mutex_;
    std::shared_ptr<GraphVersion> active_graph_;
    std::shared_ptr<GraphVersion> pending_graph_;
    std::shared_ptr<PassState> current_pass_;
    std::optional<std::uint64_t> pending_update_version_index_;
    bool pending_graph_is_complete_ = true;
    std::uint64_t next_revision_ = 1;
    bool shutdown_requested_ = false;
    bool workers_should_exit_ = false;
    std::thread coordinator_thread_;
    std::vector<std::thread> worker_threads_;

    static std::shared_ptr<GraphVersion> build_graph_version(
        std::shared_ptr<GraphVersion const> const &base,
        TaskGraphUpdate const &update,
        std::uint64_t revision);
    void coordinator_loop();
    void worker_loop();

public:
    explicit TasksRunner(std::optional<std::size_t> worker_count = std::nullopt);
    ~TasksRunner();

    TasksRunner(TasksRunner const &) = delete;
    TasksRunner(TasksRunner &&) = delete;
    TasksRunner &operator=(TasksRunner const &) = delete;
    TasksRunner &operator=(TasksRunner &&) = delete;

    void update_tasks(TaskGraphUpdate const &update);
    void update_tasks(VersionedTaskGraphUpdate const &update);

    std::uint64_t active_graph_revision() const;
    std::optional<std::uint64_t> pending_graph_revision() const;
    std::vector<std::string> active_task_ids() const;
    std::size_t active_execution_group_count() const;
};
} // namespace iv
