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

void ProjectLaneViewBuilder::succeed(LaneViewResult value)
{
    result = std::move(value);
}

std::vector<CreatableLaneDescriptor> ProjectLaneTypesBuilder::build() const
{
    if (!result.has_value()) throw std::runtime_error("runtime project lane type query was not handled");
    return *result;
}

LaneViewResult ProjectLaneViewBuilder::build() const
{
    if (!result.has_value()) {
        throw std::runtime_error("runtime project lane view result was not provided");
    }
    return *result;
}

} // namespace iv
