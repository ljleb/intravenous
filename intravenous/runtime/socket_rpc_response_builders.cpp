#include "runtime/socket_rpc_response_builders.h"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

namespace iv {
namespace {
using Json = nlohmann::ordered_json;

std::string serialize_json_line(Json const &value) {
    return value.dump() + "\n";
}

std::string jsonrpc_result(int request_id, Json result_json) {
    return serialize_json_line(Json{
        {"jsonrpc", "2.0"},
        {"id", request_id},
        {"result", std::move(result_json)},
    });
}

std::string jsonrpc_error(int request_id, int code, std::string const &message) {
    return serialize_json_line(Json{
        {"jsonrpc", "2.0"},
        {"id", request_id},
        {"error", {{"code", code}, {"message", message}}},
    });
}

[[noreturn]] void throw_unbuilt_response(char const *builder_name) {
    throw std::runtime_error(
        std::string(builder_name) +
        " did not produce a JSON-RPC response before SocketRpcServer::build()");
}
} // namespace

void SocketRpcAckResponseBuilder::fail(std::string message) {
    error_code = -32000;
    error_message = std::move(message);
}

void SocketRpcAckResponseBuilder::fail(int code, std::string message) {
    error_code = code;
    error_message = std::move(message);
}

std::string SocketRpcAckResponseBuilder::build(int request_id) const {
    if (!error_message.empty()) {
        return jsonrpc_error(request_id, error_code, error_message);
    }
    return jsonrpc_result(request_id, Json{{"ok", true}});
}

std::string SocketRpcInitializeResultBuilder::build(int) const {
    throw_unbuilt_response("SocketRpcInitializeResultBuilder");
}

std::string SocketRpcGraphQueryResultBuilder::build(int) const {
    throw_unbuilt_response("SocketRpcGraphQueryResultBuilder");
}

std::string SocketRpcRegionQueryResultBuilder::build(int) const {
    throw_unbuilt_response("SocketRpcRegionQueryResultBuilder");
}

std::string SocketRpcLogicalNodeResultBuilder::build(int) const {
    throw_unbuilt_response("SocketRpcLogicalNodeResultBuilder");
}

std::string SocketRpcLogicalNodesResultBuilder::build(int) const {
    throw_unbuilt_response("SocketRpcLogicalNodesResultBuilder");
}

std::string SocketRpcLaneViewResultBuilder::build(int) const {
    throw_unbuilt_response("SocketRpcLaneViewResultBuilder");
}
} // namespace iv
