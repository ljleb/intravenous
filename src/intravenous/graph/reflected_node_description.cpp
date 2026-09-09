#include <intravenous/graph/reflected_node_description.h>

#include <stdexcept>
#include <utility>

namespace iv::details {
class NodeDescriptionBuilder {
    ReflectedNodeDescription& _description;

public:
    explicit NodeDescriptionBuilder(ReflectedNodeDescription& description)
        : _description(description)
    {}

    NodeDescriptionSink sink() const
    {
        return NodeDescriptionSink(std::addressof(_description));
    }
};

ReflectedNodeDescription& NodeDescriptionSink::description() const
{
    if (!_description) {
        throw std::logic_error("node description sink is not initialized");
    }
    return *static_cast<ReflectedNodeDescription*>(_description);
}

void NodeDescriptionSink::add_sample_input(InputConfig const& input) const
{
    description().ports.sample_inputs.push_back(input);
}

void NodeDescriptionSink::add_sample_output(OutputConfig const& output) const
{
    description().ports.sample_outputs.push_back(output);
}

void NodeDescriptionSink::add_event_input(EventInputConfig const& input) const
{
    description().ports.event_input_configs.push_back(input);
}

void NodeDescriptionSink::add_event_output(EventOutputConfig const& output) const
{
    description().ports.event_output_configs.push_back(output);
}

void NodeDescriptionSink::set_internal_latency(std::size_t value) const
{
    description().internal_latency_samples = value;
}

void NodeDescriptionSink::set_maximum_block_size(std::size_t value) const
{
    description().maximum_block_size = value;
}

void NodeDescriptionSink::set_default_ttl(std::optional<std::size_t> value) const
{
    description().default_ttl_samples = value;
}

void NodeDescriptionSink::set_block_skippable(bool value) const
{
    description().block_skippable = value;
}

void NodeDescriptionSink::set_static_sample_value(std::optional<Sample> value) const
{
    description().static_sample_value = value;
}

ReflectedNodeDescription materialize_node_description(
    NodeBuildRequest const& request,
    std::shared_ptr<void const> storage,
    NodeConfigStringRelocations relocations)
{
    if (!request.compiler_record || !request.config || !request.describe
        || request.config_size == 0 || request.config_alignment == 0 || !storage) {
        throw std::invalid_argument("invalid node build request");
    }

    auto const& record = *request.compiler_record;
    if (!record.type_name && record.type_name_size != 0) {
        throw std::invalid_argument("node compiler record has an invalid type name");
    }

    ReflectedNodeDescription result {
        .operations = {.runtime = record.runtime},
        .node_storage = std::move(storage),
        .config_string_relocations = std::move(relocations),
        .code_key = record.code_key,
        .node_size = request.config_size,
        .node_alignment = request.config_alignment,
        .type_name = record.type_name
            ? std::string_view{record.type_name, record.type_name_size}
            : std::string_view{},
    };
    result.operations.runtime.node_data = result.node_storage.get();

    NodeDescriptionBuilder builder(result);
    auto sink = builder.sink();
    request.describe(result.node_storage.get(), sink);
    return result;
}

} // namespace iv::details
