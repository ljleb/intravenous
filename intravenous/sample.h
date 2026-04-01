#pragma once

#include <type_traits>

namespace iv {
    struct Sample {
        using storage = float;
        storage value{};

        constexpr Sample() = default;

        template<typename T>
        requires std::is_arithmetic_v<std::remove_cvref_t<T>>
        constexpr Sample(T v)
        : value(static_cast<float>(v))
        {}

        constexpr operator float() const noexcept
        {
            return value;
        }

        constexpr Sample& operator+=(Sample other) noexcept { value += other.value; return *this; }
        constexpr Sample& operator-=(Sample other) noexcept { value -= other.value; return *this; }
        constexpr Sample& operator*=(Sample other) noexcept { value *= other.value; return *this; }
        constexpr Sample& operator/=(Sample other) noexcept { value /= other.value; return *this; }

        friend constexpr Sample operator+(Sample a, Sample b) noexcept { return a.value + b.value; }
        friend constexpr Sample operator-(Sample a, Sample b) noexcept { return a.value - b.value; }
        friend constexpr Sample operator*(Sample a, Sample b) noexcept { return a.value * b.value; }
        friend constexpr Sample operator/(Sample a, Sample b) noexcept { return a.value / b.value; }

        template<typename T>
        requires std::is_arithmetic_v<std::remove_cvref_t<T>>
        friend constexpr Sample operator+(Sample a, T b) noexcept { return a.value + static_cast<float>(b); }
        template<typename T>
        requires std::is_arithmetic_v<std::remove_cvref_t<T>>
        friend constexpr Sample operator-(Sample a, T b) noexcept { return a.value - static_cast<float>(b); }
        template<typename T>
        requires std::is_arithmetic_v<std::remove_cvref_t<T>>
        friend constexpr Sample operator*(Sample a, T b) noexcept { return a.value * static_cast<float>(b); }
        template<typename T>
        requires std::is_arithmetic_v<std::remove_cvref_t<T>>
        friend constexpr Sample operator/(Sample a, T b) noexcept { return a.value / static_cast<float>(b); }

        template<typename T>
        requires std::is_arithmetic_v<std::remove_cvref_t<T>>
        friend constexpr Sample operator+(T a, Sample b) noexcept { return static_cast<float>(a) + b.value; }
        template<typename T>
        requires std::is_arithmetic_v<std::remove_cvref_t<T>>
        friend constexpr Sample operator-(T a, Sample b) noexcept { return static_cast<float>(a) - b.value; }
        template<typename T>
        requires std::is_arithmetic_v<std::remove_cvref_t<T>>
        friend constexpr Sample operator*(T a, Sample b) noexcept { return static_cast<float>(a) * b.value; }
        template<typename T>
        requires std::is_arithmetic_v<std::remove_cvref_t<T>>
        friend constexpr Sample operator/(T a, Sample b) noexcept { return static_cast<float>(a) / b.value; }

        constexpr Sample& operator++() noexcept
        {
            ++value;
            return *this;
        }

        constexpr Sample operator++(int) noexcept
        {
            Sample tmp = *this;
            ++(*this);
            return tmp;
        }

        constexpr Sample& operator--() noexcept
        {
            --value;
            return *this;
        }

        constexpr Sample operator--(int) noexcept
        {
            Sample tmp = *this;
            --(*this);
            return tmp;
        }
    };
}
