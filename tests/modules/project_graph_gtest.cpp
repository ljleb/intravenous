#include <intravenous/dsl.h>
#include <intravenous/module/package_definitions.h>
#include <intravenous/runtime/graph_connections.h>
#include <intravenous/runtime/node_definitions.h>
#include <intravenous/runtime/node_definitions_events.h>
#include <intravenous/runtime/node_definitions_project_graph_bridge.h>
#include <intravenous/runtime/node_instances.h>
#include <intravenous/runtime/package_pipeline_types.h>
#include <intravenous/runtime/project_graph.h>
#include <intravenous/runtime/project_graph_graph_connections_bridge.h>
#include <intravenous/runtime/project_graph_node_instances_bridge.h>
#include <intravenous/runtime/runtime_project_events.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>

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
    EXPECT_TRUE(generation->diagnostics.empty());
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

TEST_F(ProjectGraphFixture, MissingDefinitionRemainsDesiredAndRealizesWhenDefinitionArrives)
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

    auto generation = project_graph.current_generation();
    ASSERT_TRUE(generation);
    EXPECT_TRUE(generation->placements.empty());
    ASSERT_EQ(generation->diagnostics.size(), 1u);
    EXPECT_EQ(generation->diagnostics.front().instance_id, "instance:pending");
    auto listed = instances.list_instances();
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_FALSE(listed.front().realized);

    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{.snapshot = make_snapshot(1)});
    generation = project_graph.current_generation();
    ASSERT_TRUE(generation->placements.contains("instance:pending"));
    EXPECT_TRUE(generation->diagnostics.empty());
    listed = instances.list_instances();
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_TRUE(listed.front().realized);
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
    EXPECT_EQ(protocol.format_version, 1u);
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

    auto generation = project_graph.current_generation();
    ASSERT_TRUE(generation);
    EXPECT_EQ(generation->generation, 1u);
    EXPECT_TRUE(generation->placements.empty());
    ASSERT_EQ(instances.desired_instances().size(), 2u);
    ASSERT_EQ(connections.desired_connections().size(), 1u);

    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{.snapshot = make_snapshot(1)});
    generation = project_graph.current_generation();
    ASSERT_TRUE(generation);
    EXPECT_EQ(generation->generation, 2u);
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
    EXPECT_EQ(project_graph.current_generation()->generation, 3u);
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
