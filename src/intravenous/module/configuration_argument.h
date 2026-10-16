#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace iv::details {
// This identity is populated by the IV Clang plugin/finalizer for every
// transported configuration type.  The address is package-local, so pointer
// equality would be wrong: independently compiled packages intentionally have
// distinct descriptors for the same C++ type.  The two compiler-produced
// strings establish both nominal identity and the definition fingerprint.
struct ConfigurationTypeIdentity {
    char const* nominal_id = nullptr;
    std::size_t nominal_id_size = 0;
    char const* definition_fingerprint = nullptr;
    std::size_t definition_fingerprint_size = 0;
    // Human-readable canonical spelling is diagnostic-only. It never decides
    // compatibility; `nominal_id` and `definition_fingerprint` do.
    char const* display_name = nullptr;
    std::size_t display_name_size = 0;
};
static_assert(std::is_standard_layout_v<ConfigurationTypeIdentity>);
static_assert(std::is_trivially_copyable_v<ConfigurationTypeIdentity>);

#if defined(__clang__)
#define IV_CONFIGURATION_TYPE_IDENTITY_STORAGE \
    __attribute__((used, section("iv_configuration_type_identities")))
#else
#error "IV package configuration identities require Clang"
#endif

// The plugin recognizes and records every materialized specialization. The
// finalizer replaces this zero initializer with the compiler-produced stable
// identity strings before the package enters ORC.
template<class T>
IV_CONFIGURATION_TYPE_IDENTITY_STORAGE inline ConfigurationTypeIdentity
    configuration_type_identity_storage{};

#undef IV_CONFIGURATION_TYPE_IDENTITY_STORAGE

template<class T>
constexpr ConfigurationTypeIdentity const* configuration_type_identity() noexcept
{
    using Transported = std::remove_cvref_t<T>;
    return std::addressof(configuration_type_identity_storage<Transported>);
}


struct ConfigurationValueOperations {
    std::size_t size = 0;
    std::size_t alignment = 1;
    void (*copy_construct)(void*, void const*) = nullptr;
    void (*destroy)(void*) noexcept = nullptr;
    bool (*equal)(void const*, void const*) = nullptr;
    std::size_t (*hash)(void const*) = nullptr;
};
static_assert(std::is_standard_layout_v<ConfigurationValueOperations>);
static_assert(std::is_trivially_copyable_v<ConfigurationValueOperations>);

template<class T>
struct ConfigurationValueOperationsStorage {
    using Value = std::remove_cvref_t<T>;

    static void copy_construct(void* destination, void const* source)
        requires std::copy_constructible<Value>
    {
        ::new (destination) Value(*static_cast<Value const*>(source));
    }

    static void destroy(void* value) noexcept
    {
        static_cast<Value*>(value)->~Value();
    }

    static bool equal(void const* lhs, void const* rhs)
        requires requires(Value const& a, Value const& b) {
            { a == b } -> std::convertible_to<bool>;
        }
    {
        return *static_cast<Value const*>(lhs) == *static_cast<Value const*>(rhs);
    }

    static std::size_t hash(void const* value)
        requires requires(Value const& v) {
            { std::hash<Value>{}(v) } -> std::convertible_to<std::size_t>;
        }
    {
        return std::hash<Value>{}(*static_cast<Value const*>(value));
    }

    inline static constexpr ConfigurationValueOperations value{
        .size = sizeof(Value),
        .alignment = alignof(Value),
        .copy_construct = [] {
            if constexpr (std::copy_constructible<Value>) {
                return &copy_construct;
            } else {
                return static_cast<void (*)(void*, void const*)>(nullptr);
            }
        }(),
        .destroy = &destroy,
        .equal = [] {
            if constexpr (requires(Value const& a, Value const& b) {
                { a == b } -> std::convertible_to<bool>;
            }) {
                return &equal;
            } else {
                return static_cast<bool (*)(void const*, void const*)>(nullptr);
            }
        }(),
        .hash = [] {
            if constexpr (requires(Value const& v) {
                { std::hash<Value>{}(v) } -> std::convertible_to<std::size_t>;
            }) {
                return &hash;
            } else {
                return static_cast<std::size_t (*)(void const*)>(nullptr);
            }
        }(),
    };
};

template<class T>
constexpr ConfigurationValueOperations const* configuration_value_operations() noexcept
{
    return std::addressof(ConfigurationValueOperationsStorage<std::remove_cvref_t<T>>::value);
}

struct ConfigurationArgument {
    void* data = nullptr;
    ConfigurationTypeIdentity const* type = nullptr;
    bool is_const = false;
    bool is_rvalue = false;
    // Array arguments use C++'s built-in array-to-pointer conversion before
    // signature matching. `data` then is the pointer value itself rather than
    // the address of a pointer object.
    bool is_array_decay = false;
};
static_assert(std::is_standard_layout_v<ConfigurationArgument>);
static_assert(std::is_trivially_copyable_v<ConfigurationArgument>);

template<class T>
constexpr ConfigurationArgument configuration_argument(T&& value) noexcept
{
    using Original = std::remove_reference_t<T>;
    if constexpr (std::is_array_v<Original>) {
        using Transported = std::decay_t<T>;
        return {
            .data = const_cast<void*>(
                static_cast<void const*>(std::addressof(value))),
            .type = configuration_type_identity<Transported>(),
            .is_const = std::is_const_v<Original>,
            .is_rvalue = false,
            .is_array_decay = true,
        };
    } else {
        static_assert(!std::is_function_v<Original>,
            "pass a function pointer explicitly to g.node<Id>(...) rather than a function");
        return {
            .data = const_cast<void*>(static_cast<void const*>(std::addressof(value))),
            .type = configuration_type_identity<T>(),
            .is_const = std::is_const_v<Original>,
            .is_rvalue = !std::is_lvalue_reference_v<T>,
            .is_array_decay = false,
        };
    }
}

template<class... T>
constexpr auto configuration_arguments(T&&... values) noexcept
{
    return std::array<ConfigurationArgument, sizeof...(T)>{
        configuration_argument(std::forward<T>(values))...};
}

inline std::string_view configuration_type_nominal_id(
    ConfigurationTypeIdentity const* identity) noexcept
{
    return identity && identity->nominal_id
        ? std::string_view(identity->nominal_id, identity->nominal_id_size)
        : std::string_view{};
}

inline std::string_view configuration_type_definition_fingerprint(
    ConfigurationTypeIdentity const* identity) noexcept
{
    return identity && identity->definition_fingerprint
        ? std::string_view(
              identity->definition_fingerprint,
              identity->definition_fingerprint_size)
        : std::string_view{};
}

inline std::string_view configuration_type_display_name(
    ConfigurationTypeIdentity const* identity) noexcept
{
    return identity && identity->display_name
        ? std::string_view(identity->display_name, identity->display_name_size)
        : std::string_view{};
}

inline bool same_configuration_type(
    ConfigurationTypeIdentity const* lhs,
    ConfigurationTypeIdentity const* rhs) noexcept
{
    return lhs && rhs
        && configuration_type_nominal_id(lhs) == configuration_type_nominal_id(rhs)
        && configuration_type_definition_fingerprint(lhs)
            == configuration_type_definition_fingerprint(rhs)
        && !configuration_type_nominal_id(lhs).empty()
        && !configuration_type_definition_fingerprint(lhs).empty();
}
}
