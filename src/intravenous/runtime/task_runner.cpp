#include <intravenous/runtime/task_runner.h>
#include <intravenous/runtime/task_runner_events.h>

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <queue>
#include <ranges>
#include <stdexcept>
#include <unordered_set>

namespace iv {
namespace {
    template<typename Range>
    bool has_duplicate_strings(Range const &range)
    {
        std::unordered_set<std::string> seen;
        for (auto const &value : range) {
            if (!seen.insert(value).second) {
                return true;
            }
        }
        return false;
    }
}

struct TasksRunner::DeclaredTask {
    std::string id {};
    std::vector<std::string> depends_on {};
    TaskCallback callback {};
};

struct TasksRunner::ExecutionGroup {
    std::vector<std::string> task_ids {};
    std::vector<TaskCallback> callbacks {};
    std::vector<std::size_t> depends_on {};
    std::vector<std::size_t> users {};
    std::size_t order = 0;
};

struct TasksRunner::CompiledPlan {
    std::vector<ExecutionGroup> groups {};
};

struct TasksRunner::GraphVersion {
    std::uint64_t revision = 0;
    bool is_complete = true;
    std::unordered_map<std::string, DeclaredTask> tasks {};
    std::vector<std::string> sorted_task_ids {};
    CompiledPlan compiled_plan {};
};

struct TasksRunner::PassState {
    struct ReadyItem {
        std::size_t user_count = 0;
        std::size_t order = 0;
        std::size_t group_index = 0;
    };

    struct ReadyItemCompare {
        bool operator()(ReadyItem const &lhs, ReadyItem const &rhs) const
        {
            if (lhs.user_count != rhs.user_count) {
                return lhs.user_count < rhs.user_count;
            }
            return lhs.order > rhs.order;
        }
    };

    std::shared_ptr<GraphVersion const> graph {};
    std::vector<std::size_t> remaining_dependencies {};
    std::priority_queue<ReadyItem, std::vector<ReadyItem>, ReadyItemCompare> ready {};
    std::size_t running = 0;
    std::size_t completed = 0;

    explicit PassState(std::shared_ptr<GraphVersion const> graph_)
      : graph(std::move(graph_))
    {
        auto const &groups = graph->compiled_plan.groups;
        remaining_dependencies.reserve(groups.size());
        for (std::size_t index = 0; index < groups.size(); ++index) {
            auto const &group = groups[index];
            remaining_dependencies.push_back(group.depends_on.size());
            if (group.depends_on.empty()) {
                ready.push(ReadyItem{
                    .user_count = group.users.size(),
                    .order = group.order,
                    .group_index = index,
                });
            }
        }
    }
};

namespace {
    void validate_callback(TaskCallback const &callback, std::string const &task_id)
    {
        if (!callback.invoke) {
            throw std::runtime_error("task '" + task_id + "' is missing a callback");
        }
    }

    std::unordered_map<std::string, TasksRunner::DeclaredTask> apply_update(
        std::shared_ptr<TasksRunner::GraphVersion const> const &base,
        TaskGraphUpdate const &update)
    {
        std::unordered_set<std::string> touched;
        auto require_unique_touch = [&](std::string const &id) {
            if (!touched.insert(id).second) {
                throw std::runtime_error("task id '" + id + "' appears more than once in one update");
            }
        };

        for (auto const &task : update.to_create) {
            require_unique_touch(task.id);
        }
        for (auto const &task_id : update.to_delete) {
            require_unique_touch(task_id);
        }
        for (auto const &task : update.to_update) {
            require_unique_touch(task.id);
        }

        auto tasks = base->tasks;

        for (auto const &task_id : update.to_delete) {
            if (!tasks.contains(task_id)) {
                throw std::runtime_error("cannot delete missing task '" + task_id + "'");
            }
            tasks.erase(task_id);
        }

        if (!update.to_delete.empty()) {
            std::unordered_set<std::string> deleted_ids{
                update.to_delete.begin(),
                update.to_delete.end()
            };
            for (auto &[_, task] : tasks) {
                task.depends_on.erase(
                    std::remove_if(
                        task.depends_on.begin(),
                        task.depends_on.end(),
                        [&](std::string const &dependency) {
                            return deleted_ids.contains(dependency);
                        }),
                    task.depends_on.end());
            }
        }

        for (auto const &task : update.to_create) {
            if (tasks.contains(task.id)) {
                throw std::runtime_error("cannot create existing task '" + task.id + "'");
            }
            validate_callback(task.callback, task.id);
            if (has_duplicate_strings(task.depends_on)) {
                throw std::runtime_error("task '" + task.id + "' declares duplicate dependencies");
            }
            tasks.emplace(task.id, TasksRunner::DeclaredTask{
                .id = task.id,
                .depends_on = task.depends_on,
                .callback = task.callback,
            });
        }

        for (auto const &task_update : update.to_update) {
            auto it = tasks.find(task_update.id);
            if (it == tasks.end()) {
                throw std::runtime_error("cannot update missing task '" + task_update.id + "'");
            }
            if (task_update.depends_on) {
                if (has_duplicate_strings(*task_update.depends_on)) {
                    throw std::runtime_error("task '" + task_update.id + "' declares duplicate dependencies");
                }
                it->second.depends_on = *task_update.depends_on;
            }
            if (task_update.callback) {
                validate_callback(*task_update.callback, task_update.id);
                it->second.callback = *task_update.callback;
            }
        }

        return tasks;
    }

    std::vector<std::string> sorted_task_ids(
        std::unordered_map<std::string, TasksRunner::DeclaredTask> const &tasks)
    {
        std::vector<std::string> ids;
        ids.reserve(tasks.size());
        for (auto const &[task_id, _] : tasks) {
            ids.push_back(task_id);
        }
        std::ranges::sort(ids);
        return ids;
    }

    struct DeclaredGraphAnalysis {
        bool is_complete = true;
        std::unordered_map<std::string, std::vector<std::string>> users_by_task {};
        std::vector<std::string> topological_order {};
    };

    DeclaredGraphAnalysis analyze_declared_graph(
        std::unordered_map<std::string, TasksRunner::DeclaredTask> const &tasks,
        std::vector<std::string> const &sorted_ids)
    {
        DeclaredGraphAnalysis analysis;
        std::unordered_map<std::string, std::size_t> indegree;
        indegree.reserve(tasks.size());
        for (auto const &[task_id, _] : tasks) {
            indegree.emplace(task_id, 0);
            analysis.users_by_task.emplace(task_id, std::vector<std::string>{});
        }

        for (auto const &[task_id, task] : tasks) {
            for (auto const &dependency : task.depends_on) {
                if (!tasks.contains(dependency)) {
                    analysis.is_complete = false;
                    continue;
                }
                analysis.users_by_task.at(dependency).push_back(task_id);
                indegree.at(task_id) += 1;
            }
        }

        for (auto &[_, users] : analysis.users_by_task) {
            std::ranges::sort(users);
        }

        std::priority_queue<
            std::string,
            std::vector<std::string>,
            std::greater<>
        > ready;
        for (auto const &task_id : sorted_ids) {
            if (indegree.at(task_id) == 0) {
                ready.push(task_id);
            }
        }

        while (!ready.empty()) {
            auto task_id = ready.top();
            ready.pop();
            analysis.topological_order.push_back(task_id);

            for (auto const &user : analysis.users_by_task.at(task_id)) {
                auto &remaining = indegree.at(user);
                remaining -= 1;
                if (remaining == 0) {
                    ready.push(user);
                }
            }
        }

        if (analysis.topological_order.size() != tasks.size()) {
            throw std::runtime_error("task graph contains a cycle");
        }

        return analysis;
    }

    TasksRunner::CompiledPlan compile_execution_plan(
        std::unordered_map<std::string, TasksRunner::DeclaredTask> const &tasks,
        std::vector<std::string> const &sorted_ids)
    {
        auto const analysis = analyze_declared_graph(tasks, sorted_ids);

        std::unordered_set<std::string> assigned;
        std::vector<std::vector<std::string>> chains;
        chains.reserve(tasks.size());

        auto can_merge_successor = [&](std::string const &task_id, std::string const &successor) {
            auto const &users = analysis.users_by_task.at(task_id);
            if (users.size() != 1 || users.front() != successor) {
                return false;
            }
            auto const &successor_task = tasks.at(successor);
            return successor_task.depends_on.size() == 1 && successor_task.depends_on.front() == task_id;
        };

        for (auto const &task_id : analysis.topological_order) {
            if (assigned.contains(task_id)) {
                continue;
            }

            std::vector<std::string> chain;
            chain.push_back(task_id);
            assigned.insert(task_id);

            auto current = task_id;
            while (true) {
                auto const &users = analysis.users_by_task.at(current);
                if (users.size() != 1) {
                    break;
                }
                auto const &successor = users.front();
                if (assigned.contains(successor) || !can_merge_successor(current, successor)) {
                    break;
                }
                chain.push_back(successor);
                assigned.insert(successor);
                current = successor;
            }

            chains.push_back(std::move(chain));
        }

        std::unordered_map<std::string, std::size_t> group_index_by_task;
        group_index_by_task.reserve(tasks.size());
        for (std::size_t group_index = 0; group_index < chains.size(); ++group_index) {
            for (auto const &task_id : chains[group_index]) {
                group_index_by_task.emplace(task_id, group_index);
            }
        }

        TasksRunner::CompiledPlan plan;
        plan.groups.reserve(chains.size());
        for (std::size_t group_index = 0; group_index < chains.size(); ++group_index) {
            auto const &chain = chains[group_index];
            TasksRunner::ExecutionGroup group;
            group.order = group_index;
            group.task_ids = chain;
            group.callbacks.reserve(chain.size());
            std::unordered_set<std::size_t> dependency_groups;
            for (auto const &task_id : chain) {
                auto const &task = tasks.at(task_id);
                group.callbacks.push_back(task.callback);
                for (auto const &dependency : task.depends_on) {
                    auto const dependency_group = group_index_by_task.at(dependency);
                    if (dependency_group != group_index) {
                        dependency_groups.insert(dependency_group);
                    }
                }
            }
            group.depends_on.assign(dependency_groups.begin(), dependency_groups.end());
            std::ranges::sort(group.depends_on);
            plan.groups.push_back(std::move(group));
        }

        for (std::size_t group_index = 0; group_index < plan.groups.size(); ++group_index) {
            auto &group = plan.groups[group_index];
            std::unordered_set<std::size_t> user_groups;
            for (auto const &task_id : group.task_ids) {
                for (auto const &user_task_id : analysis.users_by_task.at(task_id)) {
                    auto const user_group = group_index_by_task.at(user_task_id);
                    if (user_group != group_index) {
                        user_groups.insert(user_group);
                    }
                }
            }
            group.users.assign(user_groups.begin(), user_groups.end());
            std::ranges::sort(group.users);
        }

        return plan;
    }
}

std::shared_ptr<TasksRunner::GraphVersion> TasksRunner::build_graph_version(
    std::shared_ptr<GraphVersion const> const &base,
    TaskGraphUpdate const &update,
    std::uint64_t revision)
{
    auto tasks = apply_update(base, update);
    auto ids = sorted_task_ids(tasks);
    auto const analysis = analyze_declared_graph(tasks, ids);

    auto graph = std::make_shared<GraphVersion>();
    graph->revision = revision;
    graph->is_complete = analysis.is_complete;
    graph->tasks = std::move(tasks);
    graph->sorted_task_ids = std::move(ids);
    if (graph->is_complete) {
        graph->compiled_plan = compile_execution_plan(graph->tasks, graph->sorted_task_ids);
    }
    return graph;
}

TasksRunner::TasksRunner(std::optional<std::size_t> worker_count)
  : worker_count_(std::max<std::size_t>(1, worker_count.value_or(std::thread::hardware_concurrency())))
  , active_graph_(std::make_shared<GraphVersion>())
{
    coordinator_thread_ = std::thread([this]() { coordinator_loop(); });
    worker_threads_.reserve(worker_count_);
    for (std::size_t index = 0; index < worker_count_; ++index) {
        worker_threads_.emplace_back([this]() { worker_loop(); });
    }
}

TasksRunner::~TasksRunner()
{
    {
        std::unique_lock update_lock(update_mutex_);
        std::unique_lock state_lock(state_mutex_);
        if (!shutdown_requested_) {
            shutdown_requested_ = true;
            pending_graph_ = std::make_shared<GraphVersion>();
            pending_graph_->revision = next_revision_;
            next_revision_ += 1;
            state_cv_.notify_all();
        }
        state_cv_.wait(state_lock, [&] { return workers_should_exit_; });
    }
    if (coordinator_thread_.joinable()) {
        coordinator_thread_.join();
    }
    for (auto &worker : worker_threads_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void TasksRunner::update_tasks(TaskGraphUpdate const &update)
{
    update_tasks(VersionedTaskGraphUpdate{
        .version_index = 0,
        .update = update,
    });
}

void TasksRunner::update_tasks(VersionedTaskGraphUpdate const &update)
{
    std::scoped_lock update_lock(update_mutex_);

    std::shared_ptr<GraphVersion const> base;
    std::uint64_t revision = 0;
    {
        std::scoped_lock state_lock(state_mutex_);
        if (shutdown_requested_) {
            throw std::runtime_error("cannot update tasks after shutdown");
        }
        if (pending_graph_ && !pending_graph_is_complete_) {
            if (!pending_update_version_index_.has_value()) {
                pending_update_version_index_ = update.version_index;
            } else if (*pending_update_version_index_ != update.version_index) {
                throw std::runtime_error("cannot mix task graph update version indices while pending graph has unmet dependencies");
            }
        }
        base = pending_graph_ ? std::const_pointer_cast<GraphVersion const>(pending_graph_)
                              : std::const_pointer_cast<GraphVersion const>(active_graph_);
        revision = next_revision_;
        next_revision_ += 1;
    }

    auto next_graph = build_graph_version(base, update.update, revision);
    {
        std::scoped_lock state_lock(state_mutex_);
        pending_graph_ = std::move(next_graph);
        pending_graph_is_complete_ = pending_graph_->is_complete;
        if (!pending_graph_is_complete_) {
            if (!pending_update_version_index_.has_value()) {
                pending_update_version_index_ = update.version_index;
            } else if (*pending_update_version_index_ != update.version_index) {
                throw std::runtime_error("cannot mix task graph update version indices while pending graph has unmet dependencies");
            }
        } else {
            pending_update_version_index_.reset();
        }
    }
    state_cv_.notify_all();
}

std::uint64_t TasksRunner::active_graph_revision() const
{
    std::scoped_lock lock(state_mutex_);
    return active_graph_->revision;
}

std::optional<std::uint64_t> TasksRunner::pending_graph_revision() const
{
    std::scoped_lock lock(state_mutex_);
    if (!pending_graph_) {
        return std::nullopt;
    }
    return pending_graph_->revision;
}

std::vector<std::string> TasksRunner::active_task_ids() const
{
    std::scoped_lock lock(state_mutex_);
    return active_graph_->sorted_task_ids;
}

std::size_t TasksRunner::active_execution_group_count() const
{
    std::scoped_lock lock(state_mutex_);
    return active_graph_->compiled_plan.groups.size();
}

void TasksRunner::coordinator_loop()
{
    std::unique_lock lock(state_mutex_);
    while (true) {
        if (!current_pass_) {
            if (pending_graph_ && pending_graph_is_complete_) {
                active_graph_ = std::move(pending_graph_);
            }
            if (shutdown_requested_ && active_graph_->compiled_plan.groups.empty()) {
                workers_should_exit_ = true;
                state_cv_.notify_all();
                return;
            }
            if (active_graph_->compiled_plan.groups.empty()) {
                state_cv_.wait(lock, [&] {
                    return workers_should_exit_
                        || (pending_graph_ != nullptr && pending_graph_is_complete_)
                        || (shutdown_requested_ && active_graph_->compiled_plan.groups.empty());
                });
                continue;
            }

            current_pass_ = std::make_shared<PassState>(
                std::const_pointer_cast<GraphVersion const>(active_graph_));
            state_cv_.notify_all();
        }

        auto pass = current_pass_;
        state_cv_.wait(lock, [&] {
            return workers_should_exit_
                || !current_pass_
                || (pass->completed == pass->graph->compiled_plan.groups.size()
                    && pass->running == 0);
        });

        if (workers_should_exit_) {
            return;
        }
        if (current_pass_ != pass) {
            continue;
        }
        auto const finished_revision = pass->graph->revision;
        lock.unlock();
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_task_runner_pass_finished_event,
            TasksRunnerPassFinished{
                .graph_revision = finished_revision,
            });
        lock.lock();
        if (current_pass_ != pass) {
            continue;
        }
        current_pass_.reset();
    }
}

void TasksRunner::worker_loop()
{
    std::unique_lock lock(state_mutex_);
    while (true) {
        state_cv_.wait(lock, [&] {
            return workers_should_exit_
                || (current_pass_ && !current_pass_->ready.empty());
        });

        if (workers_should_exit_) {
            return;
        }

        auto pass = current_pass_;
        auto const item = pass->ready.top();
        pass->ready.pop();
        pass->running += 1;
        auto const group_index = item.group_index;
        lock.unlock();

        for (auto const &callback : pass->graph->compiled_plan.groups[group_index].callbacks) {
            callback.invoke(callback.context);
        }

        lock.lock();
        pass->running -= 1;
        pass->completed += 1;
        for (auto const user_group_index : pass->graph->compiled_plan.groups[group_index].users) {
            auto &remaining = pass->remaining_dependencies[user_group_index];
            remaining -= 1;
            if (remaining == 0) {
                auto const &user_group = pass->graph->compiled_plan.groups[user_group_index];
                pass->ready.push(PassState::ReadyItem{
                    .user_count = user_group.users.size(),
                    .order = user_group.order,
                    .group_index = user_group_index,
                });
            }
        }
        state_cv_.notify_all();
    }
}
} // namespace iv
