#pragma once

#include <intravenous/channel_layout.h>
#include <intravenous/compat.h>
#include <intravenous/continuous_value.h>
#include <intravenous/coverage.h>

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <type_traits>

namespace iv {

// Compiler-resolved storage for one semantic channel. Logical value ordinals,
// rather than project-sample indices, address the ring. This is what lets the
// same facade serve slow FFT-block streams and faster oversampled streams.
struct ContinuousChannelStorageBinding {
    std::byte* storage = nullptr;
    std::size_t value_capacity = 0;
    std::size_t value_stride_bytes = sizeof(Sample);
    std::size_t value_delay = 0;
    // Compiler-owned invocation storage used only when the requested
    // history-inclusive block cannot alias the source ring contiguously.
    std::byte* contiguous_scratch = nullptr;
    // Selected direct alias or populated scratch for the current invocation.
    mutable std::byte* contiguous_storage = nullptr;
};

struct ContinuousPortStorageBinding {
    std::array<
        ContinuousChannelStorageBinding,
        maximum_supported_channel_count> channels{};
    ContinuousValueStorageGeometry geometry{};
    std::size_t value_capacity = 0;
    std::size_t storage_latency = 0;
    ChannelLayout channel_layout {
        .channel_type = ChannelTypeId::mono,
        .sample_layout = SampleStreamLayout::planar,
    };
};

struct ContinuousInputPortBinding {
    ContinuousPortStorageBinding storage{};
    std::size_t history = 0;
    std::size_t read_latency = 0;
    std::int32_t absolute_pace_exponent = 0;
    std::size_t contiguous_value_capacity = 0;
    // Physical invocation-order ordinal selected by generated execution for
    // this callback. It is deliberately independent of the global lattice
    // ordinal exposed to node code.
    mutable std::uint64_t sequential_begin_ordinal = 0;
    mutable std::uint64_t contiguous_begin_ordinal = 0;
    mutable std::size_t contiguous_value_count = 0;
};

struct ContinuousOutputPortBinding {
    ContinuousPortStorageBinding storage{};
    std::size_t history = 0;
    std::size_t latency = 0;
    std::int32_t absolute_pace_exponent = 0;
    mutable std::uint64_t sequential_begin_ordinal = 0;
};

// One logical random-access Region. storage addresses one contiguous native-
// layout buffer containing range.extent values for every channel. The backing
// of two descriptors is independent even when their logical ranges overlap.
// This is a non-owning reflected ABI record; transaction/snapshot owners keep
// the storage alive for the callback that receives it.
struct ContinuousRegionStorageView {
    std::byte* storage = nullptr;
    // Optional background-evaluation completeness bitmap. One byte belongs
    // to each native-layout value (frame/channel pair). Mutable Region access
    // marks it; read-only persisted and Tick views leave it null.
    std::uint8_t* written_values = nullptr;
    GlobalIndexRange range{};
    ContinuousValueStorageGeometry geometry{};
    ChannelLayout channel_layout{
        .channel_type = ChannelTypeId::mono,
        .sample_layout = SampleStreamLayout::planar,
    };

    [[nodiscard]] constexpr std::size_t frame_count() const noexcept
    {
        return static_cast<std::size_t>(range.extent);
    }

    [[nodiscard]] constexpr std::size_t storage_size_bytes() const noexcept
    {
        return frame_count() * channel_count(channel_layout)
            * geometry.value_size_bytes;
    }
};

static_assert(std::is_standard_layout_v<ContinuousRegionStorageView>);
static_assert(std::is_trivially_copyable_v<ContinuousRegionStorageView>);

static_assert(std::is_standard_layout_v<ContinuousChannelStorageBinding>);
static_assert(std::is_trivially_copyable_v<ContinuousChannelStorageBinding>);
static_assert(std::is_standard_layout_v<ContinuousPortStorageBinding>);
static_assert(std::is_trivially_copyable_v<ContinuousPortStorageBinding>);
static_assert(std::is_standard_layout_v<ContinuousInputPortBinding>);
static_assert(std::is_trivially_copyable_v<ContinuousInputPortBinding>);
static_assert(std::is_standard_layout_v<ContinuousOutputPortBinding>);
static_assert(std::is_trivially_copyable_v<ContinuousOutputPortBinding>);

namespace details {

template<RegisteredContinuousValue Value, bool Mutable>
class ContinuousRegionView {
    using Extension = ContinuousValueType<Value>;
    using Element = typename Extension::storage_element;
    using StorageByte = std::conditional_t<Mutable, std::byte, std::byte const>;
    using StorageElement = std::conditional_t<Mutable, Element, Element const>;

    ContinuousRegionStorageView const* region_ = nullptr;

    [[nodiscard]] constexpr StorageByte* value_storage(
        std::size_t frame,
        std::size_t channel) const noexcept
    {
        IV_ASSERT(region_ != nullptr && region_->storage != nullptr,
            "continuous Region storage is absent");
        IV_ASSERT(frame < frame_count(),
            "continuous Region frame is out of bounds");
        IV_ASSERT(channel < channel_count(region_->channel_layout),
            "continuous Region channel is out of bounds");
        auto const channels = channel_count(region_->channel_layout);
        auto const value = region_->channel_layout.sample_layout
                == SampleStreamLayout::planar
            ? channel * frame_count() + frame
            : frame * channels + channel;
        return region_->storage + value * region_->geometry.value_size_bytes;
    }

    constexpr void mark_value_written(
        std::size_t frame,
        std::size_t channel) const noexcept
    requires Mutable
    {
        if (!region_->written_values) return;
        auto const channels = channel_count(region_->channel_layout);
        auto const value = region_->channel_layout.sample_layout
                == SampleStreamLayout::planar
            ? channel * frame_count() + frame
            : frame * channels + channel;
        region_->written_values[value] = 1;
    }

public:
    using reference = std::conditional_t<
        Mutable,
        typename Extension::mutable_reference,
        typename Extension::const_reference>;

    constexpr ContinuousRegionView() noexcept = default;
    explicit constexpr ContinuousRegionView(
        ContinuousRegionStorageView const& region) noexcept
        : region_(&region)
    {
        IV_ASSERT(region.geometry.value_type
                == continuous_value_type_id_v<Value>,
            "continuous Region value type does not match its typed facade");
        IV_ASSERT(region.geometry.element_size == sizeof(Element)
                && region.geometry.element_alignment == alignof(Element),
            "continuous Region geometry does not match its typed facade");
    }

    [[nodiscard]] constexpr GlobalIndexRange range() const noexcept
    {
        return region_->range;
    }

    [[nodiscard]] constexpr std::size_t frame_count() const noexcept
    {
        return region_ ? region_->frame_count() : 0;
    }

    [[nodiscard]] constexpr ChannelLayout channel_layout() const noexcept
    {
        return region_->channel_layout;
    }

    [[nodiscard]] constexpr reference value(
        std::size_t frame,
        std::size_t channel = 0) const noexcept
    {
        auto* bytes = value_storage(frame, channel);
        IV_ASSERT(reinterpret_cast<std::uintptr_t>(bytes) % alignof(Element) == 0,
            "continuous Region value storage is misaligned");
        auto elements = std::span<StorageElement>{
            reinterpret_cast<StorageElement*>(bytes),
            region_->geometry.elements_per_value,
        };
        if constexpr (Mutable) {
            mark_value_written(frame, channel);
            return Extension::write_value(elements);
        } else {
            return Extension::read_value(elements);
        }
    }

    [[nodiscard]] constexpr reference at(
        GlobalIndex index,
        std::size_t channel = 0) const noexcept
    {
        auto const offset = range().offset_of(index);
        IV_ASSERT(offset.has_value(),
            "continuous Region index is out of bounds");
        return value(static_cast<std::size_t>(*offset), channel);
    }

    // Dense native-layout storage. Planar storage contains complete channels
    // consecutively; interleaved storage contains complete frames
    // consecutively. The ChannelLayout says which interpretation applies.
    [[nodiscard]] constexpr std::span<StorageElement> elements() const noexcept
    {
        if (frame_count() == 0) return {};
        IV_ASSERT(region_ != nullptr && region_->storage != nullptr,
            "continuous Region storage is absent");
        if constexpr (Mutable) {
            if (region_->written_values) {
                std::fill_n(
                    region_->written_values,
                    frame_count() * channel_count(region_->channel_layout),
                    std::uint8_t{1});
            }
        }
        return {
            reinterpret_cast<StorageElement*>(region_->storage),
            frame_count() * channel_count(region_->channel_layout)
                * region_->geometry.elements_per_value,
        };
    }

    // One channel is itself contiguous only for planar storage.
    [[nodiscard]] constexpr std::span<StorageElement> channel_elements(
        std::size_t channel) const noexcept
    {
        IV_ASSERT(region_->channel_layout.sample_layout
                == SampleStreamLayout::planar,
            "an interleaved Region has no contiguous channel span");
        IV_ASSERT(channel < channel_count(region_->channel_layout),
            "continuous Region channel is out of bounds");
        auto const count = frame_count() * region_->geometry.elements_per_value;
        if constexpr (Mutable) {
            if (region_->written_values) {
                std::fill_n(
                    region_->written_values + channel * frame_count(),
                    frame_count(),
                    std::uint8_t{1});
            }
        }
        return {
            reinterpret_cast<StorageElement*>(region_->storage)
                + channel * count,
            count,
        };
    }
};

template<RegisteredContinuousValue Value, bool Mutable>
class ContinuousCoverageView {
    std::span<ContinuousRegionStorageView const> regions_{};
    IndexCoverage const* coverage_ = nullptr;

public:
    using region_type = ContinuousRegionView<Value, Mutable>;

    class iterator {
        ContinuousRegionStorageView const* current_ = nullptr;

    public:
        using value_type = region_type;
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::forward_iterator_tag;
        using iterator_concept = std::forward_iterator_tag;

        constexpr iterator() noexcept = default;
        explicit constexpr iterator(
            ContinuousRegionStorageView const* current) noexcept
            : current_(current)
        {}

        [[nodiscard]] constexpr region_type operator*() const noexcept
        {
            return region_type{*current_};
        }

        constexpr iterator& operator++() noexcept
        {
            ++current_;
            return *this;
        }

        constexpr iterator operator++(int) noexcept
        {
            auto previous = *this;
            ++*this;
            return previous;
        }

        [[nodiscard]] friend constexpr bool operator==(
            iterator const&, iterator const&) = default;
    };

    constexpr ContinuousCoverageView() noexcept = default;
    constexpr ContinuousCoverageView(
        std::span<ContinuousRegionStorageView const> regions,
        IndexCoverage const& coverage) noexcept
        : regions_(regions), coverage_(&coverage)
    {
        for (auto const& region : regions_) {
            IV_ASSERT(region.geometry.value_type
                    == continuous_value_type_id_v<Value>,
                "continuous Coverage contains the wrong value type");
        }
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept
    {
        return regions_.size();
    }
    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return regions_.empty();
    }
    [[nodiscard]] constexpr region_type operator[](
        std::size_t index) const noexcept
    {
        IV_ASSERT(index < regions_.size(),
            "continuous Coverage Region is out of bounds");
        return region_type{regions_[index]};
    }
    [[nodiscard]] constexpr iterator begin() const noexcept
    {
        return iterator{regions_.data()};
    }
    [[nodiscard]] constexpr iterator end() const noexcept
    {
        return iterator{regions_.empty()
            ? regions_.data()
            : regions_.data() + regions_.size()};
    }
    [[nodiscard]] constexpr IndexCoverage const& coverage() const noexcept
    {
        IV_ASSERT(coverage_ != nullptr,
            "continuous Coverage has no positional union");
        return *coverage_;
    }
};

template<RegisteredContinuousValue Value>
class ContinuousChannelStorageAccess {
    using Extension = ContinuousValueType<Value>;
    using Element = typename Extension::storage_element;

    ContinuousChannelStorageBinding const* binding_ = nullptr;
    ContinuousValueStorageGeometry const* geometry_ = nullptr;

    [[nodiscard]] constexpr std::span<Element> elements_at(
        std::uint64_t ordinal) const noexcept
    {
        IV_ASSERT(binding_ != nullptr && geometry_ != nullptr,
            "continuous channel storage is absent");
        IV_ASSERT(binding_->storage != nullptr,
            "continuous channel storage pointer is null");
        IV_ASSERT(binding_->value_capacity != 0
                && std::has_single_bit(binding_->value_capacity),
            "continuous channel capacity must be a nonzero power of two");
        IV_ASSERT(geometry_->value_type == continuous_value_type_id_v<Value>,
            "continuous channel value type does not match its typed facade");
        IV_ASSERT(geometry_->element_size == sizeof(Element)
                && geometry_->element_alignment == alignof(Element),
            "continuous channel storage geometry does not match its extension");

        auto const delayed = ordinal - binding_->value_delay;
        auto const slot = static_cast<std::size_t>(delayed)
            & (binding_->value_capacity - 1);
        auto* bytes = binding_->storage + slot * binding_->value_stride_bytes;
        IV_ASSERT(reinterpret_cast<std::uintptr_t>(bytes) % alignof(Element) == 0,
            "continuous channel value storage is misaligned");
        return {
            reinterpret_cast<Element*>(bytes),
            geometry_->elements_per_value,
        };
    }

public:
    constexpr ContinuousChannelStorageAccess() noexcept = default;

    constexpr ContinuousChannelStorageAccess(
        ContinuousChannelStorageBinding const& binding,
        ContinuousValueStorageGeometry const& geometry) noexcept
        : binding_(&binding), geometry_(&geometry)
    {}

    [[nodiscard]] constexpr typename Extension::const_reference read(
        std::uint64_t ordinal) const noexcept
    {
        auto elements = elements_at(ordinal);
        return Extension::read_value(
            std::span<Element const>{elements.data(), elements.size()});
    }

    [[nodiscard]] constexpr typename Extension::mutable_reference write(
        std::uint64_t ordinal) const noexcept
    {
        return Extension::write_value(elements_at(ordinal));
    }
};

template<RegisteredContinuousValue Value, bool Mutable>
class ContinuousValueBlockView {
    using Extension = ContinuousValueType<Value>;
    using Element = typename Extension::storage_element;
    using StorageByte = std::conditional_t<Mutable, std::byte, std::byte const>;
    using StorageElement = std::conditional_t<Mutable, Element, Element const>;

    StorageByte* storage_ = nullptr;
    ContinuousValueStorageGeometry const* geometry_ = nullptr;
    ContinuousPortStorageBinding const* ring_storage_ = nullptr;
    std::uint64_t ring_begin_ordinal_ = 0;
    std::size_t ring_channel_ = 0;
    std::size_t count_ = 0;

    [[nodiscard]] constexpr std::span<StorageElement> elements_at(
        std::size_t index) const noexcept
    {
        IV_ASSERT(storage_ != nullptr && geometry_ != nullptr && index < count_,
            "continuous block value is out of bounds");
        auto* bytes = storage_ + index * geometry_->value_size_bytes;
        IV_ASSERT(reinterpret_cast<std::uintptr_t>(bytes) % alignof(Element) == 0,
            "continuous block storage is misaligned");
        return {
            reinterpret_cast<StorageElement*>(bytes),
            geometry_->elements_per_value,
        };
    }

public:
    using reference = std::conditional_t<
        Mutable,
        typename Extension::mutable_reference,
        typename Extension::const_reference>;

    class iterator {
        ContinuousValueBlockView const* view_ = nullptr;
        std::size_t index_ = 0;

    public:
        constexpr iterator() noexcept = default;
        constexpr iterator(
            ContinuousValueBlockView const& view,
            std::size_t index) noexcept
            : view_(&view), index_(index)
        {}

        [[nodiscard]] constexpr reference operator*() const noexcept
        {
            return (*view_)[index_];
        }

        constexpr iterator& operator++() noexcept
        {
            ++index_;
            return *this;
        }

        [[nodiscard]] constexpr bool operator==(
            iterator const&) const noexcept = default;
    };

    constexpr ContinuousValueBlockView() noexcept = default;

    constexpr ContinuousValueBlockView(
        StorageByte* storage,
        ContinuousValueStorageGeometry const& geometry,
        std::size_t count) noexcept
        : storage_(storage)
        , geometry_(&geometry)
        , count_(count)
    {}

    constexpr ContinuousValueBlockView(
        ContinuousPortStorageBinding const& storage,
        std::uint64_t begin_ordinal,
        std::size_t count,
        std::size_t channel) noexcept
        : geometry_(&storage.geometry)
        , ring_storage_(&storage)
        , ring_begin_ordinal_(begin_ordinal)
        , ring_channel_(channel)
        , count_(count)
    {
        IV_ASSERT(channel < channel_count(storage.channel_layout),
            "continuous ring block channel is out of bounds");
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept { return count_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return count_ == 0; }

    [[nodiscard]] constexpr iterator begin() const noexcept
    {
        return iterator{*this, 0};
    }

    [[nodiscard]] constexpr iterator end() const noexcept
    {
        return iterator{*this, count_};
    }

    [[nodiscard]] constexpr reference operator[](
        std::size_t index) const noexcept
    {
        if (ring_storage_ != nullptr) {
            IV_ASSERT(index < count_,
                "continuous ring block value is out of bounds");
            ContinuousChannelStorageAccess<Value> access{
                ring_storage_->channels[ring_channel_],
                ring_storage_->geometry};
            if constexpr (Mutable) {
                return access.write(ring_begin_ordinal_ + index);
            } else {
                return access.read(ring_begin_ordinal_ + index);
            }
        }
        auto elements = elements_at(index);
        if constexpr (Mutable) {
            return Extension::write_value(elements);
        } else {
            return Extension::read_value(elements);
        }
    }

    // Dense storage elements underlying the transported values. For Sample
    // this is the ordinary contiguous sample span required by FFT libraries;
    // for FFTBlock it is the concatenated canonical complex-bin storage.
    [[nodiscard]] constexpr auto elements() const noexcept
    {
        using SpanElement = std::conditional_t<Mutable, Element, Element const>;
        if (count_ == 0) return std::span<SpanElement>{};
        IV_ASSERT(storage_ != nullptr && geometry_ != nullptr
                && ring_storage_ == nullptr,
            "continuous block does not have one dense element span");
        return std::span<SpanElement>{
            reinterpret_cast<SpanElement*>(storage_),
            count_ * geometry_->elements_per_value,
        };
    }
};

template<RegisteredContinuousValue Value>
class ContinuousInputPort {
    using Extension = ContinuousValueType<Value>;

    ContinuousPortStorageBinding const* storage_ = nullptr;
    std::size_t history_ = 0;
    std::size_t read_latency_ = 0;
    FractionalGlobalIndex begin_{};
    std::uint64_t begin_ordinal_ = 0;
    std::uint64_t sequential_begin_ordinal_ = 0;
    std::size_t block_size_ = 0;
    std::uint64_t contiguous_begin_ordinal_ = 0;
    std::size_t contiguous_value_count_ = 0;

    [[nodiscard]] constexpr ContinuousValueBlockView<Value, false>
    contiguous_block(
        std::uint64_t requested_begin,
        std::size_t requested_count,
        std::size_t channel) const noexcept
    {
        auto const* base = storage_->channels[channel].contiguous_storage;
        if (base != nullptr) {
            auto const offset_bits =
                requested_begin - contiguous_begin_ordinal_;
            if (offset_bits <= contiguous_value_count_
                && requested_count
                    <= contiguous_value_count_ - offset_bits) {
                auto const offset = static_cast<std::size_t>(offset_bits);
                return {
                    base + offset * storage_->geometry.value_size_bytes,
                    storage_->geometry,
                    requested_count,
                };
            }
        }

        IV_ASSERT(history_ == 0 && requested_count <= 1,
            "continuous input block was not prepared contiguously");
        return {
            *storage_,
            requested_begin,
            requested_count,
            channel,
        };
    }

public:
    using value_type = Value;
    using reference = typename Extension::const_reference;
    using block_type = ContinuousValueBlockView<Value, false>;

    constexpr ContinuousInputPort() noexcept = default;

    template<class Binding>
        requires requires(Binding const& binding) {
            binding.storage;
            binding.history;
            binding.read_latency;
            binding.contiguous_begin_ordinal;
            binding.contiguous_value_count;
            binding.sequential_begin_ordinal;
        }
    constexpr ContinuousInputPort(
        Binding const& binding,
        FractionalGlobalIndex begin,
        std::uint64_t begin_ordinal,
        std::uint64_t sequential_begin_ordinal,
        std::size_t block_size) noexcept
        : storage_(&binding.storage)
        , history_(binding.history)
        , read_latency_(binding.read_latency)
        , begin_(begin)
        , begin_ordinal_(begin_ordinal)
        , sequential_begin_ordinal_(sequential_begin_ordinal)
        , block_size_(block_size)
        , contiguous_begin_ordinal_(binding.contiguous_begin_ordinal)
        , contiguous_value_count_(binding.contiguous_value_count)
    {
        IV_ASSERT(storage_->geometry.value_type
                == continuous_value_type_id_v<Value>,
            "continuous input value type does not match its resolved binding");
    }

    [[nodiscard]] constexpr reference get(
        std::size_t history = 0,
        std::size_t channel = 0) const noexcept
    {
        IV_ASSERT(storage_ != nullptr && history <= history_,
            "continuous input history is out of bounds");
        IV_ASSERT(channel < channel_count(storage_->channel_layout),
            "continuous input channel is out of bounds");
        ContinuousChannelStorageAccess<Value> access{
            storage_->channels[channel], storage_->geometry};
        return access.read(
            sequential_begin_ordinal_ - read_latency_ - history);
    }

    [[nodiscard]] constexpr reference get_value(
        std::size_t value_offset,
        std::size_t channel = 0) const noexcept
    {
        IV_ASSERT(storage_ != nullptr && value_offset < block_size_,
            "continuous input value offset is out of bounds");
        IV_ASSERT(channel < channel_count(storage_->channel_layout),
            "continuous input channel is out of bounds");
        ContinuousChannelStorageAccess<Value> access{
            storage_->channels[channel], storage_->geometry};
        return access.read(
            sequential_begin_ordinal_ - read_latency_ + value_offset);
    }

    [[nodiscard]] constexpr block_type block(
        std::size_t channel = 0) const noexcept
    {
        IV_ASSERT(storage_ != nullptr
                && channel < channel_count(storage_->channel_layout),
            "continuous input block channel is out of bounds");
        return contiguous_block(
            sequential_begin_ordinal_ - read_latency_, block_size_, channel);
    }

    [[nodiscard]] constexpr block_type block_extended(
        std::size_t channel = 0) const noexcept
    {
        IV_ASSERT(storage_ != nullptr
                && channel < channel_count(storage_->channel_layout),
            "continuous input extended block channel is out of bounds");
        return contiguous_block(
            sequential_begin_ordinal_ - read_latency_ - history_,
            history_ + block_size_,
            channel);
    }

    [[nodiscard]] constexpr FractionalGlobalIndex begin() const noexcept
    {
        return begin_;
    }

    [[nodiscard]] constexpr std::size_t block_size() const noexcept
    {
        return block_size_;
    }

    [[nodiscard]] constexpr std::uint64_t value_index() const noexcept
    {
        return begin_ordinal_;
    }

    [[nodiscard]] constexpr std::size_t history() const noexcept
    {
        return history_;
    }

    [[nodiscard]] constexpr std::size_t latency() const noexcept
    {
        return read_latency_;
    }

    [[nodiscard]] constexpr ChannelLayout channel_layout() const noexcept
    {
        return storage_->channel_layout;
    }
};

template<RegisteredContinuousValue Value>
class ContinuousOutputPort {
    using Extension = ContinuousValueType<Value>;

    ContinuousPortStorageBinding const* storage_ = nullptr;
    std::size_t history_ = 0;
    std::size_t latency_ = 0;
    FractionalGlobalIndex begin_{};
    std::uint64_t begin_ordinal_ = 0;
    std::uint64_t sequential_begin_ordinal_ = 0;
    std::size_t block_size_ = 0;
    mutable std::size_t cursor_ = 0;

public:
    using value_type = Value;
    using const_reference = typename Extension::const_reference;
    using mutable_reference = typename Extension::mutable_reference;
    using block_type = ContinuousValueBlockView<Value, true>;

    constexpr ContinuousOutputPort() noexcept = default;

    template<class Binding>
        requires requires(Binding const& binding) {
            binding.storage;
            binding.history;
            binding.latency;
            binding.sequential_begin_ordinal;
        }
    constexpr ContinuousOutputPort(
        Binding const& binding,
        FractionalGlobalIndex begin,
        std::uint64_t begin_ordinal,
        std::uint64_t sequential_begin_ordinal,
        std::size_t block_size) noexcept
        : storage_(&binding.storage)
        , history_(binding.history)
        , latency_(binding.latency)
        , begin_(begin)
        , begin_ordinal_(begin_ordinal)
        , sequential_begin_ordinal_(sequential_begin_ordinal)
        , block_size_(block_size)
    {
        IV_ASSERT(storage_->geometry.value_type
                == continuous_value_type_id_v<Value>,
            "continuous output value type does not match its resolved binding");
    }

    [[nodiscard]] constexpr mutable_reference value(
        std::size_t offset,
        std::size_t channel = 0) const noexcept
    {
        IV_ASSERT(storage_ != nullptr && offset < block_size_,
            "continuous output value offset is out of bounds");
        IV_ASSERT(channel < channel_count(storage_->channel_layout),
            "continuous output channel is out of bounds");
        ContinuousChannelStorageAccess<Value> access{
            storage_->channels[channel], storage_->geometry};
        return access.write(
            sequential_begin_ordinal_ + storage_->storage_latency + offset);
    }

    [[nodiscard]] constexpr const_reference get(
        std::size_t offset = 0,
        std::size_t channel = 0) const noexcept
    {
        IV_ASSERT(storage_ != nullptr
                && offset <= history_ + latency_,
            "continuous output retained offset is out of bounds");
        IV_ASSERT(channel < channel_count(storage_->channel_layout),
            "continuous output channel is out of bounds");
        ContinuousChannelStorageAccess<Value> access{
            storage_->channels[channel], storage_->geometry};
        return access.read(
            sequential_begin_ordinal_ + cursor_
                + storage_->storage_latency - 1 - offset);
    }

    [[nodiscard]] constexpr mutable_reference update_value(
        std::size_t offset = 0,
        std::size_t channel = 0) const noexcept
    {
        IV_ASSERT(storage_ != nullptr && offset < latency_,
            "continuous output update lies outside its latency horizon");
        IV_ASSERT(channel < channel_count(storage_->channel_layout),
            "continuous output channel is out of bounds");
        ContinuousChannelStorageAccess<Value> access{
            storage_->channels[channel], storage_->geometry};
        return access.write(
            sequential_begin_ordinal_ + cursor_
                + storage_->storage_latency - 1 - offset);
    }

    [[nodiscard]] constexpr block_type block(
        std::size_t channel = 0) const noexcept
    {
        return {
            *storage_,
            sequential_begin_ordinal_ + storage_->storage_latency,
            block_size_,
            channel,
        };
    }

    constexpr void push(Value const& value) const noexcept
    requires (ContinuousValueType<Value>::rank == 0)
    {
        IV_ASSERT(cursor_ < block_size_,
            "continuous output authored more values than its projected block");
        this->value(cursor_++) = value;
    }

    constexpr void push(FFTBlock value) const noexcept
    requires std::same_as<Value, FFTBlock>
    {
        IV_ASSERT(cursor_ < block_size_,
            "FFT output authored more blocks than its projected block");
        auto destination = this->value(cursor_++);
        IV_ASSERT(destination.size() == value.size(),
            "FFT output block size does not match its resolved value size");
        for (std::size_t bin = 0; bin < value.size(); ++bin) {
            destination[bin] = value[bin];
        }
    }

    constexpr void update(
        Value const& value,
        std::size_t offset = 0,
        std::size_t channel = 0) const noexcept
    requires (ContinuousValueType<Value>::rank == 0)
    {
        update_value(offset, channel) = value;
    }

    constexpr void update(
        FFTBlock value,
        std::size_t offset = 0,
        std::size_t channel = 0) const noexcept
    requires std::same_as<Value, FFTBlock>
    {
        auto destination = update_value(offset, channel);
        IV_ASSERT(destination.size() == value.size(),
            "FFT output update size does not match its resolved value size");
        for (std::size_t bin = 0; bin < value.size(); ++bin) {
            destination[bin] = value[bin];
        }
    }

    [[nodiscard]] constexpr FractionalGlobalIndex begin() const noexcept
    {
        return begin_;
    }

    [[nodiscard]] constexpr std::size_t block_size() const noexcept
    {
        return block_size_;
    }

    [[nodiscard]] constexpr std::uint64_t value_index() const noexcept
    {
        return begin_ordinal_;
    }

    [[nodiscard]] constexpr std::size_t history() const noexcept
    {
        return history_;
    }

    [[nodiscard]] constexpr std::size_t latency() const noexcept
    {
        return latency_;
    }

    [[nodiscard]] constexpr ChannelLayout channel_layout() const noexcept
    {
        return storage_->channel_layout;
    }
};

} // namespace details

template<RegisteredContinuousValue Value>
using ContinuousRegion = details::ContinuousRegionView<Value, false>;

template<RegisteredContinuousValue Value>
using WritableContinuousRegion = details::ContinuousRegionView<Value, true>;

template<RegisteredContinuousValue Value>
using ContinuousCoverage = details::ContinuousCoverageView<Value, false>;

template<RegisteredContinuousValue Value>
using WritableContinuousCoverage = details::ContinuousCoverageView<Value, true>;

} // namespace iv
