#include <intravenous/node/tick.h>

#include <sstream>
#include <stdexcept>

namespace iv {
std::span<std::byte> remaining_buffer(
    std::span<std::byte> buffer, std::byte* state_base)
{
    if (!state_base) {
        throw std::logic_error("nested node state pointer cannot be null");
    }

    auto* const buffer_begin = buffer.data();
    auto* const buffer_end = buffer_begin + buffer.size();
    if (state_base < buffer_begin || state_base > buffer_end) {
        std::ostringstream oss;
        oss << "nested node state pointer is outside the enclosing buffer"
            << " (state=" << static_cast<void*>(state_base)
            << ", begin=" << static_cast<void*>(buffer_begin)
            << ", end=" << static_cast<void*>(buffer_end) << ")";
        throw std::logic_error(oss.str());
    }

    return {state_base, static_cast<size_t>(buffer_end - state_base)};
}
} // namespace iv
