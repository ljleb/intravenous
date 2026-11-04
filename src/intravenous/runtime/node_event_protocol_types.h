#pragma once

#include <intravenous/node/address.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace iv {
// Language-neutral JSON value shape for one semantic node event. These records
// contain no provider ABI layout, LLVM symbol, NodeCodeKey, or frontend-language
// spelling. A frontend binding generator may translate them to its own types.
enum class NodeEventValueKind {
    unsupported,
    boolean,
    signed_integer,
    unsigned_integer,
    floating_point,
    string,
    converted_scalar,
    enumeration,
    aggregate,
    sequence,
    node_address,
};

struct NodeEventValueSchema;

struct NodeEventFieldSchema {
    std::string name{};
    std::shared_ptr<NodeEventValueSchema const> value{};
};

struct NodeEventEnumeratorSchema {
    std::string name{};
    // Decimal text preserves the complete signed/unsigned 64-bit value for
    // frontend languages whose native JSON number type cannot do so.
    std::string value{};
};

struct NodeEventValueSchema {
    NodeEventValueKind kind = NodeEventValueKind::unsupported;
    std::string unsupported_reason{};
    bool integer_is_signed = false;
    std::size_t integer_bit_width = 0;
    std::vector<NodeEventFieldSchema> fields{};
    std::vector<NodeEventEnumeratorSchema> enumerators{};
    std::shared_ptr<NodeEventValueSchema const> element{};
    // Zero means a variable-length sequence. A nonzero value is the exact
    // element count of a fixed sequence.
    std::size_t fixed_element_count = 0;
};

struct NodeEventTypeId {
    std::string nominal_id{};
    std::string definition_fingerprint{};

    bool operator==(NodeEventTypeId const&) const = default;
};

struct NodeEventTypeSchema {
    NodeEventTypeId id{};
    // Diagnostic-only C++ spelling. Binding generators must use `id`, not this
    // string, for compatibility and collision handling.
    std::string display_name{};
    NodeEventValueSchema payload{};
};

struct NodeEventReceiverProtocol {
    NodeAddress address{};
    std::vector<NodeEventTypeId> supported_event_types{};
};

struct NodeEventDefinitionProtocol {
    std::string definition_id{};
    std::vector<NodeEventTypeId> supported_event_types{};
};

struct NodeEventProtocolSnapshot {
    std::uint32_t format_version = 1;
    std::uint64_t project_graph_generation = 0;
    std::uint64_t definitions_generation = 0;
    std::vector<NodeEventTypeSchema> event_types{};
    std::vector<NodeEventDefinitionProtocol> definitions{};
    std::vector<NodeEventReceiverProtocol> receivers{};
};
} // namespace iv
