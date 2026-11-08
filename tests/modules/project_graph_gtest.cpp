#include <intravenous/dsl.h>
#include <intravenous/module/package_definitions.h>
#include <intravenous/runtime/graph_connections.h>
#include <intravenous/runtime/node_definitions.h>
#include <intravenous/runtime/node_definitions_events.h>
#include <intravenous/runtime/node_definitions_project_graph_bridge.h>
#include <intravenous/runtime/node_instances.h>
#include <intravenous/runtime/package_pipeline_types.h>
#include <intravenous/runtime/project_graph.h>
#include <intravenous/runtime/project_graph_events.h>
#include <intravenous/runtime/project_graph_graph_connections_bridge.h>
#include <intravenous/runtime/project_graph_node_instances_bridge.h>
#include <intravenous/runtime/runtime_project_events.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using iv::details::ConfigurationArgument;
using iv::details::PackageDefinition;
using iv::details::PackageDefinitionKind;
using iv::details::RegisteredSignature;

constexpr char definition_id[] = "iv.test.project_graph.node";
constexpr char package_root[] = "/tmp/iv-project-graph-tests";
constexpr char source_file[] = "/tmp/iv-project-graph-tests/module.cpp";
RegisteredSignature const zero_signature{};
std::atomic<int> configure_calls{0};

constexpr char replacing_definition_id[] =
    "iv.test.project_graph.replacing";
constexpr char successor_definition_id[] =
    "iv.test.project_graph.successor";
constexpr char sender_definition_id[] =
    "iv.test.project_graph.sender";
constexpr char receiver_definition_id[] =
    "iv.test.project_graph.receiver";
constexpr char selector_sender_definition_id[] =
    "iv.test.project_graph.selector_sender";
constexpr char leaf_receiver_definition_id[] =
    "iv.test.project_graph.leaf_receiver";
constexpr char manual_control_leaf_definition_id[] =
    "iv.test.project_graph.manual_control_leaf";
std::atomic<int> successor_configure_calls{0};
std::atomic<int> successor_value{0};

struct ReplaceWithValue {
    int value = 0;
};

struct ReplaceWithoutArguments {};

struct ReplaceThenSend {
    int value = 0;
};

struct SuccessorMessage {
    int expected_value = 0;
};

struct RepeatDelivery {
    bool repeat = true;
};

struct SendToReceiver {
    int value = 0;
};

struct ReceiveFromSender {
    int value = 0;
};

struct SendToSelectedReceiver {
    int value = 0;
};

struct ReplaceAncestorThenRepeat {};

struct ReplaceUnrelatedThenRepeat {
    iv::NodeAddress target{};
};

struct LeafMessage {
    int value = 0;
};

std::atomic<int> successor_message_calls{0};
std::atomic<int> successor_message_value{0};
std::atomic<int> receiver_message_calls{0};
std::atomic<int> receiver_message_value{0};
std::atomic<int> leaf_message_calls{0};
std::atomic<int> leaf_message_value{0};

iv::details::ConfigurationExpressionTypeSchema const int_schema{
    .kind = iv::details::ConfigurationExpressionTypeKind::signed_integer,
    .size = sizeof(int),
    .alignment = alignof(int),
    .integer_is_signed = true,
    .integer_bit_width = sizeof(int) * 8,
    .construct_scalar =
        &iv::details::configuration_expression_details::construct_scalar<int>,
    .default_construct =
        &iv::details::configuration_expression_details::default_construct<int>,
    .destroy = &iv::details::configuration_expression_details::destroy<int>,
};
iv::details::ConfigurationTypeIdentity const int_identity{
    .nominal_id = "test:int",
    .nominal_id_size = 8,
    .definition_fingerprint = "test:int:v1",
    .definition_fingerprint_size = 11,
    .display_name = "int",
    .display_name_size = 3,
    .expression_schema = &int_schema,
};
constexpr char protocol_event_nominal_id[] = "iv.test.SetValue";
constexpr char protocol_event_fingerprint[] = "iv.test.SetValue:v1";
iv::details::ConfigurationExpressionFieldSchema const protocol_event_fields[]{
    {
        .name = "value",
        .name_size = 5,
        .byte_offset = 0,
        .type = &int_schema,
        .assign = &iv::details::configuration_expression_details::assign<int>,
    },
};
iv::details::ConfigurationExpressionTypeSchema const protocol_event_schema{
    .kind = iv::details::ConfigurationExpressionTypeKind::aggregate,
    .size = sizeof(ReplaceWithValue),
    .alignment = alignof(ReplaceWithValue),
    .fields = protocol_event_fields,
    .field_count = std::size(protocol_event_fields),
    .default_construct =
        &iv::details::configuration_expression_details::default_construct<
            ReplaceWithValue>,
    .destroy = &iv::details::configuration_expression_details::destroy<
        ReplaceWithValue>,
};
iv::details::ConfigurationTypeIdentity const protocol_event_identity{
    .nominal_id = protocol_event_nominal_id,
    .nominal_id_size = sizeof(protocol_event_nominal_id) - 1,
    .definition_fingerprint = protocol_event_fingerprint,
    .definition_fingerprint_size = sizeof(protocol_event_fingerprint) - 1,
    .display_name = "ReplaceWithValue",
    .display_name_size = 16,
    .expression_schema = &protocol_event_schema,
};
iv::details::ConfigurationExpressionTypeSchema const send_to_receiver_schema{
    .kind = iv::details::ConfigurationExpressionTypeKind::aggregate,
    .size = sizeof(SendToReceiver),
    .alignment = alignof(SendToReceiver),
    .fields = protocol_event_fields,
    .field_count = std::size(protocol_event_fields),
    .default_construct =
        &iv::details::configuration_expression_details::default_construct<
            SendToReceiver>,
    .destroy = &iv::details::configuration_expression_details::destroy<
        SendToReceiver>,
};
iv::details::ConfigurationTypeIdentity const send_to_receiver_identity{
    .nominal_id = "iv.test.SendToReceiver",
    .nominal_id_size = 22,
    .definition_fingerprint = "iv.test.SendToReceiver:v1",
    .definition_fingerprint_size = 25,
    .display_name = "SendToReceiver",
    .display_name_size = 14,
    .expression_schema = &send_to_receiver_schema,
};
iv::details::ConfigurationTypeIdentity const* const int_types[]{&int_identity};
iv::details::ConfigurationValueOperations const* const int_operations[]{
    iv::details::configuration_value_operations<int>(),
};
RegisteredSignature const one_int_signature{
    .parameter_types = int_types,
    .parameter_operations = int_operations,
    .required_argument_count = 1,
    .argument_count = 1,
};

RegisteredSignature const* signature_callback() { return &zero_signature; }

void configure_module(iv::GraphBuilder& graph, std::span<ConfigurationArgument>)
{
    using namespace iv;
    ++configure_calls;
    auto input = graph.input<"main">();
    graph.outputs("main"_P = input);
}

template<class Module, class Event,
         void (Module::*Handler)(
             iv::ModuleMessageContext<Module>&, Event const&) const>
void invoke_module_message_handler(
    void const* receiver,
    iv::NodeMessageContextOperations const& operations,
    iv::NodeAddress const& address,
    iv::ModuleMessageLocalRefs const* local_refs,
    void const* event)
{
    auto context = iv::ModuleMessageContext<Module>(
        operations, address, local_refs);
    (static_cast<Module const*>(receiver)->*Handler)(
        context, *static_cast<Event const*>(event));
}

template<class Node, class Event,
         void (Node::*Handler)(
             iv::NodeMessageContext<Node>&, Event const&) const>
void invoke_leaf_message_handler(
    void const* receiver,
    iv::NodeMessageContextOperations const& operations,
    iv::NodeAddress const& address,
    iv::ModuleMessageLocalRefs const*,
    void const* event)
{
    auto context = iv::NodeMessageContext<Node>(operations, address);
    (static_cast<Node const*>(receiver)->*Handler)(
        context, *static_cast<Event const*>(event));
}

struct ReplacingModule {
    int preserved = 0;

    explicit ReplacingModule(int preserved_) : preserved(preserved_) {}

    void build_graph(iv::GraphBuilder& graph) const
    {
        using namespace iv;
        auto input = graph.input<"main">();
        graph.outputs("main"_P = input);
    }

    void on_message(
        iv::ModuleMessageContext<ReplacingModule>& context,
        ReplaceWithValue const& event) const
    {
        context.replace<"iv.test.project_graph.successor">(
            preserved + event.value);
    }

    void on_message(
        iv::ModuleMessageContext<ReplacingModule>& context,
        ReplaceWithoutArguments const&) const
    {
        context.replace<"iv.test.project_graph.successor">();
    }

    void on_message(
        iv::ModuleMessageContext<ReplacingModule>& context,
        ReplaceThenSend const& event) const
    {
        auto const value = preserved + event.value;
        context.replace<"iv.test.project_graph.successor">(value);
        context.send(
            context.address(),
            SuccessorMessage{.expected_value = value});
    }

    void on_message(
        iv::ModuleMessageContext<ReplacingModule>& context,
        RepeatDelivery const& event) const
    {
        if (event.repeat) {
            context.send(
                context.address(),
                RepeatDelivery{.repeat = false});
        }
    }
};

struct SuccessorModule {
    int configured_value = 0;

    explicit SuccessorModule(int configured_value_)
        : configured_value(configured_value_)
    {
        ++successor_configure_calls;
        successor_value = configured_value;
    }

    void build_graph(iv::GraphBuilder& graph) const
    {
        using namespace iv;
        auto input = graph.input<"main">();
        graph.outputs("main"_P = input);
    }

    void on_message(
        iv::ModuleMessageContext<SuccessorModule>&,
        SuccessorMessage const& event) const
    {
        ++successor_message_calls;
        successor_message_value = event.expected_value == configured_value
            ? configured_value
            : -1;
    }

    void on_message(
        iv::ModuleMessageContext<SuccessorModule>& context,
        ReplaceWithValue const& event) const
    {
        context.replace<"iv.test.project_graph.successor">(event.value);
    }
};

struct ReceiverModule {
    void build_graph(iv::GraphBuilder& graph) const
    {
        using namespace iv;
        auto input = graph.input<"main">();
        graph.outputs("main"_P = input);
    }

    void on_message(
        iv::ModuleMessageContext<ReceiverModule>&,
        ReceiveFromSender const& event) const
    {
        ++receiver_message_calls;
        receiver_message_value = event.value;
    }
};

struct SenderModule {
    void build_graph(iv::GraphBuilder& graph) const
    {
        using namespace iv;
        details::iv_builder_prepare_node_source_identity(
            &graph,
            "project-graph-test:receiver",
            "receiver");
        auto receiver = graph.node<"iv.test.project_graph.receiver">();
        receiver._annotate_source_info(
            "project-graph-test:receiver",
            source_file,
            1,
            2);
        auto input = graph.input<"main">();
        graph.outputs("main"_P = input);
    }

    void on_message(
        iv::ModuleMessageContext<SenderModule>& context,
        SendToReceiver const& event) const
    {
        context.send(
            context.ref<"receiver">(),
            ReceiveFromSender{.value = event.value});
    }

    void on_message(
        iv::ModuleMessageContext<SenderModule>& context,
        ReplaceAncestorThenRepeat const&) const
    {
        auto const receiver = context.ref<"receiver">();
        context.send(receiver, ReceiveFromSender{.value = 1});
        context.replace<"iv.test.project_graph.sender">();
        context.send(receiver, ReceiveFromSender{.value = 2});
    }

    void on_message(
        iv::ModuleMessageContext<SenderModule>& context,
        ReplaceUnrelatedThenRepeat const& event) const
    {
        context.send(event.target, ReceiveFromSender{.value = 1});
        context.replace<"iv.test.project_graph.sender">();
        context.send(event.target, ReceiveFromSender{.value = 2});
    }
};

struct SelectorSenderModule {
    void build_graph(iv::GraphBuilder& graph) const
    {
        using namespace iv;
        details::iv_builder_prepare_node_source_identity(
            &graph,
            "project-graph-test:tiled-receiver",
            "receiver");
        auto receiver = graph.node<
            "iv.test.project_graph.receiver", stereo>();
        receiver._annotate_source_info(
            "project-graph-test:tiled-receiver",
            source_file,
            1,
            2);
        auto input = graph.input<"main">();
        graph.outputs("main"_P = input);
    }

    void on_message(
        iv::ModuleMessageContext<SelectorSenderModule>& context,
        SendToSelectedReceiver const& event) const
    {
        context.send(
            context.ref<"receiver">().member(0).tiled_child<"left">(),
            ReceiveFromSender{.value = event.value});
    }
};

struct LeafReceiver {
    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs() { return std::tuple{}; }

    void tick(iv::TickSampleContext<LeafReceiver> const&) const {}

    void on_message(
        iv::NodeMessageContext<LeafReceiver>&,
        LeafMessage const& event) const
    {
        ++leaf_message_calls;
        leaf_message_value = event.value;
    }
};

struct ManualControlLeaf {
    static constexpr auto inputs()
    {
        return std::array{
            iv::input<iv::Sample>(
                "gain", iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs() { return std::tuple{}; }

    void tick(iv::TickSampleContext<ManualControlLeaf> const&) const {}
};

template<class Module>
void configure_module_class(
    iv::GraphBuilder& graph,
    std::shared_ptr<Module const> receiver)
{
    receiver->build_graph(graph);
    iv::details::register_builder_module_class_message_handlers(
        graph, std::move(receiver));
}

void configure_replacing_module(
    iv::GraphBuilder& graph,
    std::span<ConfigurationArgument> arguments)
{
    if (arguments.size() != 1) {
        throw std::invalid_argument("replaceable module requires one argument");
    }
    configure_module_class(
        graph,
        std::make_shared<ReplacingModule const>(
            *static_cast<int const*>(arguments[0].data)));
}

void configure_successor_module(
    iv::GraphBuilder& graph,
    std::span<ConfigurationArgument> arguments)
{
    if (arguments.size() != 1) {
        throw std::invalid_argument("successor requires one argument");
    }
    configure_module_class(
        graph,
        std::make_shared<SuccessorModule const>(
            *static_cast<int const*>(arguments[0].data)));
}

void configure_receiver_module(
    iv::GraphBuilder& graph,
    std::span<ConfigurationArgument>)
{
    configure_module_class(
        graph,
        std::make_shared<ReceiverModule const>());
}

void configure_sender_module(
    iv::GraphBuilder& graph,
    std::span<ConfigurationArgument>)
{
    configure_module_class(
        graph,
        std::make_shared<SenderModule const>());
}

void configure_selector_sender_module(
    iv::GraphBuilder& graph,
    std::span<ConfigurationArgument>)
{
    configure_module_class(
        graph,
        std::make_shared<SelectorSenderModule const>());
}

iv::NodeRef configure_leaf_receiver(
    iv::GraphBuilder& graph,
    std::span<ConfigurationArgument>,
    iv::ChannelLayout const* tiled_layout)
{
    if (tiled_layout) {
        return iv::details::configure_concrete_node_tiled<LeafReceiver>(
            graph, *tiled_layout);
    }
    return iv::details::configure_concrete_node<LeafReceiver>(graph).node_ref();
}

iv::NodeRef configure_manual_control_leaf(
    iv::GraphBuilder& graph,
    std::span<ConfigurationArgument>,
    iv::ChannelLayout const* tiled_layout)
{
    if (tiled_layout) {
        return iv::details::configure_concrete_node_tiled<ManualControlLeaf>(
            graph, *tiled_layout);
    }
    return iv::details::configure_concrete_node<ManualControlLeaf>(graph)
        .node_ref();
}

RegisteredSignature const* one_int_signature_callback()
{
    return &one_int_signature;
}

template<class Module, class Event,
         void (Module::*Handler)(
             iv::ModuleMessageContext<Module>&, Event const&) const>
void add_module_message_handler(
    iv::PackageRevision& revision,
    char const* definition,
    std::size_t definition_size)
{
    revision.module_message_handlers.push_back(iv::ModuleMessageHandlerData{
        .definition_id = {
            .data = definition,
            .size = definition_size,
        },
        .event_type = iv::details::configuration_type_identity<Event>(),
        .invoke = &invoke_module_message_handler<Module, Event, Handler>,
    });
}

std::shared_ptr<iv::NodeDefinitionsSnapshot const> make_snapshot(std::uint64_t generation)
{
    auto revision = std::make_shared<iv::PackageRevision>();
    revision->package_id = "iv.test.project_graph.package";
    revision->package_root = package_root;
    revision->revision = generation;
    revision->provider_definitions.push_back(PackageDefinition{
        .kind = PackageDefinitionKind::module,
        .id = definition_id,
        .id_size = sizeof(definition_id) - 1,
        .source_file = source_file,
        .source_file_size = sizeof(source_file) - 1,
        .package_root = package_root,
        .package_root_size = sizeof(package_root) - 1,
        .module_build = &configure_module,
        .signature = &signature_callback,
    });

    auto snapshot = std::make_shared<iv::NodeDefinitionsSnapshot>();
    snapshot->generation = generation;
    snapshot->package_revisions.push_back(revision);
    snapshot->by_id.emplace(definition_id, iv::NodeDefinitionEntry{
        .definition_id = definition_id,
        .kind = iv::NodeDefinitionKind::module,
        .version = generation,
        .definition = iv::ModuleNodeDefinition{
            .definition_id = definition_id,
            .package_id = revision->package_id,
            .package_root = revision->package_root,
            .module_id = definition_id,
            .provider = iv::NodeDefinitionProvider{
                .module_build = &configure_module,
                .signature = &signature_callback,
            },
        },
    });
    return snapshot;
}

std::shared_ptr<iv::NodeDefinitionsSnapshot const> make_replacement_snapshot(
    std::uint64_t generation)
{
    auto snapshot = std::const_pointer_cast<iv::NodeDefinitionsSnapshot>(
        make_snapshot(generation));
    auto revision = std::const_pointer_cast<iv::PackageRevision>(
        snapshot->package_revisions.front());
    revision->provider_definitions.push_back(PackageDefinition{
        .kind = PackageDefinitionKind::module,
        .id = replacing_definition_id,
        .id_size = sizeof(replacing_definition_id) - 1,
        .source_file = source_file,
        .source_file_size = sizeof(source_file) - 1,
        .package_root = package_root,
        .package_root_size = sizeof(package_root) - 1,
        .module_build = &configure_replacing_module,
        .signature = &one_int_signature_callback,
    });
    revision->provider_definitions.push_back(PackageDefinition{
        .kind = PackageDefinitionKind::module,
        .id = successor_definition_id,
        .id_size = sizeof(successor_definition_id) - 1,
        .source_file = source_file,
        .source_file_size = sizeof(source_file) - 1,
        .package_root = package_root,
        .package_root_size = sizeof(package_root) - 1,
        .module_build = &configure_successor_module,
        .signature = &one_int_signature_callback,
    });
    revision->provider_definitions.push_back(PackageDefinition{
        .kind = PackageDefinitionKind::module,
        .id = sender_definition_id,
        .id_size = sizeof(sender_definition_id) - 1,
        .source_file = source_file,
        .source_file_size = sizeof(source_file) - 1,
        .package_root = package_root,
        .package_root_size = sizeof(package_root) - 1,
        .module_build = &configure_sender_module,
        .signature = &signature_callback,
    });
    revision->provider_definitions.push_back(PackageDefinition{
        .kind = PackageDefinitionKind::module,
        .id = receiver_definition_id,
        .id_size = sizeof(receiver_definition_id) - 1,
        .source_file = source_file,
        .source_file_size = sizeof(source_file) - 1,
        .package_root = package_root,
        .package_root_size = sizeof(package_root) - 1,
        .module_build = &configure_receiver_module,
        .signature = &signature_callback,
    });
    revision->provider_definitions.push_back(PackageDefinition{
        .kind = PackageDefinitionKind::module,
        .id = selector_sender_definition_id,
        .id_size = sizeof(selector_sender_definition_id) - 1,
        .source_file = source_file,
        .source_file_size = sizeof(source_file) - 1,
        .package_root = package_root,
        .package_root_size = sizeof(package_root) - 1,
        .module_build = &configure_selector_sender_module,
        .signature = &signature_callback,
    });
    revision->provider_definitions.push_back(PackageDefinition{
        .kind = PackageDefinitionKind::node,
        .id = leaf_receiver_definition_id,
        .id_size = sizeof(leaf_receiver_definition_id) - 1,
        .source_file = source_file,
        .source_file_size = sizeof(source_file) - 1,
        .package_root = package_root,
        .package_root_size = sizeof(package_root) - 1,
        .node_build = &configure_leaf_receiver,
        .node_compiler_record =
            &iv::details::node_compiler_record<LeafReceiver>,
        .signature = &signature_callback,
    });
    revision->provider_definitions.push_back(PackageDefinition{
        .kind = PackageDefinitionKind::node,
        .id = manual_control_leaf_definition_id,
        .id_size = sizeof(manual_control_leaf_definition_id) - 1,
        .source_file = source_file,
        .source_file_size = sizeof(source_file) - 1,
        .package_root = package_root,
        .package_root_size = sizeof(package_root) - 1,
        .node_build = &configure_manual_control_leaf,
        .node_compiler_record =
            &iv::details::node_compiler_record<ManualControlLeaf>,
        .signature = &signature_callback,
    });
    add_module_message_handler<
        ReplacingModule,
        ReplaceWithValue,
        static_cast<void (ReplacingModule::*)(
            iv::ModuleMessageContext<ReplacingModule>&,
            ReplaceWithValue const&) const>(&ReplacingModule::on_message)>(
        *revision,
        replacing_definition_id,
        sizeof(replacing_definition_id) - 1);
    add_module_message_handler<
        ReplacingModule,
        ReplaceWithoutArguments,
        static_cast<void (ReplacingModule::*)(
            iv::ModuleMessageContext<ReplacingModule>&,
            ReplaceWithoutArguments const&) const>(&ReplacingModule::on_message)>(
        *revision,
        replacing_definition_id,
        sizeof(replacing_definition_id) - 1);
    add_module_message_handler<
        ReplacingModule,
        ReplaceThenSend,
        static_cast<void (ReplacingModule::*)(
            iv::ModuleMessageContext<ReplacingModule>&,
            ReplaceThenSend const&) const>(&ReplacingModule::on_message)>(
        *revision,
        replacing_definition_id,
        sizeof(replacing_definition_id) - 1);
    add_module_message_handler<
        ReplacingModule,
        RepeatDelivery,
        static_cast<void (ReplacingModule::*)(
            iv::ModuleMessageContext<ReplacingModule>&,
            RepeatDelivery const&) const>(&ReplacingModule::on_message)>(
        *revision,
        replacing_definition_id,
        sizeof(replacing_definition_id) - 1);
    add_module_message_handler<
        SuccessorModule,
        SuccessorMessage,
        &SuccessorModule::on_message>(
        *revision,
        successor_definition_id,
        sizeof(successor_definition_id) - 1);
    add_module_message_handler<
        SuccessorModule,
        ReplaceWithValue,
        static_cast<void (SuccessorModule::*)(
            iv::ModuleMessageContext<SuccessorModule>&,
            ReplaceWithValue const&) const>(&SuccessorModule::on_message)>(
        *revision,
        successor_definition_id,
        sizeof(successor_definition_id) - 1);
    add_module_message_handler<
        ReceiverModule,
        ReceiveFromSender,
        &ReceiverModule::on_message>(
        *revision,
        receiver_definition_id,
        sizeof(receiver_definition_id) - 1);
    add_module_message_handler<
        SenderModule,
        SendToReceiver,
        &SenderModule::on_message>(
        *revision,
        sender_definition_id,
        sizeof(sender_definition_id) - 1);
    add_module_message_handler<
        SenderModule,
        ReplaceAncestorThenRepeat,
        static_cast<void (SenderModule::*)(
            iv::ModuleMessageContext<SenderModule>&,
            ReplaceAncestorThenRepeat const&) const>(&SenderModule::on_message)>(
        *revision,
        sender_definition_id,
        sizeof(sender_definition_id) - 1);
    add_module_message_handler<
        SenderModule,
        ReplaceUnrelatedThenRepeat,
        static_cast<void (SenderModule::*)(
            iv::ModuleMessageContext<SenderModule>&,
            ReplaceUnrelatedThenRepeat const&) const>(&SenderModule::on_message)>(
        *revision,
        sender_definition_id,
        sizeof(sender_definition_id) - 1);
    add_module_message_handler<
        SelectorSenderModule,
        SendToSelectedReceiver,
        &SelectorSenderModule::on_message>(
        *revision,
        selector_sender_definition_id,
        sizeof(selector_sender_definition_id) - 1);
    revision->node_message_handlers.push_back(iv::NodeMessageHandlerData{
        .code_key = iv::details::node_compiler_record<LeafReceiver>.code_key,
        .event_type = iv::details::configuration_type_identity<LeafMessage>(),
        .invoke = &invoke_leaf_message_handler<
            LeafReceiver,
            LeafMessage,
            &LeafReceiver::on_message>,
    });
    snapshot->by_id.emplace(replacing_definition_id, iv::NodeDefinitionEntry{
        .definition_id = replacing_definition_id,
        .kind = iv::NodeDefinitionKind::module,
        .version = generation,
        .definition = iv::ModuleNodeDefinition{
            .definition_id = replacing_definition_id,
            .package_id = revision->package_id,
            .package_root = revision->package_root,
            .module_id = replacing_definition_id,
            .provider = iv::NodeDefinitionProvider{
                .module_build = &configure_replacing_module,
                .signature = &one_int_signature_callback,
            },
        },
    });
    snapshot->by_id.emplace(successor_definition_id, iv::NodeDefinitionEntry{
        .definition_id = successor_definition_id,
        .kind = iv::NodeDefinitionKind::module,
        .version = generation,
        .definition = iv::ModuleNodeDefinition{
            .definition_id = successor_definition_id,
            .package_id = revision->package_id,
            .package_root = revision->package_root,
            .module_id = successor_definition_id,
            .provider = iv::NodeDefinitionProvider{
                .module_build = &configure_successor_module,
                .signature = &one_int_signature_callback,
            },
        },
    });
    snapshot->by_id.emplace(sender_definition_id, iv::NodeDefinitionEntry{
        .definition_id = sender_definition_id,
        .kind = iv::NodeDefinitionKind::module,
        .version = generation,
        .definition = iv::ModuleNodeDefinition{
            .definition_id = sender_definition_id,
            .package_id = revision->package_id,
            .package_root = revision->package_root,
            .module_id = sender_definition_id,
            .provider = iv::NodeDefinitionProvider{
                .module_build = &configure_sender_module,
                .signature = &signature_callback,
            },
        },
    });
    snapshot->by_id.emplace(receiver_definition_id, iv::NodeDefinitionEntry{
        .definition_id = receiver_definition_id,
        .kind = iv::NodeDefinitionKind::module,
        .version = generation,
        .definition = iv::ModuleNodeDefinition{
            .definition_id = receiver_definition_id,
            .package_id = revision->package_id,
            .package_root = revision->package_root,
            .module_id = receiver_definition_id,
            .provider = iv::NodeDefinitionProvider{
                .module_build = &configure_receiver_module,
                .signature = &signature_callback,
            },
        },
    });
    snapshot->by_id.emplace(selector_sender_definition_id, iv::NodeDefinitionEntry{
        .definition_id = selector_sender_definition_id,
        .kind = iv::NodeDefinitionKind::module,
        .version = generation,
        .definition = iv::ModuleNodeDefinition{
            .definition_id = selector_sender_definition_id,
            .package_id = revision->package_id,
            .package_root = revision->package_root,
            .module_id = selector_sender_definition_id,
            .provider = iv::NodeDefinitionProvider{
                .module_build = &configure_selector_sender_module,
                .signature = &signature_callback,
            },
        },
    });
    snapshot->by_id.emplace(leaf_receiver_definition_id, iv::NodeDefinitionEntry{
        .definition_id = leaf_receiver_definition_id,
        .kind = iv::NodeDefinitionKind::leaf,
        .version = generation,
        .definition = iv::LeafNodeDefinition{
            .definition_id = leaf_receiver_definition_id,
            .package_id = revision->package_id,
            .package_root = revision->package_root,
            .provider = iv::NodeDefinitionProvider{
                .leaf_build = &configure_leaf_receiver,
                .signature = &signature_callback,
            },
            .compiler_record = iv::details::node_compiler_record<LeafReceiver>,
        },
    });
    snapshot->by_id.emplace(
        manual_control_leaf_definition_id,
        iv::NodeDefinitionEntry{
            .definition_id = manual_control_leaf_definition_id,
            .kind = iv::NodeDefinitionKind::leaf,
            .version = generation,
            .definition = iv::LeafNodeDefinition{
                .definition_id = manual_control_leaf_definition_id,
                .package_id = revision->package_id,
                .package_root = revision->package_root,
                .provider = iv::NodeDefinitionProvider{
                    .leaf_build = &configure_manual_control_leaf,
                    .signature = &signature_callback,
                },
                .compiler_record =
                    iv::details::node_compiler_record<ManualControlLeaf>,
            },
        });
    return snapshot;
}

std::shared_ptr<iv::NodeDefinitionsSnapshot const> make_serialized_sender_snapshot(
    std::uint64_t generation)
{
    auto snapshot = std::const_pointer_cast<iv::NodeDefinitionsSnapshot>(
        make_replacement_snapshot(generation));
    auto revision = std::const_pointer_cast<iv::PackageRevision>(
        snapshot->package_revisions.front());
    auto const sender = std::ranges::find_if(
        revision->module_message_handlers,
        [](iv::ModuleMessageHandlerData const& handler) {
            auto const definition = std::string_view(
                static_cast<char const*>(handler.definition_id.data),
                handler.definition_id.size);
            return definition == sender_definition_id
                && handler.event_type
                    == iv::details::configuration_type_identity<SendToReceiver>();
        });
    if (sender == revision->module_message_handlers.end()) {
        throw std::logic_error("serialized sender handler was not registered");
    }
    sender->event_type = &send_to_receiver_identity;
    return snapshot;
}

std::shared_ptr<iv::NodeDefinitionsSnapshot const> make_protocol_snapshot(
    std::uint64_t generation)
{
    auto snapshot = std::const_pointer_cast<iv::NodeDefinitionsSnapshot>(
        make_replacement_snapshot(generation));
    auto revision = std::const_pointer_cast<iv::PackageRevision>(
        snapshot->package_revisions.front());
    revision->node_message_handlers.clear();
    revision->module_message_handlers.clear();
    revision->module_message_handlers.push_back(iv::ModuleMessageHandlerData{
        .definition_id = {
            .data = replacing_definition_id,
            .size = sizeof(replacing_definition_id) - 1,
        },
        .event_type = &protocol_event_identity,
        .invoke = &invoke_module_message_handler<
            ReplacingModule,
            ReplaceWithValue,
            static_cast<void (ReplacingModule::*)(
                iv::ModuleMessageContext<ReplacingModule>&,
                ReplaceWithValue const&) const>(&ReplacingModule::on_message)>,
    });
    revision->module_message_handlers.push_back(iv::ModuleMessageHandlerData{
        .definition_id = {
            .data = successor_definition_id,
            .size = sizeof(successor_definition_id) - 1,
        },
        .event_type = &protocol_event_identity,
        .invoke = &invoke_module_message_handler<
            SuccessorModule,
            ReplaceWithValue,
            static_cast<void (SuccessorModule::*)(
                iv::ModuleMessageContext<SuccessorModule>&,
                ReplaceWithValue const&) const>(&SuccessorModule::on_message)>,
    });
    return snapshot;
}

struct ProjectGraphFixture : ::testing::Test {
    iv::NodeInstances instances;
    iv::GraphConnections connections;
    iv::ProjectGraph project_graph;
    iv::project_graph_node_instances_bridge::scope instances_scope{
        project_graph, instances};
    iv::project_graph_graph_connections_bridge::scope connections_scope{
        project_graph, connections};

    void SetUp() override
    {
        configure_calls = 0;
        successor_configure_calls = 0;
        successor_value = 0;
        successor_message_calls = 0;
        successor_message_value = 0;
        receiver_message_calls = 0;
        receiver_message_value = 0;
        leaf_message_calls = 0;
        leaf_message_value = 0;
    }
};

struct ManualControlCompileRecord {
    std::uint64_t project_generation = 0;
    std::vector<iv::ManualControlId> dynamic_inputs{};
};

struct ManualControlTargetRecord {
    std::uint64_t project_generation = 0;
    iv::ManualControlId id{};
    iv::Sample value{};
};

class ManualControlSchedulerProbe {
    mutable std::mutex mutex_{};
    std::condition_variable changed_{};
    std::vector<ManualControlCompileRecord> compilations_{};
    std::vector<ManualControlCompileRecord> stages_{};
    std::vector<ManualControlTargetRecord> targets_{};
    bool block_next_compilation_ = false;
    bool compilation_is_blocked_ = false;
    bool release_blocked_compilation_ = false;

    static std::vector<iv::ManualControlId> dynamic_inputs(
        std::span<iv::ConfiguredManualControl const> controls)
    {
        std::vector<iv::ManualControlId> result;
        for (auto const& control : controls) {
            if (control.dynamic) result.push_back(control.id);
        }
        return result;
    }

public:
    iv::GraphJitCompileResult compile(
        iv::ProjectGraphGraphJitRequest const& request)
    {
        auto const inputs = request.successor.graph
            ? dynamic_inputs(request.successor.graph->manual_controls)
            : std::vector<iv::ManualControlId>{};
        {
            std::unique_lock lock(mutex_);
            compilations_.push_back({
                .project_generation = request.successor.project_generation,
                .dynamic_inputs = inputs,
            });
            auto const block = std::exchange(block_next_compilation_, false);
            if (block) {
                compilation_is_blocked_ = true;
                changed_.notify_all();
                changed_.wait(lock, [&] {
                    return release_blocked_compilation_;
                });
                compilation_is_blocked_ = false;
                release_blocked_compilation_ = false;
            }
            changed_.notify_all();
        }

        auto compiled = std::make_shared<iv::CompiledGraph>();
        compiled->project_generation = request.successor.project_generation;
        if (request.successor.graph) {
            for (auto const& configured :
                 request.successor.graph->manual_controls) {
                compiled->manual_controls.push_back(
                    iv::ManualControlRuntimeBinding{
                        .configured = configured,
                    });
            }
        }
        return {
            .attempted = true,
            .compiled_graph = std::move(compiled),
        };
    }

    iv::ProjectGraphGraphExecutorResult accept_structural_graph(
        std::shared_ptr<iv::CompiledGraph const> const&)
    {
        return {.attempted = true, .accepted = true};
    }

    iv::GraphExecutorStageResult stage_realtime_code(
        std::shared_ptr<iv::CompiledGraph const> graph)
    {
        std::vector<iv::ManualControlId> inputs;
        if (graph) {
            for (auto const& binding : graph->manual_controls) {
                if (binding.configured.dynamic) {
                    inputs.push_back(binding.configured.id);
                }
            }
        }
        {
            std::scoped_lock lock(mutex_);
            stages_.push_back({
                .project_generation = graph ? graph->project_generation : 0,
                .dynamic_inputs = std::move(inputs),
            });
        }
        changed_.notify_all();
        return iv::GraphExecutorStageResult::accepted;
    }

    bool set_target(
        std::uint64_t project_generation,
        iv::ManualControlId const& id,
        iv::Sample value)
    {
        {
            std::scoped_lock lock(mutex_);
            targets_.push_back({
                .project_generation = project_generation,
                .id = id,
                .value = value,
            });
        }
        changed_.notify_all();
        return true;
    }

    void block_next_compilation()
    {
        std::scoped_lock lock(mutex_);
        block_next_compilation_ = true;
        release_blocked_compilation_ = false;
    }

    void release_blocked_compilation()
    {
        {
            std::scoped_lock lock(mutex_);
            release_blocked_compilation_ = true;
        }
        changed_.notify_all();
    }

    [[nodiscard]] bool wait_for_blocked_compilation()
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(
            lock, std::chrono::seconds(5),
            [&] { return compilation_is_blocked_; });
    }

    [[nodiscard]] bool wait_for_compilations(std::size_t count)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(
            lock, std::chrono::seconds(5),
            [&] { return compilations_.size() >= count; });
    }

    [[nodiscard]] bool wait_for_stages(
        std::size_t count,
        std::chrono::milliseconds timeout = std::chrono::seconds(5))
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(
            lock, timeout,
            [&] { return stages_.size() >= count; });
    }

    [[nodiscard]] std::size_t compilation_count() const
    {
        std::scoped_lock lock(mutex_);
        return compilations_.size();
    }

    [[nodiscard]] std::vector<ManualControlCompileRecord> compilations() const
    {
        std::scoped_lock lock(mutex_);
        return compilations_;
    }

    [[nodiscard]] std::vector<ManualControlCompileRecord> stages() const
    {
        std::scoped_lock lock(mutex_);
        return stages_;
    }

    [[nodiscard]] std::vector<ManualControlTargetRecord> targets() const
    {
        std::scoped_lock lock(mutex_);
        return targets_;
    }
};

ManualControlSchedulerProbe* active_manual_control_scheduler_probe = nullptr;

iv::GraphJitCompileResult probe_manual_control_compile(
    iv::ProjectGraphGraphJitRequest const& request)
{
    return active_manual_control_scheduler_probe->compile(request);
}

iv::ProjectGraphGraphExecutorResult probe_structural_graph_acceptance(
    std::shared_ptr<iv::CompiledGraph const> const& graph)
{
    return active_manual_control_scheduler_probe->accept_structural_graph(graph);
}

iv::GraphExecutorStageResult probe_realtime_code_stage(
    std::shared_ptr<iv::CompiledGraph const> graph)
{
    return active_manual_control_scheduler_probe->stage_realtime_code(
        std::move(graph));
}

bool probe_manual_control_target(
    std::uint64_t project_generation,
    iv::ManualControlId const& id,
    iv::Sample value)
{
    return active_manual_control_scheduler_probe->set_target(
        project_generation, id, value);
}

class ScopedManualControlSchedulerEvents {
    iv::ProjectGraphGraphJitRequestedEvent previous_graph_jit_{};
    iv::ProjectGraphGraphExecutorRequestedEvent previous_graph_executor_{};
    iv::ProjectGraphRealtimeCodeRequestedEvent previous_realtime_code_{};
    iv::ProjectGraphManualControlTargetRequestedEvent previous_target_{};

public:
    explicit ScopedManualControlSchedulerEvents(
        ManualControlSchedulerProbe& probe)
        : previous_graph_jit_(std::exchange(
              iv::iv_runtime_project_graph_graph_jit_requested_event,
              &probe_manual_control_compile))
        , previous_graph_executor_(std::exchange(
              iv::iv_runtime_project_graph_graph_executor_requested_event,
              &probe_structural_graph_acceptance))
        , previous_realtime_code_(std::exchange(
              iv::iv_runtime_project_graph_realtime_code_requested_event,
              &probe_realtime_code_stage))
        , previous_target_(std::exchange(
              iv::iv_runtime_project_graph_manual_control_target_requested_event,
              &probe_manual_control_target))
    {
        if (std::exchange(active_manual_control_scheduler_probe, &probe)) {
            throw std::logic_error(
                "manual-control scheduler probe is already active");
        }
    }

    ~ScopedManualControlSchedulerEvents()
    {
        active_manual_control_scheduler_probe = nullptr;
        iv::iv_runtime_project_graph_graph_jit_requested_event =
            previous_graph_jit_;
        iv::iv_runtime_project_graph_graph_executor_requested_event =
            previous_graph_executor_;
        iv::iv_runtime_project_graph_realtime_code_requested_event =
            previous_realtime_code_;
        iv::iv_runtime_project_graph_manual_control_target_requested_event =
            previous_target_;
    }
};

struct ProjectGraphManualControlSchedulingFixture : ::testing::Test {
    ManualControlSchedulerProbe probe;
    ScopedManualControlSchedulerEvents event_scope{probe};
    iv::NodeInstances instances;
    iv::GraphConnections connections;
    iv::ProjectGraph project_graph;
    iv::project_graph_node_instances_bridge::scope instances_scope{
        project_graph, instances};
    iv::project_graph_graph_connections_bridge::scope connections_scope{
        project_graph, connections};

    void TearDown() override
    {
        // A failed assertion must not leave ProjectGraph's worker blocked while
        // the fixture joins it during destruction.
        probe.release_blocked_compilation();
    }

    std::vector<iv::ManualControlId> realize_controls(std::size_t count)
    {
        project_graph.handle_node_definitions_snapshot_changed(
            iv::NodeDefinitionsSnapshotChanged{
                .snapshot = make_replacement_snapshot(1),
            });

        iv::ProjectReplaceGraphRequest request;
        std::vector<iv::ManualControlId> result;
        for (std::size_t index = 0; index < count; ++index) {
            auto const instance_id = "instance:manual:" +
                std::to_string(index);
            request.instances.push_back({
                .instance_id = instance_id,
                .definition_id = manual_control_leaf_definition_id,
            });
            result.push_back({
                .node = {.instance_id = instance_id},
                .input_name = "gain",
            });
            request.manual_controls.push_back({.id = result.back()});
        }

        iv::ProjectAckBuilder builder;
        project_graph.handle_project_replace_graph(request, builder);
        EXPECT_NO_THROW(builder.build());
        return result;
    }
};
} // namespace

TEST_F(ProjectGraphFixture, DefinitionAndMutationTransactionsBuildWholeRootGenerations)
{
    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{.snapshot = make_snapshot(1)});
    auto generation = project_graph.current_generation();
    ASSERT_TRUE(generation);
    EXPECT_EQ(generation->generation, 1u);
    EXPECT_EQ(generation->definitions_generation, 1u);
    EXPECT_TRUE(generation->placements.empty());

    iv::ProjectStringBuilder create_builder;
    project_graph.handle_project_create_node_instance(
        iv::ProjectCreateNodeInstanceRequest{
            .instance_id = "instance:a",
            .definition_id = definition_id,
        },
        create_builder);
    EXPECT_EQ(create_builder.build(), "instance:a");

    generation = project_graph.current_generation();
    ASSERT_TRUE(generation);
    EXPECT_EQ(generation->generation, 2u);
    ASSERT_EQ(generation->placements.size(), 1u);
    EXPECT_TRUE(generation->placements.contains("instance:a"));
    EXPECT_FALSE(project_graph.configuration_failure());
    EXPECT_EQ(configure_calls.load(), 1);

    iv::ProjectAckBuilder update_builder;
    project_graph.handle_project_update_node_instances(
        iv::ProjectUpdateNodeInstancesRequest{
            .updates = {{.instance_id = "instance:a", .display_name = "Renamed"}},
        },
        update_builder);
    EXPECT_NO_THROW(update_builder.build());
    generation = project_graph.current_generation();
    EXPECT_EQ(generation->generation, 3u);
    EXPECT_EQ(generation->placements.size(), 1u);
    // Same definitions generation + same typed value tuple reuses configuration.
    EXPECT_EQ(configure_calls.load(), 1);

    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{.snapshot = make_snapshot(2)});
    generation = project_graph.current_generation();
    EXPECT_EQ(generation->generation, 4u);
    EXPECT_EQ(generation->definitions_generation, 2u);
    EXPECT_EQ(generation->placements.size(), 1u);
    EXPECT_EQ(configure_calls.load(), 2);

    iv::ProjectAckBuilder delete_builder;
    project_graph.handle_project_delete_node_instance(
        iv::ProjectDeleteNodeInstanceRequest{.instance_id = "instance:a"},
        delete_builder);
    EXPECT_NO_THROW(delete_builder.build());
    generation = project_graph.current_generation();
    EXPECT_EQ(generation->generation, 5u);
    EXPECT_TRUE(generation->placements.empty());
    EXPECT_TRUE(instances.list_instances().empty());
}

TEST_F(ProjectGraphFixture, MissingDefinitionFailsThePassAndRealizesWhenDefinitionArrives)
{
    iv::ProjectStringBuilder builder;
    project_graph.handle_project_create_node_instance(
        iv::ProjectCreateNodeInstanceRequest{
            .instance_id = "instance:pending",
            .definition_id = definition_id,
            .package_root = std::filesystem::path(package_root),
        },
        builder);
    EXPECT_EQ(builder.build(), "instance:pending");

    // The instance stays desired, but its configuration pass fails. This is
    // the project's first build, so no generation becomes current.
    EXPECT_FALSE(project_graph.current_generation());
    auto failure = project_graph.configuration_failure();
    ASSERT_TRUE(failure);
    ASSERT_EQ(failure->node_instance_diagnostics.size(), 1u);
    EXPECT_EQ(failure->node_instance_diagnostics.front().instance_id, "instance:pending");
    auto listed = instances.list_instances();
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_FALSE(listed.front().realized);

    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{.snapshot = make_snapshot(1)});
    auto generation = project_graph.current_generation();
    ASSERT_TRUE(generation);
    ASSERT_TRUE(generation->placements.contains("instance:pending"));
    EXPECT_FALSE(project_graph.configuration_failure());
    listed = instances.list_instances();
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_TRUE(listed.front().realized);
}

TEST_F(ProjectGraphFixture, InvalidEditKeepsThePreviousGenerationCurrent)
{
    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{.snapshot = make_snapshot(1)});
    iv::ProjectStringBuilder create_builder;
    project_graph.handle_project_create_node_instance(
        iv::ProjectCreateNodeInstanceRequest{
            .instance_id = "instance:a",
            .definition_id = definition_id,
        },
        create_builder);
    EXPECT_EQ(create_builder.build(), "instance:a");
    auto const good = project_graph.current_generation();
    ASSERT_TRUE(good);
    ASSERT_TRUE(good->placements.contains("instance:a"));

    // A second instance of an unknown definition fails the pass; the
    // previous generation, with instance:a, stays current.
    iv::ProjectStringBuilder invalid_builder;
    project_graph.handle_project_create_node_instance(
        iv::ProjectCreateNodeInstanceRequest{
            .instance_id = "instance:b",
            .definition_id = "iv.test.project_graph.unknown",
            .package_root = std::filesystem::path(package_root),
        },
        invalid_builder);
    EXPECT_EQ(invalid_builder.build(), "instance:b");
    EXPECT_EQ(project_graph.current_generation(), good);
    auto failure = project_graph.configuration_failure();
    ASSERT_TRUE(failure);
    ASSERT_EQ(failure->node_instance_diagnostics.size(), 1u);
    EXPECT_EQ(failure->node_instance_diagnostics.front().instance_id, "instance:b");

    // Removing the invalid instance succeeds again and clears the failure.
    iv::ProjectAckBuilder delete_builder;
    project_graph.handle_project_delete_node_instance(
        iv::ProjectDeleteNodeInstanceRequest{.instance_id = "instance:b"},
        delete_builder);
    EXPECT_NO_THROW(delete_builder.build());
    EXPECT_NE(project_graph.current_generation(), good);
    EXPECT_FALSE(project_graph.configuration_failure());
}

TEST_F(ProjectGraphFixture, NodeMessageReplacementPersistsCompleteSuccessorArguments)
{
    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{
            .snapshot = make_replacement_snapshot(1),
        });
    iv::ProjectStringBuilder create_builder;
    project_graph.handle_project_create_node_instance(
        iv::ProjectCreateNodeInstanceRequest{
            .instance_id = "instance:replaceable",
            .definition_id = replacing_definition_id,
            .configuration_source = "30",
        },
        create_builder);
    ASSERT_EQ(create_builder.build(), "instance:replaceable");
    auto const before = project_graph.current_generation();
    ASSERT_TRUE(before);

    auto result = project_graph.dispatch_node_message(
        iv::NodeAddress{.instance_id = "instance:replaceable"},
        iv::OwnedNodeMessage(ReplaceWithValue{.value = 37}));

    EXPECT_TRUE(result.diagnostics.empty());
    EXPECT_TRUE(result.project_state_changed);
    auto const replaced = project_graph.current_generation();
    ASSERT_TRUE(replaced);
    EXPECT_GT(replaced->generation, before->generation);
    ASSERT_EQ(replaced->node_replacements.size(), 1u);
    EXPECT_EQ(
        replaced->node_replacements[0].definition_id,
        successor_definition_id);
    EXPECT_EQ(
        nlohmann::ordered_json::parse(
            replaced->node_replacements[0].configuration_arguments_json),
        nlohmann::ordered_json::array({67}));
    ASSERT_TRUE(replaced->placements.contains("instance:replaceable"));
    EXPECT_EQ(
        replaced->placements.at("instance:replaceable")
            .configured->definition_id,
        successor_definition_id);
    EXPECT_EQ(successor_value.load(), 67);

    iv::ProjectAckBuilder update_builder;
    project_graph.handle_project_update_node_instances(
        iv::ProjectUpdateNodeInstancesRequest{
            .updates = {{
                .instance_id = "instance:replaceable",
                .display_name = "Renamed after replacement",
            }},
        },
        update_builder);
    EXPECT_NO_THROW(update_builder.build());
    auto const after_unrelated_rebuild = project_graph.current_generation();
    ASSERT_TRUE(after_unrelated_rebuild);
    ASSERT_EQ(after_unrelated_rebuild->node_replacements.size(), 1u);
    EXPECT_EQ(
        after_unrelated_rebuild->placements.at("instance:replaceable")
            .configured->definition_id,
        successor_definition_id);
    EXPECT_EQ(successor_value.load(), 67);
}

TEST_F(ProjectGraphFixture, EventProtocolListsPackageSchemasAndActiveAddressSupport)
{
    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{
            .snapshot = make_protocol_snapshot(9),
        });
    iv::ProjectStringBuilder create_builder;
    project_graph.handle_project_create_node_instance(
        iv::ProjectCreateNodeInstanceRequest{
            .instance_id = "instance:protocol",
            .definition_id = replacing_definition_id,
            .configuration_source = "30",
        },
        create_builder);
    ASSERT_EQ(create_builder.build(), "instance:protocol");

    auto const protocol = project_graph.node_event_protocol();
    EXPECT_EQ(protocol.format_version, 0u);
    EXPECT_EQ(protocol.definitions_generation, 9u);
    EXPECT_EQ(
        protocol.project_graph_generation,
        project_graph.current_generation()->generation);
    ASSERT_EQ(protocol.event_types.size(), 1u);
    auto const& event = protocol.event_types.front();
    EXPECT_EQ(event.id.nominal_id, protocol_event_nominal_id);
    EXPECT_EQ(event.id.definition_fingerprint, protocol_event_fingerprint);
    EXPECT_EQ(event.display_name, "ReplaceWithValue");
    EXPECT_EQ(event.payload.kind, iv::NodeEventValueKind::aggregate);
    ASSERT_EQ(event.payload.fields.size(), 1u);
    EXPECT_EQ(event.payload.fields[0].name, "value");
    ASSERT_NE(event.payload.fields[0].value, nullptr);
    EXPECT_EQ(
        event.payload.fields[0].value->kind,
        iv::NodeEventValueKind::signed_integer);

    ASSERT_EQ(protocol.definitions.size(), 2u);
    EXPECT_EQ(
        protocol.definitions[0].definition_id,
        replacing_definition_id);
    ASSERT_EQ(protocol.definitions[0].supported_event_types.size(), 1u);
    EXPECT_EQ(protocol.definitions[0].supported_event_types[0], event.id);
    EXPECT_EQ(
        protocol.definitions[1].definition_id,
        successor_definition_id);
    ASSERT_EQ(protocol.definitions[1].supported_event_types.size(), 1u);
    EXPECT_EQ(protocol.definitions[1].supported_event_types[0], event.id);

    ASSERT_EQ(protocol.receivers.size(), 1u);
    EXPECT_EQ(protocol.receivers[0].address.instance_id, "instance:protocol");
    EXPECT_TRUE(protocol.receivers[0].address.path.empty());
    ASSERT_EQ(protocol.receivers[0].supported_event_types.size(), 1u);
    EXPECT_EQ(protocol.receivers[0].supported_event_types[0], event.id);
}

TEST_F(ProjectGraphFixture, FailedReplacementPreviewDoesNotCommitReplacement)
{
    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{
            .snapshot = make_replacement_snapshot(1),
        });
    iv::ProjectStringBuilder create_builder;
    project_graph.handle_project_create_node_instance(
        iv::ProjectCreateNodeInstanceRequest{
            .instance_id = "instance:replaceable",
            .definition_id = replacing_definition_id,
            .configuration_source = "30",
        },
        create_builder);
    ASSERT_EQ(create_builder.build(), "instance:replaceable");
    auto const before = project_graph.current_generation();
    ASSERT_TRUE(before);

    EXPECT_THROW(
        (void)project_graph.dispatch_node_message(
            iv::NodeAddress{.instance_id = "instance:replaceable"},
            iv::OwnedNodeMessage(ReplaceWithoutArguments{})),
        std::runtime_error);

    EXPECT_EQ(project_graph.current_generation(), before);
    EXPECT_TRUE(project_graph.current_generation()->node_replacements.empty());
    EXPECT_TRUE(instances.desired_node_replacements().empty());
    EXPECT_EQ(successor_configure_calls.load(), 0);
}

TEST_F(ProjectGraphFixture, ReplacementThenSendUsesTheSuccessorHandler)
{
    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{
            .snapshot = make_replacement_snapshot(1),
        });
    iv::ProjectStringBuilder create_builder;
    project_graph.handle_project_create_node_instance(
        iv::ProjectCreateNodeInstanceRequest{
            .instance_id = "instance:replaceable",
            .definition_id = replacing_definition_id,
            .configuration_source = "30",
        },
        create_builder);
    ASSERT_EQ(create_builder.build(), "instance:replaceable");

    auto result = project_graph.dispatch_node_message(
        iv::NodeAddress{.instance_id = "instance:replaceable"},
        iv::OwnedNodeMessage(ReplaceThenSend{.value = 12}));

    EXPECT_TRUE(result.diagnostics.empty());
    EXPECT_TRUE(result.project_state_changed);
    EXPECT_EQ(successor_message_calls.load(), 1);
    EXPECT_EQ(successor_message_value.load(), 42);
    ASSERT_EQ(project_graph.current_generation()->node_replacements.size(), 1u);
    EXPECT_EQ(
        project_graph.current_generation()->placements
            .at("instance:replaceable")
            .configured->definition_id,
        successor_definition_id);
}

TEST_F(ProjectGraphFixture, OneInteractionRejectsASecondDeliveryToTheSameNode)
{
    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{
            .snapshot = make_replacement_snapshot(1),
        });
    iv::ProjectStringBuilder create_builder;
    project_graph.handle_project_create_node_instance(
        iv::ProjectCreateNodeInstanceRequest{
            .instance_id = "instance:replaceable",
            .definition_id = replacing_definition_id,
            .configuration_source = "30",
        },
        create_builder);
    ASSERT_EQ(create_builder.build(), "instance:replaceable");
    auto const before = project_graph.current_generation();

    EXPECT_THROW(
        (void)project_graph.dispatch_node_message(
            iv::NodeAddress{.instance_id = "instance:replaceable"},
            iv::OwnedNodeMessage(RepeatDelivery{})),
        std::runtime_error);

    EXPECT_EQ(project_graph.current_generation(), before);
    EXPECT_TRUE(project_graph.current_generation()->node_replacements.empty());
}

TEST_F(ProjectGraphFixture, ModuleHandlerSendsToACapturedNodeRef)
{
    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{
            .snapshot = make_replacement_snapshot(1),
        });
    iv::ProjectStringBuilder create_builder;
    project_graph.handle_project_create_node_instance(
        iv::ProjectCreateNodeInstanceRequest{
            .instance_id = "instance:sender",
            .definition_id = sender_definition_id,
        },
        create_builder);
    ASSERT_EQ(create_builder.build(), "instance:sender");

    auto result = project_graph.dispatch_node_message(
        iv::NodeAddress{.instance_id = "instance:sender"},
        iv::OwnedNodeMessage(SendToReceiver{.value = 91}));

    EXPECT_TRUE(result.diagnostics.empty())
        << (result.diagnostics.empty()
                ? std::string{}
                : result.diagnostics.front().message);
    EXPECT_FALSE(result.project_state_changed);
    EXPECT_EQ(receiver_message_calls.load(), 1);
    EXPECT_EQ(receiver_message_value.load(), 91);
}

TEST_F(ProjectGraphFixture, SerializedModuleMessageSendsToACapturedNodeRef)
{
    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{
            .snapshot = make_serialized_sender_snapshot(1),
        });
    iv::ProjectStringBuilder create_builder;
    project_graph.handle_project_create_node_instance(
        iv::ProjectCreateNodeInstanceRequest{
            .instance_id = "instance:sender",
            .definition_id = sender_definition_id,
        },
        create_builder);
    ASSERT_EQ(create_builder.build(), "instance:sender");

    auto result = project_graph.dispatch_node_message(
        iv::NodeAddress{.instance_id = "instance:sender"},
        iv::SerializedNodeMessage{
            .event_type_nominal_id = "iv.test.SendToReceiver",
            .event_type_definition_fingerprint =
                "iv.test.SendToReceiver:v1",
            .payload = std::make_shared<nlohmann::ordered_json const>(
                nlohmann::ordered_json{{"value", 73}}),
        });

    EXPECT_TRUE(result.diagnostics.empty())
        << (result.diagnostics.empty()
                ? std::string{}
                : result.diagnostics.front().message);
    EXPECT_FALSE(result.project_state_changed);
    EXPECT_EQ(receiver_message_calls.load(), 1);
    EXPECT_EQ(receiver_message_value.load(), 73);
}

TEST_F(ProjectGraphFixture, ModuleMessageSelectorsReachOneTiledConcreteReceiver)
{
    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{
            .snapshot = make_replacement_snapshot(1),
        });
    iv::ProjectStringBuilder create_builder;
    project_graph.handle_project_create_node_instance(
        iv::ProjectCreateNodeInstanceRequest{
            .instance_id = "instance:selector-sender",
            .definition_id = selector_sender_definition_id,
        },
        create_builder);
    ASSERT_EQ(create_builder.build(), "instance:selector-sender");

    auto result = project_graph.dispatch_node_message(
        iv::NodeAddress{.instance_id = "instance:selector-sender"},
        iv::OwnedNodeMessage(SendToSelectedReceiver{.value = 64}));

    EXPECT_TRUE(result.diagnostics.empty())
        << (result.diagnostics.empty()
                ? std::string{}
                : result.diagnostics.front().message);
    EXPECT_EQ(receiver_message_calls.load(), 1);
    EXPECT_EQ(receiver_message_value.load(), 64);
}

TEST_F(ProjectGraphFixture, AncestorReplacementCreatesANewDescendantDeliveryWorld)
{
    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{
            .snapshot = make_replacement_snapshot(1),
        });
    iv::ProjectStringBuilder create_builder;
    project_graph.handle_project_create_node_instance(
        iv::ProjectCreateNodeInstanceRequest{
            .instance_id = "instance:sender",
            .definition_id = sender_definition_id,
        },
        create_builder);
    ASSERT_EQ(create_builder.build(), "instance:sender");

    auto result = project_graph.dispatch_node_message(
        iv::NodeAddress{.instance_id = "instance:sender"},
        iv::OwnedNodeMessage(ReplaceAncestorThenRepeat{}));

    EXPECT_TRUE(result.diagnostics.empty());
    EXPECT_TRUE(result.project_state_changed);
    EXPECT_EQ(receiver_message_calls.load(), 2);
    EXPECT_EQ(receiver_message_value.load(), 2);
    ASSERT_EQ(project_graph.current_generation()->node_replacements.size(), 1u);
    EXPECT_EQ(
        project_graph.current_generation()->node_replacements[0].address,
        (iv::NodeAddress{.instance_id = "instance:sender"}));
}

TEST_F(ProjectGraphFixture, UnrelatedReplacementDoesNotResetReceiverDelivery)
{
    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{
            .snapshot = make_replacement_snapshot(1),
        });
    for (auto const& [instance, definition] : std::array{
             std::pair{"instance:sender", sender_definition_id},
             std::pair{"instance:receiver", receiver_definition_id},
         }) {
        iv::ProjectStringBuilder create_builder;
        project_graph.handle_project_create_node_instance(
            iv::ProjectCreateNodeInstanceRequest{
                .instance_id = instance,
                .definition_id = definition,
            },
            create_builder);
        ASSERT_EQ(create_builder.build(), instance);
    }
    auto const before = project_graph.current_generation();

    EXPECT_THROW(
        (void)project_graph.dispatch_node_message(
            iv::NodeAddress{.instance_id = "instance:sender"},
            iv::OwnedNodeMessage(ReplaceUnrelatedThenRepeat{
                .target = {.instance_id = "instance:receiver"},
            })),
        std::runtime_error);

    EXPECT_EQ(receiver_message_calls.load(), 1);
    EXPECT_EQ(project_graph.current_generation(), before);
    EXPECT_TRUE(project_graph.current_generation()->node_replacements.empty());
}

TEST_F(ProjectGraphFixture, SuccessiveReplacementsReplaceTheCompletePriorRecord)
{
    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{
            .snapshot = make_replacement_snapshot(1),
        });
    iv::ProjectStringBuilder create_builder;
    project_graph.handle_project_create_node_instance(
        iv::ProjectCreateNodeInstanceRequest{
            .instance_id = "instance:replaceable",
            .definition_id = replacing_definition_id,
            .configuration_source = "30",
        },
        create_builder);
    ASSERT_EQ(create_builder.build(), "instance:replaceable");

    auto first = project_graph.dispatch_node_message(
        iv::NodeAddress{.instance_id = "instance:replaceable"},
        iv::OwnedNodeMessage(ReplaceWithValue{.value = 12}));
    ASSERT_TRUE(first.project_state_changed);

    auto second = project_graph.dispatch_node_message(
        iv::NodeAddress{.instance_id = "instance:replaceable"},
        iv::OwnedNodeMessage(ReplaceWithValue{.value = 99}));

    EXPECT_TRUE(second.project_state_changed);
    auto const generation = project_graph.current_generation();
    ASSERT_EQ(generation->node_replacements.size(), 1u);
    EXPECT_EQ(
        nlohmann::ordered_json::parse(
            generation->node_replacements[0].configuration_arguments_json),
        nlohmann::ordered_json::array({99}));
    EXPECT_EQ(successor_value.load(), 99);
}

TEST_F(ProjectGraphFixture, ProjectGraphDispatchesToALeafHandler)
{
    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{
            .snapshot = make_replacement_snapshot(1),
        });
    iv::ProjectStringBuilder create_builder;
    project_graph.handle_project_create_node_instance(
        iv::ProjectCreateNodeInstanceRequest{
            .instance_id = "instance:leaf",
            .definition_id = leaf_receiver_definition_id,
        },
        create_builder);
    ASSERT_EQ(create_builder.build(), "instance:leaf");

    auto result = project_graph.dispatch_node_message(
        iv::NodeAddress{.instance_id = "instance:leaf"},
        iv::OwnedNodeMessage(LeafMessage{.value = 57}));

    EXPECT_TRUE(result.diagnostics.empty());
    EXPECT_FALSE(result.project_state_changed);
    EXPECT_EQ(leaf_message_calls.load(), 1);
    EXPECT_EQ(leaf_message_value.load(), 57);
}

TEST_F(ProjectGraphFixture, StaleDefinitionsSnapshotCannotRollBackRootGeneration)
{
    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{.snapshot = make_snapshot(2)});
    auto const current = project_graph.current_generation();
    ASSERT_TRUE(current);

    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{.snapshot = make_snapshot(1)});
    auto const after_stale = project_graph.current_generation();
    ASSERT_TRUE(after_stale);
    EXPECT_EQ(after_stale->generation, current->generation);
    EXPECT_EQ(after_stale->definitions_generation, 2u);
}

TEST(ProjectGraphBridge, NodeDefinitionsSnapshotFlowsThroughProjectGraphBeforeNodeInstances)
{
    iv::NodeDefinitions definitions;
    iv::ProjectGraph project_graph;
    iv::NodeInstances instances;
    iv::GraphConnections connections;
    auto definitions_scope = iv::node_definitions_project_graph_bridge::bind(
        definitions, project_graph);
    auto instances_scope = iv::project_graph_node_instances_bridge::bind(
        project_graph, instances);
    auto connections_scope = iv::project_graph_graph_connections_bridge::bind(
        project_graph, connections);

    IV_INVOKE_LINKER_EVENT(
        iv::iv_runtime_node_definitions_snapshot_changed_event,
        iv::NodeDefinitionsSnapshotChanged{.snapshot = make_snapshot(7)});

    auto const generation = project_graph.current_generation();
    ASSERT_TRUE(generation);
    EXPECT_EQ(generation->definitions_generation, 7u);
    EXPECT_EQ(instances.configuration_cache_size(), 0u);
}

TEST(ProjectGraph, FailedRootBuildDoesNotCommitDefinitionsAndSameSnapshotCanRetry)
{
    iv::ProjectGraph project_graph;
    iv::NodeInstances instances;
    auto instances_scope = iv::project_graph_node_instances_bridge::bind(
        project_graph, instances);
    auto snapshot = make_snapshot(1);
    EXPECT_THROW(
        project_graph.handle_node_definitions_snapshot_changed(
            iv::NodeDefinitionsSnapshotChanged{.snapshot = snapshot}),
        std::runtime_error);
    EXPECT_FALSE(project_graph.current_generation());
    EXPECT_EQ(project_graph.definitions_snapshot()->generation, 0u);

    iv::GraphConnections connections;
    auto connections_scope = iv::project_graph_graph_connections_bridge::bind(
        project_graph, connections);
    EXPECT_NO_THROW(project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{.snapshot = snapshot}));
    auto generation = project_graph.current_generation();
    ASSERT_TRUE(generation);
    EXPECT_EQ(generation->definitions_generation, 1u);
}

TEST_F(ProjectGraphFixture, InvalidUpdateBatchDoesNotPartiallyMutateDesiredState)
{
    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{.snapshot = make_snapshot(1)});
    iv::ProjectStringBuilder create_builder;
    project_graph.handle_project_create_node_instance(
        iv::ProjectCreateNodeInstanceRequest{
            .instance_id = "instance:a",
            .definition_id = definition_id,
            .display_name = "Original",
        },
        create_builder);
    (void)create_builder.build();
    auto const before = project_graph.current_generation();
    ASSERT_TRUE(before);

    iv::ProjectAckBuilder update_builder;
    EXPECT_THROW(
        project_graph.handle_project_update_node_instances(
            iv::ProjectUpdateNodeInstancesRequest{
                .updates = {
                    {.instance_id = "instance:a", .display_name = "Changed"},
                    {.instance_id = "instance:missing", .display_name = "Invalid"},
                },
            },
            update_builder),
        std::runtime_error);

    auto listed = instances.list_instances();
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed.front().display_name, "Original");
    EXPECT_EQ(project_graph.current_generation()->generation, before->generation);
}


TEST_F(ProjectGraphFixture, ConnectionMutationRebuildsWholeRootAndDanglingIntentRecovers)
{
    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{.snapshot = make_snapshot(1)});

    for (auto const* instance_id : {"instance:source", "instance:sink"}) {
        iv::ProjectStringBuilder builder;
        project_graph.handle_project_create_node_instance(
            iv::ProjectCreateNodeInstanceRequest{
                .instance_id = std::string(instance_id),
                .definition_id = definition_id,
            },
            builder);
        EXPECT_EQ(builder.build(), instance_id);
    }

    iv::ProjectAckBuilder connect_builder;
    project_graph.handle_project_upsert_graph_connection(
        iv::ProjectUpsertGraphConnectionRequest{
            .connection = iv::ProjectSampleConnection{
                .connection_id = "connection:main",
                .outputs = {{
                    .instance_id = "instance:source",
                    .port = {.name = "main"},
                }},
                .inputs = {{
                    .instance_id = "instance:sink",
                    .port = {.name = "main"},
                }},
            },
        },
        connect_builder);
    EXPECT_NO_THROW(connect_builder.build());
    auto generation = project_graph.current_generation();
    ASSERT_TRUE(generation);
    EXPECT_EQ(generation->applied_connection_ids,
              std::vector<std::string>{"connection:main"});
    EXPECT_TRUE(generation->connection_diagnostics.empty());
    ASSERT_EQ(connections.desired_connections().size(), 1u);

    iv::ProjectAckBuilder delete_builder;
    project_graph.handle_project_delete_node_instance(
        iv::ProjectDeleteNodeInstanceRequest{.instance_id = "instance:sink"},
        delete_builder);
    EXPECT_NO_THROW(delete_builder.build());
    generation = project_graph.current_generation();
    ASSERT_TRUE(generation);
    EXPECT_TRUE(generation->applied_connection_ids.empty());
    ASSERT_EQ(generation->connection_diagnostics.size(), 1u);
    EXPECT_EQ(connections.desired_connections().size(), 1u);

    iv::ProjectStringBuilder recreate_builder;
    project_graph.handle_project_create_node_instance(
        iv::ProjectCreateNodeInstanceRequest{
            .instance_id = "instance:sink",
            .definition_id = definition_id,
        },
        recreate_builder);
    EXPECT_EQ(recreate_builder.build(), "instance:sink");
    generation = project_graph.current_generation();
    ASSERT_TRUE(generation);
    EXPECT_EQ(generation->applied_connection_ids,
              std::vector<std::string>{"connection:main"});
    EXPECT_TRUE(generation->connection_diagnostics.empty());
}

TEST_F(ProjectGraphFixture, ReplaceGraphReplaysNodesAndConnectionsInOneGeneration)
{
    iv::ProjectAckBuilder builder;
    project_graph.handle_project_replace_graph(
        iv::ProjectReplaceGraphRequest{
            .instances = {
                {
                    .instance_id = "instance:source",
                    .definition_id = definition_id,
                    .package_root = std::filesystem::path(package_root),
                    .display_name = "Source",
                },
                {
                    .instance_id = "instance:target",
                    .definition_id = definition_id,
                    .package_root = std::filesystem::path(package_root),
                    .configuration_source = "",
                },
            },
            .connections = {iv::ProjectSampleConnection{
                .connection_id = "connection:main",
                .outputs = {{
                    .instance_id = "instance:source",
                    .port = {.name = "main"},
                }},
                .inputs = {{
                    .instance_id = "instance:target",
                    .port = {.name = "main"},
                }},
            }},
        },
        builder);
    EXPECT_NO_THROW(builder.build());

    // No definition is available yet, so the pass fails; the replaced
    // instances and connection are still the desired revision.
    EXPECT_FALSE(project_graph.current_generation());
    auto const failure = project_graph.configuration_failure();
    ASSERT_TRUE(failure);
    EXPECT_EQ(failure->node_instance_diagnostics.size(), 2u);
    ASSERT_EQ(instances.desired_instances().size(), 2u);
    ASSERT_EQ(connections.desired_connections().size(), 1u);

    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{.snapshot = make_snapshot(1)});
    auto generation = project_graph.current_generation();
    ASSERT_TRUE(generation);
    EXPECT_EQ(generation->generation, 1u);
    EXPECT_EQ(generation->placements.size(), 2u);
    EXPECT_EQ(
        generation->applied_connection_ids,
        std::vector<std::string>{"connection:main"});

    iv::ProjectAckBuilder replacement_builder;
    project_graph.handle_project_replace_graph(
        iv::ProjectReplaceGraphRequest{},
        replacement_builder);
    EXPECT_NO_THROW(replacement_builder.build());
    EXPECT_TRUE(instances.desired_instances().empty());
    EXPECT_TRUE(connections.desired_connections().empty());
    EXPECT_EQ(project_graph.current_generation()->generation, 2u);
}

TEST_F(ProjectGraphFixture,
       ManualControlsResolveStableAddressesAndInputNamesPerGeneration)
{
    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{
            .snapshot = make_replacement_snapshot(1),
        });

    iv::ProjectAckBuilder builder;
    project_graph.handle_project_replace_graph(
        iv::ProjectReplaceGraphRequest{
            .instances = {{
                .instance_id = "instance:manual",
                .definition_id = manual_control_leaf_definition_id,
            }},
            .manual_controls = {
                iv::ProjectManualControl{
                    .id = {
                        .node = {.instance_id = "instance:manual"},
                        .input_name = "gain",
                    },
                    .stored_value = iv::Sample{0.625f},
                    .participation = iv::ManualControlParticipation::
                        participate_with_connections,
                },
                iv::ProjectManualControl{
                    .id = {
                        .node = {.instance_id = "instance:manual"},
                        .input_name = "missing",
                    },
                    .stored_value = iv::Sample{0.25f},
                },
            },
        },
        builder);
    EXPECT_NO_THROW(builder.build());

    auto const generation = project_graph.current_generation();
    ASSERT_TRUE(generation);
    ASSERT_EQ(generation->manual_controls.size(), 2u);
    ASSERT_EQ(generation->graph->manual_controls.size(), 1u);
    auto const& configured = generation->graph->manual_controls.front();
    EXPECT_EQ(configured.id.input_name, "gain");
    EXPECT_FLOAT_EQ(static_cast<float>(configured.settled_value), 0.625f);
    EXPECT_EQ(
        configured.participation,
        iv::ManualControlParticipation::participate_with_connections);
    EXPECT_FALSE(configured.dynamic);
    EXPECT_EQ(
        generation->graph->node_bundles
            .resolve_sample_input(configured.input)
            .config.name,
        "gain");
    ASSERT_EQ(generation->manual_control_diagnostics.size(), 1u);
    EXPECT_EQ(
        generation->manual_control_diagnostics.front().id.input_name,
        "missing");
}

TEST_F(ProjectGraphManualControlSchedulingFixture,
       HoverCompilationDoesNotActivateAndInteractionRejectsStaleResult)
{
    auto const controls = realize_controls(2);
    ASSERT_EQ(controls.size(), 2u);
    auto const baseline_compilations = probe.compilation_count();
    EXPECT_TRUE(probe.stages().empty());

    probe.block_next_compilation();
    iv::ProjectAckBuilder hover_first;
    project_graph.handle_project_set_manual_control_hover(
        iv::ProjectSetManualControlHoverRequest{
            .client_id = "client:hover",
            .id = controls[0],
        },
        hover_first);
    EXPECT_NO_THROW(hover_first.build());
    EXPECT_TRUE(probe.wait_for_blocked_compilation());

    // Replacing the hover invalidates the in-flight result for controls[0].
    iv::ProjectAckBuilder hover_second;
    project_graph.handle_project_set_manual_control_hover(
        iv::ProjectSetManualControlHoverRequest{
            .client_id = "client:hover",
            .id = controls[1],
        },
        hover_second);
    EXPECT_NO_THROW(hover_second.build());
    probe.release_blocked_compilation();

    ASSERT_TRUE(probe.wait_for_compilations(baseline_compilations + 2));
    auto const compilations = probe.compilations();
    ASSERT_GE(compilations.size(), baseline_compilations + 2);
    EXPECT_EQ(
        compilations[baseline_compilations].dynamic_inputs,
        std::vector<iv::ManualControlId>{controls[0]});
    EXPECT_EQ(
        compilations[baseline_compilations + 1].dynamic_inputs,
        std::vector<iv::ManualControlId>{controls[1]});
    EXPECT_TRUE(probe.stages().empty());

    // Beginning the matching interaction either consumes already-ready hover
    // code or upgrades its in-flight request to activate when ready.
    auto const interaction = iv::ManualControlInteractionId{
        .client_id = "client:hover",
        .interaction_id = "pointer:1",
    };
    iv::ProjectAckBuilder begin;
    project_graph.handle_project_begin_manual_control_interaction(
        iv::ProjectBeginManualControlInteractionRequest{
            .interaction = interaction,
            .id = controls[1],
            .value = iv::Sample{0.25f},
        },
        begin);
    ASSERT_NO_THROW(begin.build());

    ASSERT_TRUE(probe.wait_for_stages(1));
    auto const stages = probe.stages();
    ASSERT_EQ(stages.size(), 1u);
    EXPECT_EQ(
        stages.front().dynamic_inputs,
        std::vector<iv::ManualControlId>{controls[1]});
}

TEST_F(ProjectGraphManualControlSchedulingFixture,
       EndingOrDisconnectingRejectsDynamicResultAndRequestsConstantCode)
{
    auto const controls = realize_controls(1);
    ASSERT_EQ(controls.size(), 1u);
    auto const& control = controls.front();

    auto verify_settlement = [&](iv::ManualControlInteractionId interaction,
                                 bool disconnect) {
        auto const baseline_compilations = probe.compilation_count();
        auto const baseline_stages = probe.stages().size();
        probe.block_next_compilation();

        iv::ProjectAckBuilder begin;
        project_graph.handle_project_begin_manual_control_interaction(
            iv::ProjectBeginManualControlInteractionRequest{
                .interaction = interaction,
                .id = control,
                .value = iv::Sample{0.5f},
            },
            begin);
        EXPECT_NO_THROW(begin.build());
        EXPECT_TRUE(probe.wait_for_blocked_compilation());

        if (disconnect) {
            project_graph.handle_socket_rpc_client_disconnected(
                interaction.client_id);
        } else {
            iv::ProjectAckBuilder end;
            project_graph.handle_project_end_manual_control_interaction(
                iv::ProjectEndManualControlInteractionRequest{
                    .interaction = interaction,
                    .value = iv::Sample{0.75f},
                },
                end);
            EXPECT_NO_THROW(end.build());
        }
        probe.release_blocked_compilation();

        EXPECT_TRUE(probe.wait_for_stages(baseline_stages + 1));
        auto const compilations = probe.compilations();
        ASSERT_GE(compilations.size(), baseline_compilations + 2);
        EXPECT_EQ(
            compilations[baseline_compilations].dynamic_inputs,
            std::vector<iv::ManualControlId>{control});
        EXPECT_TRUE(
            compilations[baseline_compilations + 1].dynamic_inputs.empty());

        auto const stages = probe.stages();
        ASSERT_EQ(stages.size(), baseline_stages + 1);
        EXPECT_TRUE(stages.back().dynamic_inputs.empty());
    };

    verify_settlement(
        iv::ManualControlInteractionId{
            .client_id = "client:end",
            .interaction_id = "pointer:1",
        },
        false);
    verify_settlement(
        iv::ManualControlInteractionId{
            .client_id = "client:disconnect",
            .interaction_id = "pointer:2",
        },
        true);
}

TEST_F(ProjectGraphFixture,
       DisconnectInvalidatesClientOwnedManualControlInteraction)
{
    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{
            .snapshot = make_replacement_snapshot(1),
        });
    auto const control_id = iv::ManualControlId{
        .node = {.instance_id = "instance:manual"},
        .input_name = "gain",
    };
    iv::ProjectAckBuilder replacement;
    project_graph.handle_project_replace_graph(
        iv::ProjectReplaceGraphRequest{
            .instances = {{
                .instance_id = "instance:manual",
                .definition_id = manual_control_leaf_definition_id,
            }},
            .manual_controls = {{.id = control_id}},
        },
        replacement);
    ASSERT_NO_THROW(replacement.build());

    auto const interaction = iv::ManualControlInteractionId{
        .client_id = "socket:17",
        .interaction_id = "pointer:3",
    };
    iv::ProjectAckBuilder begin;
    project_graph.handle_project_begin_manual_control_interaction(
        iv::ProjectBeginManualControlInteractionRequest{
            .interaction = interaction,
            .id = control_id,
            .value = iv::Sample{0.25f},
        },
        begin);
    ASSERT_NO_THROW(begin.build());

    project_graph.handle_socket_rpc_client_disconnected("socket:17");

    // The disconnected client's interaction identity is no longer active.
    iv::ProjectAckBuilder update;
    EXPECT_THROW(
        project_graph.handle_project_update_manual_control_interaction(
            iv::ProjectUpdateManualControlInteractionRequest{
                .interaction = interaction,
                .value = iv::Sample{0.5f},
            },
            update),
        std::invalid_argument);

    iv::ProjectAckBuilder begin_again;
    project_graph.handle_project_begin_manual_control_interaction(
        iv::ProjectBeginManualControlInteractionRequest{
            .interaction = interaction,
            .id = control_id,
            .value = iv::Sample{0.75f},
        },
        begin_again);
    EXPECT_NO_THROW(begin_again.build());

    iv::ProjectAckBuilder remove_target;
    project_graph.handle_project_delete_node_instance(
        iv::ProjectDeleteNodeInstanceRequest{
            .instance_id = "instance:manual",
        },
        remove_target);
    ASSERT_NO_THROW(remove_target.build());

    iv::ProjectAckBuilder update_after_removal;
    EXPECT_THROW(
        project_graph.handle_project_update_manual_control_interaction(
            iv::ProjectUpdateManualControlInteractionRequest{
                .interaction = interaction,
                .value = iv::Sample{1.0f},
            },
            update_after_removal),
        std::invalid_argument);

    iv::ProjectAckBuilder begin_after_removal;
    EXPECT_THROW(
        project_graph.handle_project_begin_manual_control_interaction(
            iv::ProjectBeginManualControlInteractionRequest{
                .interaction = interaction,
                .id = control_id,
                .value = iv::Sample{1.0f},
            },
            begin_after_removal),
        std::invalid_argument);
}

TEST_F(ProjectGraphFixture, InvalidReplacementDoesNotChangeEitherDesiredOwner)
{
    iv::ProjectStringBuilder create_builder;
    project_graph.handle_project_create_node_instance(
        iv::ProjectCreateNodeInstanceRequest{
            .instance_id = "instance:existing",
            .definition_id = definition_id,
            .package_root = std::filesystem::path(package_root),
        },
        create_builder);
    EXPECT_EQ(create_builder.build(), "instance:existing");
    auto const generation_before = project_graph.current_generation();
    auto const instances_before = instances.desired_instances();
    auto const connections_before = connections.desired_connections();

    iv::ProjectAckBuilder builder;
    EXPECT_THROW(
        project_graph.handle_project_replace_graph(
            iv::ProjectReplaceGraphRequest{
                .instances = {{
                    .instance_id = "instance:replacement",
                    .definition_id = definition_id,
                    .package_root = std::filesystem::path(package_root),
                }},
                .connections = {iv::ProjectEventConnection{
                    .connection_id = "event:invalid",
                    .source_type = iv::EventTypeId::midi,
                    .outputs = {{
                        .instance_id = "instance:replacement",
                        .port = {.name = "events", .channel = 0},
                    }},
                    .target_type = iv::EventTypeId::midi,
                    .inputs = {{
                        .instance_id = "instance:replacement",
                        .port = {.name = "events"},
                    }},
                }},
            },
            builder),
        std::invalid_argument);

    EXPECT_EQ(project_graph.current_generation(), generation_before);
    EXPECT_EQ(instances.desired_instances(), instances_before);
    EXPECT_EQ(connections.desired_connections(), connections_before);
}
