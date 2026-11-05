#pragma once

#include <intravenous/module/package_definitions.h>
#include <intravenous/runtime/project_connection_types.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace iv {
struct ConfigurationExpression;
using ConfigurationExpressionPtr = std::shared_ptr<ConfigurationExpression const>;

struct ConfigurationNumberLiteral {
    std::string source{};
};

struct ConfigurationStringLiteral {
    std::string value{};
};

struct ConfigurationIdentifierExpression {
    std::string value{};
};

struct ConfigurationUnaryExpression {
    char operation = '+';
    ConfigurationExpressionPtr operand{};
};

struct ConfigurationBinaryExpression {
    char operation = '+';
    ConfigurationExpressionPtr left{};
    ConfigurationExpressionPtr right{};
};

struct ConfigurationCallExpression {
    std::string function{};
    std::vector<ConfigurationExpressionPtr> arguments{};
};

struct ConfigurationAggregateElement {
    std::optional<std::string> name{};
    ConfigurationExpressionPtr value{};
};

struct ConfigurationAggregateExpression {
    std::vector<ConfigurationAggregateElement> elements{};
};

struct ConfigurationNodeReferenceExpression {
    std::string instance_id{};
    std::vector<ProjectNodePathSelector> path{};
};

struct ConfigurationExpression {
    std::size_t source_begin = 0;
    std::size_t source_end = 0;
    std::variant<
        bool,
        ConfigurationNumberLiteral,
        ConfigurationStringLiteral,
        ConfigurationIdentifierExpression,
        ConfigurationUnaryExpression,
        ConfigurationBinaryExpression,
        ConfigurationCallExpression,
        ConfigurationAggregateExpression,
        ConfigurationNodeReferenceExpression> value{};
};

struct ParsedConfigurationExpressionList {
    std::string source{};
    std::vector<ConfigurationExpressionPtr> arguments{};
};

[[nodiscard]] ParsedConfigurationExpressionList parse_configuration_expression_list(
    std::string source);

class EvaluatedConfigurationArguments {
public:
    EvaluatedConfigurationArguments();
    ~EvaluatedConfigurationArguments();
    EvaluatedConfigurationArguments(EvaluatedConfigurationArguments&&) noexcept;
    EvaluatedConfigurationArguments& operator=(
        EvaluatedConfigurationArguments&&) noexcept;
    EvaluatedConfigurationArguments(EvaluatedConfigurationArguments const&) = delete;
    EvaluatedConfigurationArguments& operator=(
        EvaluatedConfigurationArguments const&) = delete;

    [[nodiscard]] std::span<details::ConfigurationArgument> arguments() noexcept;
    [[nodiscard]] ParsedConfigurationExpressionList const& parsed() const noexcept;

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
    friend EvaluatedConfigurationArguments evaluate_configuration_expression_list(
        ParsedConfigurationExpressionList,
        details::RegisteredSignature const&);
    friend EvaluatedConfigurationArguments evaluate_configuration_json_arguments(
        nlohmann::ordered_json const&,
        details::RegisteredSignature const&);
};

class EvaluatedConfigurationValue {
public:
    EvaluatedConfigurationValue();
    ~EvaluatedConfigurationValue();
    EvaluatedConfigurationValue(EvaluatedConfigurationValue&&) noexcept;
    EvaluatedConfigurationValue& operator=(
        EvaluatedConfigurationValue&&) noexcept;
    EvaluatedConfigurationValue(EvaluatedConfigurationValue const&) = delete;
    EvaluatedConfigurationValue& operator=(
        EvaluatedConfigurationValue const&) = delete;

    [[nodiscard]] void const* data() const noexcept;
    [[nodiscard]] details::ConfigurationTypeIdentity const* type() const noexcept;

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
    friend EvaluatedConfigurationValue evaluate_configuration_json_value(
        nlohmann::ordered_json const&,
        details::ConfigurationTypeIdentity const&);
};

[[nodiscard]] EvaluatedConfigurationArguments evaluate_configuration_expression_list(
    ParsedConfigurationExpressionList parsed,
    details::RegisteredSignature const& signature);

[[nodiscard]] EvaluatedConfigurationArguments compile_configuration_expression_list(
    std::string source,
    details::RegisteredSignature const& signature);

[[nodiscard]] EvaluatedConfigurationArguments evaluate_configuration_json_arguments(
    nlohmann::ordered_json const& value,
    details::RegisteredSignature const& signature);

[[nodiscard]] EvaluatedConfigurationValue evaluate_configuration_json_value(
    nlohmann::ordered_json const& value,
    details::ConfigurationTypeIdentity const& type);
} // namespace iv
