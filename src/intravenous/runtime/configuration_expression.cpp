#include <intravenous/runtime/configuration_expression.h>
#include <intravenous/graph/builder.h>
#include <intravenous/runtime/project_connection_json.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

namespace iv {
namespace {
class Parser {
public:
    explicit Parser(std::string source) : result_{.source = std::move(source)} {}

    ParsedConfigurationExpressionList parse()
    {
        whitespace();
        if (done()) return std::move(result_);
        while (true) {
            result_.arguments.push_back(expression());
            whitespace();
            if (!consume(',')) break;
            whitespace();
            if (done()) fail("expected an expression after ','");
        }
        whitespace();
        if (!done()) fail("unexpected token after configuration argument list");
        return std::move(result_);
    }

private:
    ParsedConfigurationExpressionList result_;
    std::size_t position_ = 0;

    bool done() const noexcept { return position_ == result_.source.size(); }
    char peek() const noexcept { return done() ? '\0' : result_.source[position_]; }

    [[noreturn]] void fail(std::string message) const
    {
        throw std::invalid_argument(
            "configuration expression at byte " + std::to_string(position_)
            + ": " + std::move(message));
    }

    void whitespace()
    {
        while (!done() && (peek() == ' ' || peek() == '\t'
                || peek() == '\r' || peek() == '\n')) {
            ++position_;
        }
    }

    bool consume(char value)
    {
        whitespace();
        if (peek() != value) return false;
        ++position_;
        return true;
    }

    void require(char value, std::string_view message)
    {
        if (!consume(value)) fail(std::string(message));
    }

    static bool identifier_start(char value) noexcept
    {
        return (value >= 'a' && value <= 'z')
            || (value >= 'A' && value <= 'Z') || value == '_';
    }

    static bool identifier_continue(char value) noexcept
    {
        return identifier_start(value) || (value >= '0' && value <= '9');
    }

    std::string identifier()
    {
        whitespace();
        if (!identifier_start(peek())) fail("expected an identifier");
        auto const begin = position_++;
        while (identifier_continue(peek())) ++position_;
        return result_.source.substr(begin, position_ - begin);
    }

    ConfigurationExpressionPtr expression() { return additive(); }

    ConfigurationExpressionPtr additive()
    {
        auto result = multiplicative();
        while (true) {
            whitespace();
            auto const operation = peek();
            if (operation != '+' && operation != '-') break;
            ++position_;
            auto right = multiplicative();
            result = node(result->source_begin, right->source_end,
                ConfigurationBinaryExpression{
                    .operation = operation,
                    .left = std::move(result),
                    .right = std::move(right),
                });
        }
        return result;
    }

    ConfigurationExpressionPtr multiplicative()
    {
        auto result = unary();
        while (true) {
            whitespace();
            auto const operation = peek();
            if (operation != '*' && operation != '/' && operation != '%') break;
            ++position_;
            auto right = unary();
            result = node(result->source_begin, right->source_end,
                ConfigurationBinaryExpression{
                    .operation = operation,
                    .left = std::move(result),
                    .right = std::move(right),
                });
        }
        return result;
    }

    ConfigurationExpressionPtr unary()
    {
        whitespace();
        auto const begin = position_;
        if (peek() == '+' || peek() == '-') {
            auto const operation = peek();
            ++position_;
            auto operand = unary();
            return node(begin, operand->source_end, ConfigurationUnaryExpression{
                .operation = operation,
                .operand = std::move(operand),
            });
        }
        return primary();
    }

    ConfigurationExpressionPtr primary()
    {
        whitespace();
        auto const begin = position_;
        if (consume('(')) {
            auto result = expression();
            require(')', "expected ')' after expression");
            return node(begin, position_, result->value);
        }
        if (peek() == '{') return aggregate();
        if (peek() == '"') return string_literal();
        if (peek() == '&') return node_reference();
        if ((peek() >= '0' && peek() <= '9') || peek() == '.')
            return number_literal();
        if (identifier_start(peek())) {
            auto name = identifier();
            if (name == "true") return node(begin, position_, true);
            if (name == "false") return node(begin, position_, false);
            whitespace();
            if (consume('(')) {
                ConfigurationCallExpression call{.function = std::move(name)};
                whitespace();
                if (!consume(')')) {
                    while (true) {
                        call.arguments.push_back(expression());
                        if (consume(')')) break;
                        require(',', "expected ',' or ')' in function call");
                    }
                }
                return node(begin, position_, std::move(call));
            }
            return node(begin, position_,
                ConfigurationIdentifierExpression{.value = std::move(name)});
        }
        if (peek() == '#') fail("a tiled-child selector requires a node-ID subject");
        fail("expected a configuration expression");
    }

    ConfigurationExpressionPtr number_literal()
    {
        whitespace();
        auto const begin = position_;
        bool digits = false;
        while (peek() >= '0' && peek() <= '9') {
            digits = true;
            ++position_;
        }
        if (peek() == '.') {
            ++position_;
            while (peek() >= '0' && peek() <= '9') {
                digits = true;
                ++position_;
            }
        }
        if (!digits) fail("expected digits in numeric literal");
        if (peek() == 'e' || peek() == 'E') {
            ++position_;
            if (peek() == '+' || peek() == '-') ++position_;
            auto const exponent_begin = position_;
            while (peek() >= '0' && peek() <= '9') ++position_;
            if (position_ == exponent_begin) fail("expected exponent digits");
        }
        return node(begin, position_, ConfigurationNumberLiteral{
            .source = result_.source.substr(begin, position_ - begin),
        });
    }

    ConfigurationExpressionPtr string_literal()
    {
        whitespace();
        auto const begin = position_;
        require('"', "expected string literal");
        std::string value;
        while (!done() && peek() != '"') {
            auto character = result_.source[position_++];
            if (character != '\\') {
                value.push_back(character);
                continue;
            }
            if (done()) fail("unfinished string escape");
            switch (result_.source[position_++]) {
            case '\\': value.push_back('\\'); break;
            case '"': value.push_back('"'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            case '0': value.push_back('\0'); break;
            default: fail("unsupported string escape");
            }
        }
        require('"', "unterminated string literal");
        return node(begin, position_,
            ConfigurationStringLiteral{.value = std::move(value)});
    }

    ConfigurationExpressionPtr aggregate()
    {
        whitespace();
        auto const begin = position_;
        require('{', "expected '{'");
        ConfigurationAggregateExpression aggregate;
        whitespace();
        if (!consume('}')) {
            while (true) {
                std::optional<std::string> name;
                whitespace();
                if (peek() == '.') {
                    ++position_;
                    name = identifier();
                    require('=', "expected '=' after aggregate member name");
                }
                aggregate.elements.push_back({
                    .name = std::move(name),
                    .value = expression(),
                });
                if (consume('}')) break;
                require(',', "expected ',' or '}' in aggregate expression");
                if (consume('}')) break;
            }
        }
        return node(begin, position_, std::move(aggregate));
    }

    static bool hexadecimal(char value) noexcept
    {
        return (value >= '0' && value <= '9')
            || (value >= 'a' && value <= 'f')
            || (value >= 'A' && value <= 'F');
    }

    static bool canonical_hexadecimal(char value) noexcept
    {
        return (value >= '0' && value <= '9')
            || (value >= 'a' && value <= 'f');
    }

    ConfigurationExpressionPtr node_reference()
    {
        whitespace();
        auto const begin = position_;
        require('&', "expected '&'");
        auto const id_begin = position_;
        while (hexadecimal(peek()) || peek() == '-') ++position_;
        auto id = result_.source.substr(id_begin, position_ - id_begin);
        constexpr std::array<std::size_t, 4> hyphens{8, 13, 18, 23};
        if (id.size() != 36) fail("node ID must use the canonical UUID form");
        for (std::size_t index = 0; index < id.size(); ++index) {
            auto const expected_hyphen = std::ranges::contains(hyphens, index);
            if ((expected_hyphen && id[index] != '-')
                || (!expected_hyphen && !canonical_hexadecimal(id[index]))) {
                fail("node ID must use the canonical UUID form");
            }
        }

        ConfigurationNodeReferenceExpression reference{.instance_id = std::move(id)};
        while (true) {
            whitespace();
            if (consume('.')) {
                reference.path.push_back(ProjectVirtualNodeSelector{
                    .source_identity = identifier(),
                });
            } else if (consume('[')) {
                whitespace();
                auto const index_begin = position_;
                while (peek() >= '0' && peek() <= '9') ++position_;
                if (index_begin == position_) fail("expected a virtual member index");
                std::size_t index = 0;
                auto const text = std::string_view(result_.source).substr(
                    index_begin, position_ - index_begin);
                auto const parsed = std::from_chars(
                    text.data(), text.data() + text.size(), index);
                if (parsed.ec != std::errc{}) fail("virtual member index is too large");
                require(']', "expected ']' after virtual member index");
                reference.path.push_back(ProjectVirtualMemberSelector{.index = index});
            } else if (consume('#')) {
                reference.path.push_back(ProjectTiledChildSelector{
                    .name = identifier(),
                });
            } else {
                break;
            }
        }
        return node(begin, position_, std::move(reference));
    }

    template<class Value>
    ConfigurationExpressionPtr node(
        std::size_t begin,
        std::size_t end,
        Value value)
    {
        return std::make_shared<ConfigurationExpression const>(ConfigurationExpression{
            .source_begin = begin,
            .source_end = end,
            .value = std::move(value),
        });
    }
};

struct NumericValue {
    long double value = 0.0L;
    bool integral = true;
};

NumericValue numeric_value(long double value)
{
    return {value, std::isfinite(value) && std::trunc(value) == value};
}

[[noreturn]] void evaluation_error(
    ConfigurationExpression const& expression,
    std::string message)
{
    throw std::invalid_argument(
        "configuration expression at byte "
        + std::to_string(expression.source_begin) + ": " + std::move(message));
}

NumericValue numeric(ConfigurationExpression const& expression);

template<class Operation>
void provider_operation(
    ConfigurationExpression const& expression,
    Operation&& operation)
{
    try {
        std::forward<Operation>(operation)();
    } catch (std::invalid_argument const& error) {
        evaluation_error(expression, error.what());
    }
}

std::vector<NumericValue> numeric_arguments(
    ConfigurationCallExpression const& call,
    ConfigurationExpression const& expression)
{
    std::vector<NumericValue> result;
    result.reserve(call.arguments.size());
    for (auto const& argument : call.arguments) {
        if (!argument) evaluation_error(expression, "function has an empty argument");
        result.push_back(numeric(*argument));
    }
    return result;
}

NumericValue numeric(ConfigurationExpression const& expression)
{
    return std::visit([&](auto const& value) -> NumericValue {
        using Value = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::same_as<Value, ConfigurationNumberLiteral>) {
            long double result = 0.0L;
            auto const parsed = std::from_chars(
                value.source.data(), value.source.data() + value.source.size(), result);
            if (parsed.ec != std::errc{} || parsed.ptr != value.source.data() + value.source.size())
                evaluation_error(expression, "invalid numeric literal");
            return numeric_value(result);
        } else if constexpr (std::same_as<Value, ConfigurationUnaryExpression>) {
            auto result = numeric(*value.operand);
            if (value.operation == '-') result.value = -result.value;
            return result;
        } else if constexpr (std::same_as<Value, ConfigurationBinaryExpression>) {
            auto left = numeric(*value.left);
            auto right = numeric(*value.right);
            switch (value.operation) {
            case '+': return numeric_value(left.value + right.value);
            case '-': return numeric_value(left.value - right.value);
            case '*': return numeric_value(left.value * right.value);
            case '/':
                if (right.value == 0.0L) evaluation_error(expression, "division by zero");
                return numeric_value(left.value / right.value);
            case '%':
                if (!left.integral || !right.integral)
                    evaluation_error(expression, "remainder operands must be integers");
                if (right.value == 0.0L) evaluation_error(expression, "remainder by zero");
                return numeric_value(std::fmod(left.value, right.value));
            default: evaluation_error(expression, "unknown arithmetic operation");
            }
        } else if constexpr (std::same_as<Value, ConfigurationCallExpression>) {
            auto arguments = numeric_arguments(value, expression);
            auto arity = [&](std::size_t count) {
                if (arguments.size() != count) {
                    evaluation_error(expression,
                        "function '" + value.function + "' requires "
                        + std::to_string(count) + " arguments");
                }
            };
            auto unary = [&](auto function) {
                arity(1);
                return numeric_value(function(arguments[0].value));
            };
            if (value.function == "abs") return unary([](auto x) { return std::fabs(x); });
            if (value.function == "sqrt") return unary([](auto x) { return std::sqrt(x); });
            if (value.function == "cbrt") return unary([](auto x) { return std::cbrt(x); });
            if (value.function == "exp") return unary([](auto x) { return std::exp(x); });
            if (value.function == "log") return unary([](auto x) { return std::log(x); });
            if (value.function == "log2") return unary([](auto x) { return std::log2(x); });
            if (value.function == "log10") return unary([](auto x) { return std::log10(x); });
            if (value.function == "sin") return unary([](auto x) { return std::sin(x); });
            if (value.function == "cos") return unary([](auto x) { return std::cos(x); });
            if (value.function == "tan") return unary([](auto x) { return std::tan(x); });
            if (value.function == "floor") return unary([](auto x) { return std::floor(x); });
            if (value.function == "ceil") return unary([](auto x) { return std::ceil(x); });
            if (value.function == "round") return unary([](auto x) { return std::round(x); });
            if (value.function == "pow") {
                arity(2);
                return numeric_value(
                    std::pow(arguments[0].value, arguments[1].value));
            }
            if (value.function == "min" || value.function == "max") {
                if (arguments.empty())
                    evaluation_error(expression, "min/max requires at least one argument");
                auto result = arguments.front();
                for (auto const& argument : arguments | std::views::drop(1)) {
                    result.value = value.function == "min"
                        ? std::min(result.value, argument.value)
                        : std::max(result.value, argument.value);
                    result.integral = std::isfinite(result.value)
                        && std::trunc(result.value) == result.value;
                }
                return result;
            }
            if (value.function == "clamp") {
                arity(3);
                if (arguments[1].value > arguments[2].value)
                    evaluation_error(expression, "clamp lower bound exceeds upper bound");
                return numeric_value(std::clamp(
                    arguments[0].value,
                    arguments[1].value,
                    arguments[2].value));
            }
            evaluation_error(expression,
                "function '" + value.function + "' is not allowlisted");
        } else {
            evaluation_error(expression, "expected a numeric expression");
        }
    }, expression.value);
}

class OwnedValue {
public:
    explicit OwnedValue(details::ConfigurationExpressionTypeSchema const& schema)
        : schema_(&schema)
    {
        if (schema.size == 0 || schema.alignment == 0)
            throw std::invalid_argument("configuration type schema has an invalid layout");
        storage_ = ::operator new(schema.size, std::align_val_t{schema.alignment});
    }
    ~OwnedValue()
    {
        if (constructed_ && schema_->destroy) schema_->destroy(storage_);
        ::operator delete(storage_, std::align_val_t{schema_->alignment});
    }
    OwnedValue(OwnedValue const&) = delete;
    OwnedValue& operator=(OwnedValue const&) = delete;
    OwnedValue(OwnedValue&& other) noexcept
        : schema_(other.schema_), storage_(std::exchange(other.storage_, nullptr)),
          constructed_(std::exchange(other.constructed_, false))
    {}
    OwnedValue& operator=(OwnedValue&&) = delete;
    void* data() const noexcept { return storage_; }
    void mark_constructed() noexcept { constructed_ = true; }

private:
    details::ConfigurationExpressionTypeSchema const* schema_;
    void* storage_ = nullptr;
    bool constructed_ = false;
};

std::string_view field_name(details::ConfigurationExpressionFieldSchema const& field)
{
    return {field.name, field.name_size};
}

void construct_value(
    OwnedValue& output,
    details::ConfigurationExpressionTypeSchema const& schema,
    ConfigurationExpression const& expression);

void scalar_construct(
    OwnedValue& output,
    details::ConfigurationExpressionTypeSchema const& schema,
    details::ConfigurationExpressionScalar scalar,
    ConfigurationExpression const& expression)
{
    if (!schema.construct_scalar)
        evaluation_error(expression, "configuration type has no scalar constructor");
    provider_operation(expression, [&] {
        schema.construct_scalar(output.data(), scalar);
    });
    output.mark_constructed();
}

void construct_value(
    OwnedValue& output,
    details::ConfigurationExpressionTypeSchema const& schema,
    ConfigurationExpression const& expression)
{
    using Kind = details::ConfigurationExpressionTypeKind;
    if (schema.kind == Kind::boolean) {
        auto const* value = std::get_if<bool>(&expression.value);
        if (!value) evaluation_error(expression, "expected a Boolean literal");
        scalar_construct(output, schema, {
            .kind = details::ConfigurationExpressionScalarKind::boolean,
            .boolean = *value,
        }, expression);
        return;
    }
    if (schema.kind == Kind::signed_integer
        || schema.kind == Kind::unsigned_integer
        || schema.kind == Kind::floating_point) {
        auto const value = numeric(expression);
        if (!std::isfinite(value.value))
            evaluation_error(expression, "numeric expression is not finite");
        if (schema.kind == Kind::floating_point) {
            auto const converted = static_cast<double>(value.value);
            if (!std::isfinite(converted))
                evaluation_error(expression, "floating-point value is out of range");
            scalar_construct(output, schema, {
                .kind = details::ConfigurationExpressionScalarKind::floating_point,
                .floating_point = converted,
            }, expression);
            return;
        }
        if (!value.integral || std::trunc(value.value) != value.value)
            evaluation_error(expression, "integer configuration value is not integral");
        if (schema.kind == Kind::signed_integer) {
            auto const bits = schema.integer_bit_width;
            auto const minimum = bits >= 64
                ? static_cast<long double>(std::numeric_limits<std::int64_t>::min())
                : -std::ldexp(1.0L, static_cast<int>(bits - 1));
            auto const maximum = bits >= 64
                ? static_cast<long double>(std::numeric_limits<std::int64_t>::max())
                : std::ldexp(1.0L, static_cast<int>(bits - 1)) - 1.0L;
            if (value.value < minimum || value.value > maximum)
                evaluation_error(expression, "signed integer is out of range");
            scalar_construct(output, schema, {
                .kind = details::ConfigurationExpressionScalarKind::signed_integer,
                .signed_integer = static_cast<std::int64_t>(value.value),
            }, expression);
        } else {
            auto const maximum = schema.integer_bit_width >= 64
                ? static_cast<long double>(std::numeric_limits<std::uint64_t>::max())
                : std::ldexp(1.0L, static_cast<int>(schema.integer_bit_width)) - 1.0L;
            if (value.value < 0.0L || value.value > maximum)
                evaluation_error(expression, "unsigned integer is out of range");
            scalar_construct(output, schema, {
                .kind = details::ConfigurationExpressionScalarKind::unsigned_integer,
                .unsigned_integer = static_cast<std::uint64_t>(value.value),
            }, expression);
        }
        return;
    }
    if (schema.kind == Kind::string) {
        auto const* value = std::get_if<ConfigurationStringLiteral>(&expression.value);
        if (!value) evaluation_error(expression, "expected a string literal");
        scalar_construct(output, schema, {
            .kind = details::ConfigurationExpressionScalarKind::string,
            .string_data = value->value.data(),
            .string_size = value->value.size(),
        }, expression);
        return;
    }
    if (schema.kind == Kind::converted_scalar) {
        details::ConfigurationExpressionScalar scalar;
        if (auto const* value = std::get_if<bool>(&expression.value)) {
            scalar.kind = details::ConfigurationExpressionScalarKind::boolean;
            scalar.boolean = *value;
        } else if (auto const* value =
                       std::get_if<ConfigurationStringLiteral>(&expression.value)) {
            scalar.kind = details::ConfigurationExpressionScalarKind::string;
            scalar.string_data = value->value.data();
            scalar.string_size = value->value.size();
        } else {
            auto const numeric_result = numeric(expression);
            if (!std::isfinite(numeric_result.value))
                evaluation_error(expression, "numeric expression is not finite");
            if (numeric_result.integral
                && numeric_result.value >= static_cast<long double>(
                    std::numeric_limits<std::int64_t>::min())
                && numeric_result.value <= static_cast<long double>(
                    std::numeric_limits<std::int64_t>::max())) {
                scalar.kind = details::ConfigurationExpressionScalarKind::signed_integer;
                scalar.signed_integer = static_cast<std::int64_t>(
                    numeric_result.value);
            } else if (numeric_result.integral && numeric_result.value >= 0.0L
                && numeric_result.value <= static_cast<long double>(
                    std::numeric_limits<std::uint64_t>::max())) {
                scalar.kind =
                    details::ConfigurationExpressionScalarKind::unsigned_integer;
                scalar.unsigned_integer = static_cast<std::uint64_t>(
                    numeric_result.value);
            } else {
                auto const converted = static_cast<double>(numeric_result.value);
                if (!std::isfinite(converted))
                    evaluation_error(expression, "floating-point value is out of range");
                scalar.kind = details::ConfigurationExpressionScalarKind::floating_point;
                scalar.floating_point = converted;
            }
        }
        scalar_construct(output, schema, scalar, expression);
        return;
    }
    if (schema.kind == Kind::enumeration) {
        auto const* identifier = std::get_if<ConfigurationIdentifierExpression>(
            &expression.value);
        if (!identifier) evaluation_error(expression, "expected an enum identifier");
        auto const found = std::ranges::find_if(
            std::span(schema.enumerators, schema.enumerator_count),
            [&](auto const& item) {
                return std::string_view(item.name, item.name_size) == identifier->value;
            });
        if (found == std::span(schema.enumerators, schema.enumerator_count).end())
            evaluation_error(expression,
                "unknown enumerator '" + identifier->value + "'");
        details::ConfigurationExpressionScalar scalar;
        if (schema.integer_is_signed) {
            scalar.kind = details::ConfigurationExpressionScalarKind::signed_integer;
            scalar.signed_integer = found->signed_value;
        } else {
            scalar.kind = details::ConfigurationExpressionScalarKind::unsigned_integer;
            scalar.unsigned_integer = found->unsigned_value;
        }
        scalar_construct(output, schema, scalar, expression);
        return;
    }
    if (schema.kind == Kind::aggregate) {
        auto const* aggregate = std::get_if<ConfigurationAggregateExpression>(
            &expression.value);
        if (!aggregate) evaluation_error(expression, "expected an aggregate expression");
        if (!schema.default_construct)
            evaluation_error(expression, "aggregate is not default-initializable");
        provider_operation(expression, [&] {
            schema.default_construct(output.data());
        });
        output.mark_constructed();
        std::vector<bool> assigned(schema.field_count, false);
        std::size_t positional = 0;
        for (auto const& element : aggregate->elements) {
            std::size_t index = 0;
            if (element.name) {
                auto fields = std::span(schema.fields, schema.field_count);
                auto found = std::ranges::find_if(fields, [&](auto const& field) {
                    return field_name(field) == *element.name;
                });
                if (found == fields.end())
                    evaluation_error(*element.value,
                        "unknown aggregate member '" + *element.name + "'");
                index = static_cast<std::size_t>(found - fields.begin());
            } else {
                while (positional < assigned.size() && assigned[positional]) ++positional;
                index = positional++;
                if (index >= schema.field_count)
                    evaluation_error(*element.value, "too many aggregate elements");
            }
            if (assigned[index])
                evaluation_error(*element.value, "aggregate member is initialized twice");
            auto const& field = schema.fields[index];
            if (!field.type || !field.assign)
                evaluation_error(*element.value,
                    "aggregate member cannot be initialized by this language");
            OwnedValue member(*field.type);
            construct_value(member, *field.type, *element.value);
            auto* destination = static_cast<std::byte*>(output.data())
                + field.byte_offset;
            provider_operation(*element.value, [&] {
                field.assign(destination, member.data());
            });
            assigned[index] = true;
        }
        return;
    }
    if (schema.kind == Kind::sequence) {
        auto const* sequence = std::get_if<ConfigurationAggregateExpression>(
            &expression.value);
        if (!sequence) evaluation_error(expression, "expected a sequence expression");
        if (!schema.default_construct || !schema.element_type)
            evaluation_error(expression, "sequence metadata is incomplete");
        provider_operation(expression, [&] {
            schema.default_construct(output.data());
        });
        output.mark_constructed();
        if (schema.fixed_element_count != 0
            && sequence->elements.size() > schema.fixed_element_count) {
            evaluation_error(expression, "too many fixed-sequence elements");
        }
        for (std::size_t index = 0; index < sequence->elements.size(); ++index) {
            auto const& element = sequence->elements[index];
            if (element.name)
                evaluation_error(*element.value, "sequence elements cannot be named");
            OwnedValue value(*schema.element_type);
            construct_value(value, *schema.element_type, *element.value);
            if (schema.fixed_element_count != 0) {
                if (!schema.assign_element)
                    evaluation_error(*element.value,
                        "fixed sequence has no element assignment operation");
                provider_operation(*element.value, [&] {
                    schema.assign_element(output.data(), index, value.data());
                });
            } else {
                if (!schema.append_element)
                    evaluation_error(*element.value,
                        "sequence has no append operation");
                provider_operation(*element.value, [&] {
                    schema.append_element(output.data(), value.data());
                });
            }
        }
        return;
    }
    if (schema.kind == Kind::node_address) {
        auto const* address = std::get_if<ConfigurationNodeReferenceExpression>(
            &expression.value);
        if (!address) {
            evaluation_error(expression, "expected a semantic node address");
        }
        if (!schema.construct_node_address) {
            evaluation_error(
                expression,
                "NodeAddress metadata has no provider constructor");
        }
        provider_operation(expression, [&] {
            schema.construct_node_address(output.data(), NodeAddress{
                .instance_id = address->instance_id,
                .path = address->path,
            });
        });
        output.mark_constructed();
        return;
    }
    if (schema.kind == Kind::node_reference) {
        if (!std::holds_alternative<ConfigurationNodeReferenceExpression>(expression.value))
            evaluation_error(expression, "expected a project-node reference");
        // Direct NodeRef parameters are materialized by the caller's session
        // before reaching this schema-driven constructor.
        evaluation_error(expression,
            "a project-node reference can be materialized only as a direct "
            "NodeRef construction argument");
    }
    evaluation_error(expression,
        "configuration type is not supported by the restricted expression language");
}

ConfigurationExpressionPtr expression_from_json(
    details::ConfigurationExpressionTypeSchema const& schema,
    nlohmann::ordered_json const& value)
{
    using Kind = details::ConfigurationExpressionTypeKind;
    auto make = [](auto expression) -> ConfigurationExpressionPtr {
        return std::make_shared<ConfigurationExpression const>(
            ConfigurationExpression{
                .value = std::move(expression),
            });
    };

    if (schema.kind == Kind::boolean) {
        if (!value.is_boolean()) {
            throw std::invalid_argument("node-message value must be Boolean");
        }
        return make(value.get<bool>());
    }
    if (schema.kind == Kind::signed_integer
        || schema.kind == Kind::unsigned_integer
        || schema.kind == Kind::floating_point) {
        if (!value.is_number()) {
            throw std::invalid_argument("node-message value must be numeric");
        }
        return make(ConfigurationNumberLiteral{.source = value.dump()});
    }
    if (schema.kind == Kind::string) {
        if (!value.is_string()) {
            throw std::invalid_argument("node-message value must be a string");
        }
        return make(ConfigurationStringLiteral{.value = value.get<std::string>()});
    }
    if (schema.kind == Kind::converted_scalar) {
        if (value.is_boolean()) return make(value.get<bool>());
        if (value.is_number()) {
            return make(ConfigurationNumberLiteral{.source = value.dump()});
        }
        if (value.is_string()) {
            return make(ConfigurationStringLiteral{
                .value = value.get<std::string>(),
            });
        }
        throw std::invalid_argument(
            "node-message converted scalar must be Boolean, numeric, or string");
    }
    if (schema.kind == Kind::enumeration) {
        if (!value.is_string()) {
            throw std::invalid_argument(
                "node-message enum value must be an enumerator name");
        }
        return make(ConfigurationIdentifierExpression{
            .value = value.get<std::string>(),
        });
    }
    if (schema.kind == Kind::aggregate) {
        if (!value.is_object()) {
            throw std::invalid_argument(
                "node-message aggregate value must be an object");
        }
        ConfigurationAggregateExpression aggregate;
        for (auto const& item : value.items()) {
            auto fields = std::span(schema.fields, schema.field_count);
            auto const field = std::ranges::find_if(
                fields,
                [&](auto const& candidate) {
                    return field_name(candidate) == item.key();
                });
            if (field == fields.end()) {
                throw std::invalid_argument(
                    "unknown node-message field '" + item.key() + "'");
            }
            if (!field->type) {
                throw std::invalid_argument(
                    "node-message field '" + item.key()
                    + "' has no generated type metadata");
            }
            aggregate.elements.push_back({
                .name = item.key(),
                .value = expression_from_json(*field->type, item.value()),
            });
        }
        return make(std::move(aggregate));
    }
    if (schema.kind == Kind::sequence) {
        if (!value.is_array()) {
            throw std::invalid_argument(
                "node-message sequence value must be an array");
        }
        if (!schema.element_type) {
            throw std::invalid_argument(
                "node-message sequence has no generated element metadata");
        }
        ConfigurationAggregateExpression sequence;
        sequence.elements.reserve(value.size());
        for (auto const& element : value) {
            sequence.elements.push_back({
                .value = expression_from_json(*schema.element_type, element),
            });
        }
        return make(std::move(sequence));
    }
    if (schema.kind == Kind::node_address) {
        auto address = parse_node_address_json(
            value, ProjectConnectionJsonNames::socket_rpc);
        return make(ConfigurationNodeReferenceExpression{
            .instance_id = std::move(address.instance_id),
            .path = std::move(address.path),
        });
    }
    if (schema.kind == Kind::node_reference) {
        throw std::invalid_argument(
            "a node-message payload cannot contain a live NodeRef; use NodeAddress");
    }
    throw std::invalid_argument(
        "node-message value type is not supported by generated metadata");
}

void collect_referenced_instance_ids(
    ConfigurationExpression const& expression, std::vector<std::string>& output)
{
    std::visit([&](auto const& value) {
        using Value = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::same_as<Value, ConfigurationNodeReferenceExpression>) {
            if (!std::ranges::contains(output, value.instance_id))
                output.push_back(value.instance_id);
        } else if constexpr (std::same_as<Value, ConfigurationUnaryExpression>) {
            if (value.operand) collect_referenced_instance_ids(*value.operand, output);
        } else if constexpr (std::same_as<Value, ConfigurationBinaryExpression>) {
            if (value.left) collect_referenced_instance_ids(*value.left, output);
            if (value.right) collect_referenced_instance_ids(*value.right, output);
        } else if constexpr (std::same_as<Value, ConfigurationCallExpression>) {
            for (auto const& argument : value.arguments)
                if (argument) collect_referenced_instance_ids(*argument, output);
        } else if constexpr (std::same_as<Value, ConfigurationAggregateExpression>) {
            for (auto const& element : value.elements)
                if (element.value) collect_referenced_instance_ids(*element.value, output);
        }
    }, expression.value);
}
} // namespace

std::vector<std::string> configuration_referenced_instance_ids(
    ParsedConfigurationExpressionList const& parsed)
{
    std::vector<std::string> result;
    for (auto const& argument : parsed.arguments) {
        if (argument) collect_referenced_instance_ids(*argument, result);
    }
    return result;
}

struct EvaluatedConfigurationArguments::Implementation {
    struct Argument {
        details::ConfigurationTypeIdentity const* identity = nullptr;
        std::unique_ptr<OwnedValue> value{};
        // A materialized project-node reference. NodeRef is one ABI type
        // shared by the runtime and every package, so it is owned here rather
        // than through the provider's schema callbacks.
        std::unique_ptr<NodeRef> node_reference{};
    };
    ParsedConfigurationExpressionList parsed{};
    std::vector<Argument> owned{};
    std::vector<details::ConfigurationArgument> arguments{};
};

struct EvaluatedConfigurationValue::Implementation {
    details::ConfigurationTypeIdentity const* type = nullptr;
    std::unique_ptr<OwnedValue> value{};
};

ParsedConfigurationExpressionList parse_configuration_expression_list(std::string source)
{
    return Parser(std::move(source)).parse();
}

EvaluatedConfigurationArguments::EvaluatedConfigurationArguments()
    : implementation_(std::make_unique<Implementation>())
{}
EvaluatedConfigurationArguments::~EvaluatedConfigurationArguments() = default;
EvaluatedConfigurationArguments::EvaluatedConfigurationArguments(
    EvaluatedConfigurationArguments&&) noexcept = default;
EvaluatedConfigurationArguments& EvaluatedConfigurationArguments::operator=(
    EvaluatedConfigurationArguments&&) noexcept = default;

std::span<details::ConfigurationArgument>
EvaluatedConfigurationArguments::arguments() noexcept
{
    return implementation_->arguments;
}

ParsedConfigurationExpressionList const&
EvaluatedConfigurationArguments::parsed() const noexcept
{
    return implementation_->parsed;
}

EvaluatedConfigurationValue::EvaluatedConfigurationValue()
    : implementation_(std::make_unique<Implementation>())
{}

EvaluatedConfigurationValue::~EvaluatedConfigurationValue() = default;
EvaluatedConfigurationValue::EvaluatedConfigurationValue(
    EvaluatedConfigurationValue&&) noexcept = default;
EvaluatedConfigurationValue& EvaluatedConfigurationValue::operator=(
    EvaluatedConfigurationValue&&) noexcept = default;

void const* EvaluatedConfigurationValue::data() const noexcept
{
    return implementation_->value ? implementation_->value->data() : nullptr;
}

details::ConfigurationTypeIdentity const*
EvaluatedConfigurationValue::type() const noexcept
{
    return implementation_->type;
}

EvaluatedConfigurationArguments evaluate_configuration_expression_list(
    ParsedConfigurationExpressionList parsed,
    details::RegisteredSignature const& signature,
    ConfigurationNodeReferenceMaterializer const* node_references)
{
    details::validate_registered_signature_shape("<configuration expression>", signature);
    if (parsed.arguments.size() < signature.required_argument_count
        || parsed.arguments.size() > signature.argument_count) {
        details::throw_configuration_argument_count_mismatch(
            "<configuration expression>", signature, parsed.arguments.size());
    }
    EvaluatedConfigurationArguments result;
    result.implementation_->parsed = std::move(parsed);
    result.implementation_->owned.reserve(result.implementation_->parsed.arguments.size());
    result.implementation_->arguments.reserve(
        result.implementation_->parsed.arguments.size());
    for (std::size_t index = 0;
         index < result.implementation_->parsed.arguments.size(); ++index) {
        auto const* identity = signature.parameter_types[index];
        if (!identity || !identity->expression_schema)
            throw std::invalid_argument(
                "configuration argument " + std::to_string(index)
                + " has no generated expression type metadata");
        auto const& schema = *identity->expression_schema;
        auto const& expression = *result.implementation_->parsed.arguments[index];
        if (schema.kind == details::ConfigurationExpressionTypeKind::node_reference) {
            auto const* reference =
                std::get_if<ConfigurationNodeReferenceExpression>(&expression.value);
            if (!reference)
                evaluation_error(expression, "expected a project-node reference");
            if (!node_references || !*node_references)
                evaluation_error(expression,
                    "a project-node reference requires a construction session");
            auto node = std::make_unique<NodeRef>((*node_references)(*reference));
            result.implementation_->arguments.push_back({
                .data = node.get(),
                .type = identity,
                .is_const = false,
                .is_rvalue = true,
                .is_array_decay = false,
            });
            result.implementation_->owned.push_back({
                .identity = identity,
                .node_reference = std::move(node),
            });
            continue;
        }
        auto value = std::make_unique<OwnedValue>(schema);
        construct_value(*value, schema, expression);
        result.implementation_->arguments.push_back({
            .data = value->data(),
            .type = identity,
            .is_const = false,
            .is_rvalue = true,
            .is_array_decay = false,
        });
        result.implementation_->owned.push_back({
            .identity = identity,
            .value = std::move(value),
        });
    }
    return result;
}

EvaluatedConfigurationArguments evaluate_configuration_json_arguments(
    nlohmann::ordered_json const& value,
    details::RegisteredSignature const& signature)
{
    details::validate_registered_signature_shape(
        "<replacement configuration arguments>", signature);
    if (!value.is_array()) {
        throw std::invalid_argument(
            "replacement configuration arguments must be a JSON array");
    }
    if (value.size() < signature.required_argument_count
        || value.size() > signature.argument_count) {
        details::throw_configuration_argument_count_mismatch(
            "<replacement configuration arguments>", signature, value.size());
    }

    EvaluatedConfigurationArguments result;
    result.implementation_->owned.reserve(value.size());
    result.implementation_->arguments.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        auto const* identity = signature.parameter_types[index];
        if (!identity || !identity->expression_schema) {
            throw std::invalid_argument(
                "replacement configuration argument " + std::to_string(index)
                + " has no generated expression type metadata");
        }
        auto const& schema = *identity->expression_schema;
        auto owned = std::make_unique<OwnedValue>(schema);
        auto expression = expression_from_json(schema, value[index]);
        construct_value(*owned, schema, *expression);
        result.implementation_->arguments.push_back({
            .data = owned->data(),
            .type = identity,
            .is_const = false,
            .is_rvalue = true,
            .is_array_decay = false,
        });
        result.implementation_->owned.push_back({
            .identity = identity,
            .value = std::move(owned),
        });
    }
    return result;
}

EvaluatedConfigurationValue evaluate_configuration_json_value(
    nlohmann::ordered_json const& value,
    details::ConfigurationTypeIdentity const& type)
{
    if (!type.expression_schema) {
        throw std::invalid_argument(
            "node-message type has no generated value schema");
    }
    EvaluatedConfigurationValue result;
    result.implementation_->type = &type;
    result.implementation_->value = std::make_unique<OwnedValue>(
        *type.expression_schema);
    auto expression = expression_from_json(*type.expression_schema, value);
    construct_value(
        *result.implementation_->value,
        *type.expression_schema,
        *expression);
    return result;
}
} // namespace iv
