#pragma once
#include <span>
#include <array>
#include <limits>
#include <variant>
#include <memory>


namespace iv {
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

        constexpr bool can_allocate() const noexcept
        {
            return true;
        }

        constexpr std::span<std::byte> get_buffer() const noexcept
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
            T* ptr = ::new (buffer_start) T;
            buffer = { static_cast<std::byte*>(buffer_start) + num_bytes, space_left - num_bytes };
            return *ptr;
        }

        template<typename T>
        auto allocate_array(size_t number)
        {
            auto span = new_array<AlignedStorage<T>>(number);
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
        size_t total_bytes = MAX_ALLOCATION;

    public:
        explicit CountingNonAllocator(std::byte* memory_hint) :
            _memory_hint(memory_hint)
        {
        }

        constexpr bool can_allocate() const noexcept
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
            advance_buffer<T>(number);
            return std::span<T> { static_cast<T*>(nullptr), number };
        };

        template<typename T>
        T& new_object()
        {
            static AlignedStorage<T> storage;
            advance_buffer<AlignedStorage<T>>(1);
            return storage.object;
        }

        template<typename T>
        auto allocate_array(size_t number)
        {
            std::span<AlignedStorage<T>> span = new_array<AlignedStorage<T>>(number);
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

    struct TypeErasedAllocator {
        std::variant<std::reference_wrapper<FixedBufferAllocator>, std::reference_wrapper<CountingNonAllocator>> _allocator;

        constexpr bool can_allocate() const noexcept
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
            return std::visit([=](auto&& allocator) { return allocator.get().new_array<T>(number); }, _allocator);
        };

        template<typename T>
        auto new_object() -> T&
        {
            return std::visit([](auto&& allocator) -> T& { return allocator.get().new_object<T>(); }, _allocator);
        }

        template<typename T>
        auto allocate_array(size_t number)
        {
            return std::visit([=](auto&& allocator) { return allocator.get().allocate_array<T>(number); }, _allocator);
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