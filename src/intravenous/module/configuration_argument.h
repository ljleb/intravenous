#pragma once

#include <intravenous/node/address.h>

#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace iv::details {
enum class ConfigurationExpressionTypeKind : unsigned char {
    unsupported,
    boolean,
    signed_integer,
    unsigned_integer,
    floating_point,
    string,
    converted_scalar,
    enumeration,
    aggregate,
    sequence,
    node_reference,
    node_address,
};

enum class ConfigurationExpressionScalarKind : unsigned char {
    boolean,
    signed_integer,
    unsigned_integer,
    floating_point,
    string,
};

struct ConfigurationExpressionScalar {
    ConfigurationExpressionScalarKind kind =
        ConfigurationExpressionScalarKind::signed_integer;
    bool boolean = false;
    std::int64_t signed_integer = 0;
    std::uint64_t unsigned_integer = 0;
    double floating_point = 0.0;
    char const* string_data = nullptr;
    std::size_t string_size = 0;
};

struct ConfigurationExpressionTypeSchema;

struct ConfigurationExpressionFieldSchema {
    char const* name = nullptr;
    std::size_t name_size = 0;
    std::size_t byte_offset = 0;
    ConfigurationExpressionTypeSchema const* type = nullptr;
    void (*assign)(void*, void*) = nullptr;
};

struct ConfigurationExpressionEnumeratorSchema {
    char const* name = nullptr;
    std::size_t name_size = 0;
    std::int64_t signed_value = 0;
    std::uint64_t unsigned_value = 0;
};

// This schema is immutable package data emitted by the Clang metadata plugin
// and package finalizer. All callbacks execute provider-compiled C++ for the
// exact represented type; the app runtime never guesses an erased C++ layout.
struct ConfigurationExpressionTypeSchema {
    ConfigurationExpressionTypeKind kind =
        ConfigurationExpressionTypeKind::unsupported;
    std::size_t size = 0;
    std::size_t alignment = 1;
    bool integer_is_signed = false;
    std::size_t integer_bit_width = 0;
    ConfigurationExpressionFieldSchema const* fields = nullptr;
    std::size_t field_count = 0;
    ConfigurationExpressionEnumeratorSchema const* enumerators = nullptr;
    std::size_t enumerator_count = 0;
    ConfigurationExpressionTypeSchema const* element_type = nullptr;
    std::size_t fixed_element_count = 0;
    void (*construct_scalar)(void*, ConfigurationExpressionScalar const&) = nullptr;
    void (*read_scalar)(void const*, ConfigurationExpressionScalar&) = nullptr;
    void (*default_construct)(void*) = nullptr;
    void (*destroy)(void*) noexcept = nullptr;
    void (*assign_element)(void*, std::size_t, void*) = nullptr;
    void (*append_element)(void*, void*) = nullptr;
    std::size_t (*element_count)(void const*) = nullptr;
    void const* (*element_at)(void const*, std::size_t) = nullptr;
    void (*construct_node_address)(void*, NodeAddress const&) = nullptr;
};
static_assert(std::is_standard_layout_v<ConfigurationExpressionTypeSchema>);
static_assert(std::is_trivially_copyable_v<ConfigurationExpressionTypeSchema>);

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
    ConfigurationExpressionTypeSchema const* expression_schema = nullptr;
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

namespace configuration_expression_details {
template<class T>
[[gnu::used]]
void construct_scalar(void* destination, ConfigurationExpressionScalar const& source)
{
    using Value = std::remove_cvref_t<T>;
    if constexpr (std::same_as<Value, bool>) {
        if (source.kind != ConfigurationExpressionScalarKind::boolean)
            throw std::invalid_argument("configuration value is not Boolean");
        ::new (destination) Value(source.boolean);
    } else if constexpr (std::is_enum_v<Value>) {
        using Underlying = std::underlying_type_t<Value>;
        if constexpr (std::is_signed_v<Underlying>) {
            if (source.kind != ConfigurationExpressionScalarKind::signed_integer)
                throw std::invalid_argument("configuration enum value has the wrong representation");
            ::new (destination) Value(static_cast<Value>(
                static_cast<Underlying>(source.signed_integer)));
        } else {
            if (source.kind != ConfigurationExpressionScalarKind::unsigned_integer)
                throw std::invalid_argument("configuration enum value has the wrong representation");
            ::new (destination) Value(static_cast<Value>(
                static_cast<Underlying>(source.unsigned_integer)));
        }
    } else if constexpr (std::integral<Value> && std::is_signed_v<Value>) {
        if (source.kind != ConfigurationExpressionScalarKind::signed_integer)
            throw std::invalid_argument("configuration value is not a signed integer");
        ::new (destination) Value(static_cast<Value>(source.signed_integer));
    } else if constexpr (std::integral<Value>) {
        if (source.kind != ConfigurationExpressionScalarKind::unsigned_integer)
            throw std::invalid_argument("configuration value is not an unsigned integer");
        ::new (destination) Value(static_cast<Value>(source.unsigned_integer));
    } else if constexpr (std::floating_point<Value>) {
        if (source.kind != ConfigurationExpressionScalarKind::floating_point)
            throw std::invalid_argument("configuration value is not numeric");
        auto const converted = static_cast<Value>(source.floating_point);
        if (!std::isfinite(source.floating_point) || !std::isfinite(converted))
            throw std::invalid_argument("configuration floating-point value is out of range");
        ::new (destination) Value(converted);
    } else if constexpr (std::same_as<Value, std::string>) {
        if (source.kind != ConfigurationExpressionScalarKind::string)
            throw std::invalid_argument("configuration value is not a string");
        ::new (destination) Value(source.string_data, source.string_size);
    } else if constexpr (std::constructible_from<Value, bool>
        || std::constructible_from<Value, std::int64_t>
        || std::constructible_from<Value, std::uint64_t>
        || std::constructible_from<Value, double>
        || std::constructible_from<Value, std::string_view>) {
        switch (source.kind) {
        case ConfigurationExpressionScalarKind::boolean:
            if constexpr (std::constructible_from<Value, bool>) {
                ::new (destination) Value(source.boolean);
                return;
            }
            break;
        case ConfigurationExpressionScalarKind::signed_integer:
            if constexpr (std::constructible_from<Value, std::int64_t>) {
                ::new (destination) Value(source.signed_integer);
                return;
            }
            break;
        case ConfigurationExpressionScalarKind::unsigned_integer:
            if constexpr (std::constructible_from<Value, std::uint64_t>) {
                ::new (destination) Value(source.unsigned_integer);
                return;
            }
            break;
        case ConfigurationExpressionScalarKind::floating_point:
            if constexpr (std::constructible_from<Value, double>) {
                ::new (destination) Value(source.floating_point);
                return;
            }
            break;
        case ConfigurationExpressionScalarKind::string:
            if constexpr (std::constructible_from<Value, std::string_view>) {
                ::new (destination) Value(std::string_view(
                    source.string_data, source.string_size));
                return;
            }
            break;
        }
        throw std::invalid_argument(
            "configuration scalar cannot construct the requested provider type");
    } else {
        throw std::invalid_argument(
            "configuration type does not accept a scalar expression");
    }
}

template<class T>
[[gnu::used]]
void default_construct(void* destination)
{
    using Value = std::remove_cvref_t<T>;
    if constexpr (std::default_initializable<Value>) {
        ::new (destination) Value{};
    } else {
        throw std::invalid_argument(
            "configuration type cannot be initialized from an omitted value");
    }
}

template<class T>
[[gnu::used]]
void read_scalar(void const* source, ConfigurationExpressionScalar& result)
{
    using Value = std::remove_cvref_t<T>;
    auto const& value = *static_cast<Value const*>(source);
    if constexpr (std::same_as<Value, bool>) {
        result.kind = ConfigurationExpressionScalarKind::boolean;
        result.boolean = value;
    } else if constexpr (std::is_enum_v<Value>) {
        using Underlying = std::underlying_type_t<Value>;
        if constexpr (std::is_signed_v<Underlying>) {
            result.kind = ConfigurationExpressionScalarKind::signed_integer;
            result.signed_integer = static_cast<std::int64_t>(
                static_cast<Underlying>(value));
        } else {
            result.kind = ConfigurationExpressionScalarKind::unsigned_integer;
            result.unsigned_integer = static_cast<std::uint64_t>(
                static_cast<Underlying>(value));
        }
    } else if constexpr (std::integral<Value> && std::is_signed_v<Value>) {
        result.kind = ConfigurationExpressionScalarKind::signed_integer;
        result.signed_integer = static_cast<std::int64_t>(value);
    } else if constexpr (std::integral<Value>) {
        result.kind = ConfigurationExpressionScalarKind::unsigned_integer;
        result.unsigned_integer = static_cast<std::uint64_t>(value);
    } else if constexpr (std::floating_point<Value>) {
        auto const converted = static_cast<double>(value);
        if (!std::isfinite(converted)) {
            throw std::invalid_argument(
                "configuration floating-point value is not finite");
        }
        result.kind = ConfigurationExpressionScalarKind::floating_point;
        result.floating_point = converted;
    } else if constexpr (std::same_as<Value, std::string>) {
        result.kind = ConfigurationExpressionScalarKind::string;
        result.string_data = value.data();
        result.string_size = value.size();
    } else if constexpr (requires { static_cast<std::string_view>(value); }) {
        auto const converted = static_cast<std::string_view>(value);
        result.kind = ConfigurationExpressionScalarKind::string;
        result.string_data = converted.data();
        result.string_size = converted.size();
    } else if constexpr (requires { static_cast<double>(value); }) {
        auto const converted = static_cast<double>(value);
        if (!std::isfinite(converted)) {
            throw std::invalid_argument(
                "configuration converted scalar is not finite");
        }
        result.kind = ConfigurationExpressionScalarKind::floating_point;
        result.floating_point = converted;
    } else if constexpr (requires { static_cast<std::int64_t>(value); }) {
        result.kind = ConfigurationExpressionScalarKind::signed_integer;
        result.signed_integer = static_cast<std::int64_t>(value);
    } else if constexpr (requires { static_cast<std::uint64_t>(value); }) {
        result.kind = ConfigurationExpressionScalarKind::unsigned_integer;
        result.unsigned_integer = static_cast<std::uint64_t>(value);
    } else if constexpr (requires { static_cast<bool>(value); }) {
        result.kind = ConfigurationExpressionScalarKind::boolean;
        result.boolean = static_cast<bool>(value);
    } else {
        throw std::invalid_argument(
            "configuration converted scalar has no reversible scalar representation");
    }
}

template<class T>
[[gnu::used]]
void destroy(void* value) noexcept
{
    using Value = std::remove_cvref_t<T>;
    static_cast<Value*>(value)->~Value();
}

template<class T>
[[gnu::used]]
void assign(void* destination, void* source)
{
    using Value = std::remove_cvref_t<T>;
    if constexpr (std::is_trivially_copyable_v<Value>) {
        if (destination != source) {
            std::memcpy(destination, source, sizeof(Value));
        }
    } else if constexpr (std::assignable_from<Value&, Value&&>) {
        *static_cast<Value*>(destination) = std::move(*static_cast<Value*>(source));
    } else {
        throw std::invalid_argument(
            "configuration aggregate member is not move-assignable");
    }
}

template<class T>
[[gnu::used]]
void append_element(void* destination, void* source)
{
    using Value = std::remove_cvref_t<T>;
    if constexpr (requires(Value& value, typename Value::value_type&& element) {
        value.push_back(std::move(element));
    }) {
        using Element = typename Value::value_type;
        static_cast<Value*>(destination)->push_back(
            std::move(*static_cast<Element*>(source)));
    } else {
        throw std::invalid_argument(
            "configuration sequence does not support dynamic append");
    }
}

template<class T>
[[gnu::used]]
void assign_element(void* destination, std::size_t index, void* source)
{
    using Value = std::remove_cvref_t<T>;
    if constexpr (requires(
        Value& value, std::size_t position, typename Value::value_type&& element) {
        value[position] = std::move(element);
    }) {
        using Element = typename Value::value_type;
        (*static_cast<Value*>(destination))[index] =
            std::move(*static_cast<Element*>(source));
    } else {
        throw std::invalid_argument(
            "configuration sequence does not support indexed assignment");
    }
}

template<class T>
[[gnu::used]]
std::size_t element_count(void const* source)
{
    using Value = std::remove_cvref_t<T>;
    if constexpr (requires(Value const& value) { value.size(); }) {
        return static_cast<Value const*>(source)->size();
    } else {
        throw std::invalid_argument(
            "configuration sequence has no size operation");
    }
}

template<class T>
[[gnu::used]]
void const* element_at(void const* source, std::size_t index)
{
    using Value = std::remove_cvref_t<T>;
    if constexpr (requires(Value const& value, std::size_t position) {
        std::addressof(value[position]);
    }) {
        return std::addressof((*static_cast<Value const*>(source))[index]);
    } else {
        throw std::invalid_argument(
            "configuration sequence has no indexed read operation");
    }
}

template<class T>
[[gnu::used]]
void construct_node_address(void* destination, NodeAddress const& address)
{
    using Value = std::remove_cvref_t<T>;
    if constexpr (std::same_as<Value, NodeAddress>) {
        ::new (destination) Value(address);
    } else {
        throw std::invalid_argument(
            "configuration type is not a NodeAddress");
    }
}
} // namespace configuration_expression_details

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
