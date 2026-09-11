#include <intravenous/runtime/runtime_project_events.h>

#include <stdexcept>
#include <utility>

namespace iv {
void ProjectAckBuilder::succeed()
{
    handled = true;
    error_message.reset();
}

void ProjectAckBuilder::fail(std::string message)
{
    handled = false;
    error_message = std::move(message);
}

void ProjectAckBuilder::build() const
{
    if (error_message.has_value()) {
        throw std::runtime_error(*error_message);
    }
    if (!handled) {
        throw std::runtime_error("runtime project event was not handled");
    }
}

void ProjectGraphInputAckBuilder::succeed(GraphInputPublicPortsSnapshot value)
{
    handled = true;
    error_message.reset();
    public_ports = std::move(value);
}

void ProjectGraphInputAckBuilder::fail(std::string message)
{
    handled = false;
    public_ports.reset();
    error_message = std::move(message);
}

GraphInputPublicPortsSnapshot ProjectGraphInputAckBuilder::build() const
{
    if (error_message.has_value()) {
        throw std::runtime_error(*error_message);
    }
    if (!handled || !public_ports.has_value()) {
        throw std::runtime_error("graph input project event was not handled");
    }
    return *public_ports;
}

void ProjectStringBuilder::succeed(std::string value)
{
    result = std::move(value);
}

std::string ProjectStringBuilder::build() const
{
    if (!result.has_value()) {
        throw std::runtime_error("runtime project string result was not provided");
    }
    return *result;
}

void ProjectAudioDevicesBuilder::succeed(AudioDevicesSnapshot value)
{
    result = std::move(value);
}

AudioDevicesSnapshot ProjectAudioDevicesBuilder::build() const
{
    if (!result.has_value()) {
        throw std::runtime_error(
            "runtime project audio devices result was not provided");
    }
    return *result;
}

std::vector<CreatableLaneDescriptor> ProjectLaneTypesBuilder::build() const
{
    if (!result.has_value()) throw std::runtime_error("runtime project lane type query was not handled");
    return *result;
}

} // namespace iv
