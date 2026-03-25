#pragma once
#include <functional>
#include <span>
#include <array>
#include <cassert>
#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>
#include <variant>
#include <vector>


namespace iv {
    enum class AllocationKind {
        object,
        raw_array,
        aligned_array,
    };
#ifndef NDEBUG
    struct AllocationEvent {
        AllocationKind kind{};
        size_t alignment = 0;
        size_t element_size = 0;
        size_t count = 0;
        ptrdiff_t offset = 0;
        void const* type_tag = nullptr;
    };

    struct AllocationTrace {
        std::vector<AllocationEvent> events;
        size_t replay_index = 0;

        void record(AllocationEvent event)
        {
            events.push_back(event);
        }

        void reset_replay()
        {
            replay_index = 0;
        }

        void validate_next(AllocationEvent const& actual)
        {
            if (replay_index >= events.size()) {
                throw std::logic_error("allocation trace replay ran past the counting-pass record");
            }

            AllocationEvent const& expected = events[replay_index++];
            if (expected.kind != actual.kind ||
                expected.alignment != actual.alignment ||
                expected.element_size != actual.element_size ||
                expected.count != actual.count ||
                expected.offset != actual.offset ||
                expected.type_tag != actual.type_tag) {
                throw std::logic_error("allocation trace mismatch between init passes");
            }
        }

        void validate_consumed() const
        {
            if (replay_index != events.size()) {
                throw std::logic_error("allocation trace replay did not consume the full counting-pass record");
            }
        }
    };

    namespace details {
        template<typename T>
        constexpr void const* allocation_type_token()
        {
            static int token = 0;
            return &token;
        }
    }
#else
    struct AllocationTrace {
        void reset_replay() {}
        void validate_consumed() const {}
    };
#endif

	template<typename T>
    union AlignedStorage {
        alignas(T) std::byte uninitialized_object[sizeof(T)];
        T object;

        constexpr explicit AlignedStorage() :
            uninitialized_object{}
        {}
    };

    struct FixedBufferAllocator {
        std::span<std::byte> buffer;
        AllocationTrace* trace = nullptr;
        std::byte* trace_base = nullptr;

    private:
#ifndef NDEBUG
        ptrdiff_t trace_offset(void const* ptr) const
        {
            auto const* base = trace_base ? trace_base : buffer.data();
            return reinterpret_cast<std::byte const*>(ptr) - base;
        }

        template<typename T>
        void validate_trace(AllocationKind kind, void const* ptr, size_t count, size_t element_size) const
        {
            if (!trace) {
                return;
            }

            trace->validate_next(AllocationEvent{
                .kind = kind,
                .alignment = alignof(T),
                .element_size = element_size,
                .count = count,
                .offset = trace_offset(ptr),
                .type_tag = details::allocation_type_token<T>(),
            });
        }
#else
        template<typename T>
        void validate_trace(AllocationKind, void const*, size_t, size_t) const
        {
        }
#endif

    public:
        void validate_trace_consumed() const
        {
#ifndef NDEBUG
            if (trace) {
                trace->validate_consumed();
            }
#endif
        }

        constexpr bool can_allocate() const
        {
            return true;
        }

        constexpr std::span<std::byte> get_buffer() const
        {
            return buffer;
        }

        template<typename T>
        auto new_array(size_t number)
        {
            if (number == 0) return std::span<T>{};
            size_t num_bytes = number * sizeof(T);
            size_t const alignment = alignof(T);
            void* buffer_start = buffer.data();
            size_t space_left = buffer.size();
            if (!std::align(alignment, num_bytes, buffer_start, space_left)) throw std::bad_alloc();
            validate_trace<T>(AllocationKind::raw_array, buffer_start, number, sizeof(T));
            T* ptr = ::new (buffer_start) T[number];
            buffer = { static_cast<std::byte*>(buffer_start) + num_bytes, space_left - num_bytes };
            return std::span<T> { ptr, number };
        };

        template<typename T>
        T& new_object()
        {
            size_t num_bytes = sizeof(T);
            size_t const alignment = alignof(T);
            void* buffer_start = buffer.data();
            size_t space_left = buffer.size();
            if (!std::align(alignment, num_bytes, buffer_start, space_left)) throw std::bad_alloc();
            validate_trace<T>(AllocationKind::object, buffer_start, 1, sizeof(T));
            T* ptr = ::new (buffer_start) T;
            buffer = { static_cast<std::byte*>(buffer_start) + num_bytes, space_left - num_bytes };
            return *ptr;
        }

        template<typename T>
        auto allocate_array(size_t number)
        {
            if (number == 0) return std::span<T>{};
            size_t num_bytes = number * sizeof(AlignedStorage<T>);
            size_t const alignment = alignof(AlignedStorage<T>);
            void* buffer_start = buffer.data();
            size_t space_left = buffer.size();
            if (!std::align(alignment, num_bytes, buffer_start, space_left)) throw std::bad_alloc();
            validate_trace<T>(AllocationKind::aligned_array, buffer_start, number, sizeof(AlignedStorage<T>));
            auto* ptr = ::new (buffer_start) AlignedStorage<T>[number];
            buffer = { static_cast<std::byte*>(buffer_start) + num_bytes, space_left - num_bytes };
            std::span<AlignedStorage<T>> span { ptr, number };
            return std::span<T> { &(span.data()->object), span.size() };
        };

        template<typename T, typename... Args>
        void construct_at(T* ptr, Args&&... args) const
        {
            ::new (ptr) T(std::forward<Args>(args)...);
        }

        template<typename T, typename U>
        constexpr T& assign(T& t, U&& u) const
        {
            return t = std::forward<U>(u);
        }

        template<typename T>
        constexpr auto at(std::span<T> t, size_t i) const -> T&
        {
            return t[i];
        }

        template<typename T, size_t N>
        constexpr auto at(std::array<T, N>& t, size_t i) const -> T&
        {
            return t[i];
        }

        template<typename R, typename T>
        void fill_n(R& range, T&& t) const
        {
            std::fill_n(range.begin(), range.size(), std::forward<T>(t));
        }
    };

    class CountingNonAllocator {
        static constexpr size_t MAX_ALLOCATION = std::numeric_limits<uint32_t>::max();
        std::byte* _memory_hint = nullptr;
        std::byte* _trace_base = nullptr;
        AllocationTrace* _trace = nullptr;
        size_t total_bytes = MAX_ALLOCATION;

#ifndef NDEBUG
        ptrdiff_t trace_offset(void const* ptr) const
        {
            auto const* base = _trace_base ? _trace_base : _memory_hint;
            return reinterpret_cast<std::byte const*>(ptr) - base;
        }

        template<typename T>
        void record_trace(AllocationKind kind, void const* ptr, size_t count, size_t element_size)
        {
            if (!_trace) {
                return;
            }

            _trace->record(AllocationEvent{
                .kind = kind,
                .alignment = alignof(T),
                .element_size = element_size,
                .count = count,
                .offset = trace_offset(ptr),
                .type_tag = details::allocation_type_token<T>(),
            });
        }
#else
        template<typename T>
        void record_trace(AllocationKind, void const*, size_t, size_t)
        {
        }
#endif

    public:
        explicit CountingNonAllocator(
            std::byte* memory_hint,
            AllocationTrace* trace = nullptr,
            std::byte* trace_base = nullptr
        ) :
            _memory_hint(memory_hint),
            _trace_base(trace_base ? trace_base : memory_hint),
            _trace(trace)
        {
        }

        constexpr bool can_allocate() const
        {
            return false;
        }

        constexpr std::span<std::byte> get_buffer() const
        {
            return { _memory_hint, total_bytes };
        }

        constexpr size_t estimate_buffer_size() const
        {
            return MAX_ALLOCATION - total_bytes;
        }

        template<typename T>
        void advance_buffer(size_t number)
        {
            if (number == 0) return;
            size_t const alignment = alignof(T);
            size_t const num_bytes = number * sizeof(T);
            void* buffer_start = static_cast<void*>(_memory_hint);
            if (!std::align(alignment, num_bytes, buffer_start, total_bytes)) throw std::bad_alloc();
            _memory_hint = static_cast<std::byte*>(buffer_start) + num_bytes;
            total_bytes -= num_bytes;
        };

        template<typename T>
        auto new_array(size_t number)
        {
            if (number == 0) return std::span<T>{};
            size_t const alignment = alignof(T);
            size_t const num_bytes = number * sizeof(T);
            void* buffer_start = static_cast<void*>(_memory_hint);
            size_t space = total_bytes;
            if (!std::align(alignment, num_bytes, buffer_start, space)) throw std::bad_alloc();
            T* fake_ptr = static_cast<T*>(buffer_start);
            record_trace<T>(AllocationKind::raw_array, fake_ptr, number, sizeof(T));
            _memory_hint = static_cast<std::byte*>(buffer_start) + num_bytes;
            total_bytes = space - num_bytes;
            return std::span<T> { fake_ptr, number };
        };

        template<typename T>
        T& new_object()
        {
            static AlignedStorage<T> storage;
            size_t const alignment = alignof(T);
            size_t const num_bytes = sizeof(T);
            void* buffer_start = static_cast<void*>(_memory_hint);
            size_t space = total_bytes;
            if (!std::align(alignment, num_bytes, buffer_start, space)) throw std::bad_alloc();
            record_trace<T>(AllocationKind::object, buffer_start, 1, sizeof(T));
            _memory_hint = static_cast<std::byte*>(buffer_start) + num_bytes;
            total_bytes = space - num_bytes;
            return storage.object;
        }

        template<typename T>
        auto allocate_array(size_t number)
        {
            if (number == 0) return std::span<T>{};
            size_t const alignment = alignof(AlignedStorage<T>);
            size_t const num_bytes = number * sizeof(AlignedStorage<T>);
            void* buffer_start = static_cast<void*>(_memory_hint);
            size_t space = total_bytes;
            if (!std::align(alignment, num_bytes, buffer_start, space)) throw std::bad_alloc();
            auto* fake_ptr = static_cast<AlignedStorage<T>*>(buffer_start);
            record_trace<T>(AllocationKind::aligned_array, fake_ptr, number, sizeof(AlignedStorage<T>));
            _memory_hint = static_cast<std::byte*>(buffer_start) + num_bytes;
            total_bytes = space - num_bytes;
            std::span<AlignedStorage<T>> span { fake_ptr, number };
            return std::span<T> { static_cast<T*>(nullptr), span.size() };
        };

        template<typename T, typename... Args>
        void construct_at(T*, Args&&...) const
        {
        }

        template<typename T, typename U>
        constexpr T& assign(T&, U&&) const
        {
            static AlignedStorage<T> storage;
            return storage.object;
        }

        template<typename T>
        constexpr auto at(std::span<T>, size_t) const -> T&
        {
            static AlignedStorage<T> storage;
            return storage.object;
        }

        template<typename T, size_t N>
        constexpr auto at(std::array<T, N>&, size_t) const -> T&
        {
            static AlignedStorage<T> storage;
            return storage.object;
        }

        template<typename R, typename T>
        void fill_n(R&, T&&) const
        {}
    };

    inline FixedBufferAllocator make_fixed_buffer_allocator(
        std::span<std::byte> buffer,
        AllocationTrace* trace = nullptr,
        std::byte* trace_base = nullptr
    )
    {
        return FixedBufferAllocator {
            buffer,
            trace,
            trace_base,
        };
    }

    inline CountingNonAllocator make_counting_allocator(
        std::byte* memory_hint,
        AllocationTrace* trace = nullptr,
        std::byte* trace_base = nullptr
    )
    {
        return CountingNonAllocator(memory_hint, trace, trace_base);
    }

    struct TypeErasedAllocator {
        std::variant<std::reference_wrapper<FixedBufferAllocator>, std::reference_wrapper<CountingNonAllocator>> _allocator;

        template<typename A>
        requires(!std::is_same_v<std::decay_t<A>, TypeErasedAllocator>)
        TypeErasedAllocator(A&& a): _allocator(std::forward<A>(a)) {}

        TypeErasedAllocator(TypeErasedAllocator const&) = default;
        TypeErasedAllocator(TypeErasedAllocator&&) = default;

        constexpr bool can_allocate() const
        {
            return std::visit([](auto&& allocator) { return allocator.get().can_allocate(); }, _allocator);
        }

        constexpr std::span<std::byte> get_buffer() const
        {
            return std::visit([](auto&& allocator) { return allocator.get().get_buffer(); }, _allocator);
        }

        template<typename T>
        auto new_array(size_t number)
        {
            return std::visit([=](auto&& allocator) { return allocator.get().template new_array<T>(number); }, _allocator);
        };

        template<typename T>
        auto new_object() -> T&
        {
            return std::visit([](auto&& allocator) -> T& { return allocator.get().template new_object<T>(); }, _allocator);
        }

        template<typename T>
        auto allocate_array(size_t number)
        {
            return std::visit([=](auto&& allocator) { return allocator.get().template allocate_array<T>(number); }, _allocator);
        };

        template<typename T, typename... Args>
        void construct_at(T* ptr, Args&&... args) const
        {
            std::visit([&](auto&& allocator) { return allocator.get().construct_at(ptr, std::forward<Args>(args)...); }, _allocator);
        }

        template<typename T, typename U>
        constexpr T& assign(T& t, U&& u) const
        {
            return std::visit([&](auto&& allocator) -> T& { return allocator.get().assign(t, std::forward<U>(u)); }, _allocator);
        }

        template<typename T>
        constexpr auto at(std::span<T> t, size_t i) const -> T&
        {
            return std::visit([&](auto&& allocator) -> T& { return allocator.get().at(t, i); }, _allocator);
        }

        template<typename T, size_t N>
        constexpr auto at(std::array<T, N>& t, size_t i) const -> T&
        {
            return std::visit([&](auto&& allocator) -> T& { return allocator.get().at(t, i); }, _allocator);
        }

        template<typename R, typename T>
        void fill_n(R& r, T&& t) const
        {
            std::visit([&](auto&& allocator) { return allocator.get().fill_n(r, std::forward<T>(t)); }, _allocator);
        }
    };
}
