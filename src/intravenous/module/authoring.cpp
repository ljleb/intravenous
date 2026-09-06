#include <intravenous/module/authoring.h>

#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>

namespace iv::details {
std::shared_ptr<void const> copy_authored_node_bytes(
    void const* source,
    std::size_t size,
    std::size_t alignment)
{
    if (!source || size == 0 || alignment == 0) {
        throw std::invalid_argument("invalid authored node storage request");
    }

    auto* storage = ::operator new(size, std::align_val_t{alignment});
    std::memcpy(storage, source, size);
    return std::shared_ptr<void const>(
        storage,
        [alignment](void const* pointer) {
            ::operator delete(
                const_cast<void*>(pointer),
                std::align_val_t{alignment});
        });
}
} // namespace iv::details
