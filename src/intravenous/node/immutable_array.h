#pragma once

#include <cstddef>
#include <span>
#include <type_traits>

namespace iv {
class GraphBuilder;

// A read-only construction value. GraphBuilder owns a copy of the elements;
// node configuration relocations retain it for every consuming generation.
// Elements must be trivially copyable and contain no pointers or owning handles.
template<class T>
class ImmutableArray {
    static_assert(std::is_trivially_copyable_v<T>);
    T const* data_ = nullptr;
    std::size_t size_ = 0;

    friend class GraphBuilder;
    constexpr ImmutableArray(T const* data, std::size_t size) noexcept
        : data_(data), size_(size) {}

public:
    constexpr ImmutableArray() noexcept = default;
    [[nodiscard]] constexpr T const* data() const noexcept { return data_; }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] constexpr T const& operator[](std::size_t i) const noexcept { return data_[i]; }
    [[nodiscard]] constexpr T const* begin() const noexcept { return data_; }
    [[nodiscard]] constexpr T const* end() const noexcept { return size_ ? data_ + size_ : data_; }
    [[nodiscard]] constexpr std::span<T const> values() const noexcept { return {data_, size_}; }
};
} // namespace iv
