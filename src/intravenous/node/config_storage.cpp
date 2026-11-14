#include <intravenous/node/config_storage.h>
#include <intravenous/node/config_relocations.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <new>
#include <stdexcept>

namespace iv::details {
std::shared_ptr<void const> copy_node_config_bytes(
    void const* source,
    std::size_t size,
    std::size_t alignment)
{
    if (!source || size == 0 || alignment == 0) {
        throw std::invalid_argument("invalid node storage request");
    }

    auto* storage = ::operator new(size, std::align_val_t{alignment});
    std::memcpy(storage, source, size);
    return std::shared_ptr<void const>(
        storage,
        [alignment](void const* pointer) {
            ::operator delete(
                const_cast<void*>(pointer), std::align_val_t{alignment});
        });
}
std::shared_ptr<ImmutableConfigData const> copy_immutable_config_data(
    std::span<std::byte const> bytes, std::size_t alignment)
{
    if (bytes.empty() || !std::has_single_bit(alignment)) {
        throw std::invalid_argument("immutable configuration data needs bytes and a power-of-two alignment");
    }
    auto result = std::make_shared<ImmutableConfigData>();
    result->alignment = alignment;
    result->storage = copy_node_config_bytes(
        bytes.data(), bytes.size(), std::max(alignment, alignof(std::max_align_t)));
    result->bytes = {static_cast<std::byte const*>(result->storage.get()), bytes.size()};
    return result;
}
} // namespace iv::details
