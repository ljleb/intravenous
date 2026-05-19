#pragma once

#include "runtime/project_service.h"
#include "runtime/timeline.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace iv {
struct ServerInitializeRequest {
  std::filesystem::path workspace_root{};
};

struct GraphQueryBySpansRequest {
  std::filesystem::path file_path{};
  std::vector<SourceRange> ranges{};
  SourceRangeMatchMode match_mode = SourceRangeMatchMode::intersection;
};

struct GraphQueryActiveRegionsRequest {
  std::filesystem::path file_path{};
};

struct GetLogicalNodeRequest {
  std::string node_id{};
};

struct GetLogicalNodesRequest {
  std::vector<std::string> node_ids{};
};

struct CloseLaneViewRequest {
  std::string view_id{};
};

struct SetSampleInputValueRequest {
  std::string node_id{};
  size_t input_ordinal = 0;
  Sample value = 0.0f;
  std::optional<size_t> member_ordinal{};
};

struct ClearSampleInputValueOverrideRequest {
  std::string node_id{};
  size_t member_ordinal = 0;
  size_t input_ordinal = 0;
};

struct ServerShutdownRequest {};

// I have no freaking clue what this does but it clearly has 200x too many
// responsibilities
class SocketRpcServer {
  std::filesystem::path workspace_root;
  std::filesystem::path socket_path_value;
  std::filesystem::path lock_path_value;

  mutable std::mutex mutex;
  mutable std::mutex client_mutex;
  mutable std::mutex write_mutex;
  mutable std::mutex deferred_notification_mutex;
  mutable std::condition_variable ready_cv;
  mutable std::condition_variable stopped_cv;
  mutable std::condition_variable initialized_cv;
  bool ready = false;
  bool stopped = false;
  bool initialized = false;
  int listen_fd = -1;
  int client_fd = -1;
  bool client_initialized = false;
  bool defer_current_request_notifications = false;
  std::thread::id deferred_notification_thread{};
  std::vector<std::string> deferred_notifications;
  int lock_fd = -1;
  std::optional<std::jthread> accept_thread;
  std::optional<std::jthread> initialize_thread;
  RuntimeProjectInitializeResult initialized_result;
  std::exception_ptr initialization_exception;
  RuntimeProjectService *service = nullptr;

  void acquire_workspace_lock();
  void release_workspace_lock();
  bool send_message(int fd, std::string const &message);
  void notify_notification(RuntimeProjectNotification const &notification);
  void begin_deferring_current_request_notifications();
  std::vector<std::string> finish_deferring_current_request_notifications();
  void handle_client(int fd);
  void accept_loop(std::stop_token stop_token);
  void wait_until_initialized();
  RuntimeProjectService &service_or_throw();

public:
  explicit SocketRpcServer(std::filesystem::path workspace_root,
                           std::filesystem::path socket_path);
  ~SocketRpcServer();
  SocketRpcServer(SocketRpcServer &&) = delete;
  SocketRpcServer &operator=(SocketRpcServer &&) = delete;

  SocketRpcServer(SocketRpcServer const &) = delete;
  SocketRpcServer &operator=(SocketRpcServer const &) = delete;

  void attach_service(RuntimeProjectService &service);
  void detach_service(RuntimeProjectService const &service);
  void start();
  bool wait_until_ready(std::chrono::milliseconds timeout) const;
  void wait();
  std::filesystem::path socket_path() const;
  void request_shutdown();
  void send_runtime_project_notification(
      RuntimeProjectNotification const &notification);
};
} // namespace iv
