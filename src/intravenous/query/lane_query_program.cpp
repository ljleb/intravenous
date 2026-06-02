#include "query/lane_query_program.h"

#include <cmath>
#include <ranges>
#include <stdexcept>

namespace iv::query {
namespace {
BoundPropertyRef bind_property_ref(std::string const &key, LaneQuerySchema const &schema)
{
    if (key.starts_with("filter.")) {
        return BoundPropertyRef{
            .kind = BoundPropertyRef::Kind::filter,
            .filter_name = key.substr(std::string_view("filter.").size()),
            .value_type = LaneQueryValueType::unit,
        };
    }
    auto const property = schema.find(key);
    if (!property.has_value()) {
        throw std::runtime_error("unknown lane query property: " + key);
    }
    return BoundPropertyRef{
        .kind = BoundPropertyRef::Kind::base,
        .property = *property,
        .filter_name = {},
        .value_type = schema.type_of(*property),
    };
}

LaneQueryExprType expr_type_for(LaneQueryValueType type)
{
    switch (type) {
    case LaneQueryValueType::unit:
        return LaneQueryExprType::unit_value_set;
    case LaneQueryValueType::int_:
        return LaneQueryExprType::int_value_set;
    case LaneQueryValueType::float_:
        return LaneQueryExprType::float_value_set;
    }
    return LaneQueryExprType::unit_value_set;
}

bool is_integral_value(double value)
{
    return std::floor(value) == value;
}

class Binder {
    LaneQuerySchema const &schema;

    std::unique_ptr<BoundElementExpr> bind_element(RawElementExpr const &expr)
    {
        if (auto const *exists = dynamic_cast<RawExists const *>(&expr)) {
            auto result = std::make_unique<BoundExists>();
            result->property = bind_property_ref(exists->key, schema);
            return result;
        }
        if (auto const *has_value = dynamic_cast<RawHasValue const *>(&expr)) {
            auto result = std::make_unique<BoundHasValue>();
            result->property = bind_property_ref(has_value->key, schema);
            result->value = bind_value(*has_value->value, result->property.value_type);
            return result;
        }
        if (auto const *negated = dynamic_cast<RawElementNot const *>(&expr)) {
            auto result = std::make_unique<BoundElementNot>();
            result->expr = bind_element(*negated->expr);
            return result;
        }
        if (auto const *conjunction = dynamic_cast<RawElementAnd const *>(&expr)) {
            auto result = std::make_unique<BoundElementAnd>();
            result->parts.reserve(conjunction->parts.size());
            for (auto const &part : conjunction->parts) {
                result->parts.push_back(bind_element(*part));
            }
            return result;
        }
        if (auto const *disjunction = dynamic_cast<RawElementOr const *>(&expr)) {
            auto result = std::make_unique<BoundElementOr>();
            result->parts.reserve(disjunction->parts.size());
            for (auto const &part : disjunction->parts) {
                result->parts.push_back(bind_element(*part));
            }
            return result;
        }
        throw std::runtime_error("unsupported lane query element expression");
    }

    std::unique_ptr<BoundValueExpr> bind_value(
        RawValueExpr const &expr,
        LaneQueryValueType expected_type)
    {
        if (auto const *literal = dynamic_cast<RawLiteral const *>(&expr)) {
            if (std::holds_alternative<std::monostate>(literal->value)) {
                if (expected_type != LaneQueryValueType::unit) {
                    throw std::runtime_error("lane query 'unit' literal can only match unit properties");
                }
                return std::make_unique<BoundLiteralUnit>();
            }
            if (auto const *int_value = std::get_if<int>(&literal->value)) {
                if (expected_type != LaneQueryValueType::int_) {
                    throw std::runtime_error("integer literal does not match lane query property type");
                }
                auto result = std::make_unique<BoundLiteralInt>();
                result->value = *int_value;
                return result;
            }
            if (auto const *float_value = std::get_if<float>(&literal->value)) {
                if (expected_type != LaneQueryValueType::float_) {
                    throw std::runtime_error("float literal does not match lane query property type");
                }
                auto result = std::make_unique<BoundLiteralFloat>();
                result->value = *float_value;
                return result;
            }
        }
        if (auto const *range = dynamic_cast<RawRange const *>(&expr)) {
            if (expected_type == LaneQueryValueType::unit) {
                throw std::runtime_error("lane query ranges are only valid for numeric properties");
            }
            if (expected_type == LaneQueryValueType::int_) {
                if ((range->lo.has_value() && !is_integral_value(*range->lo))
                    || (range->hi.has_value() && !is_integral_value(*range->hi))) {
                    throw std::runtime_error("lane query integer ranges must use integer bounds");
                }
                auto result = std::make_unique<BoundRangeInt>();
                if (range->lo.has_value()) {
                    result->lo = static_cast<int>(*range->lo);
                }
                if (range->hi.has_value()) {
                    result->hi = static_cast<int>(*range->hi);
                }
                return result;
            }
            auto result = std::make_unique<BoundRangeFloat>();
            if (range->lo.has_value()) {
                result->lo = static_cast<float>(*range->lo);
            }
            if (range->hi.has_value()) {
                result->hi = static_cast<float>(*range->hi);
            }
            return result;
        }
        if (auto const *negated = dynamic_cast<RawValueNot const *>(&expr)) {
            auto result = std::make_unique<BoundValueNot>();
            result->type = expr_type_for(expected_type);
            result->expr = bind_value(*negated->expr, expected_type);
            return result;
        }
        if (auto const *disjunction = dynamic_cast<RawValueOr const *>(&expr)) {
            auto result = std::make_unique<BoundValueOr>();
            result->type = expr_type_for(expected_type);
            result->parts.reserve(disjunction->parts.size());
            for (auto const &part : disjunction->parts) {
                result->parts.push_back(bind_value(*part, expected_type));
            }
            return result;
        }
        if (auto const *projection = dynamic_cast<RawProjection const *>(&expr)) {
            auto const selector = bind_property_ref(projection->selector_key, schema);
            if (selector.value_type != expected_type) {
                throw std::runtime_error("lane query projection selector type does not match compared property type");
            }
            auto result = std::make_unique<BoundProjection>();
            result->type = expr_type_for(selector.value_type);
            result->selector = selector;
            result->source = bind_element(*projection->source);
            return result;
        }
        throw std::runtime_error("unsupported lane query value expression");
    }

public:
    explicit Binder(LaneQuerySchema const &schema_in) :
        schema(schema_in)
    {}

    BoundLaneQueryAst bind(RawLaneQueryAst const &raw_ast)
    {
        return BoundLaneQueryAst{
            .schema_revision = schema.revision(),
            .root = bind_element(*raw_ast.root),
        };
    }
};

void collect_filter_references_from_value(
    BoundValueExpr const &expr,
    std::vector<std::string> &names);

void collect_filter_references_from_element(
    BoundElementExpr const &expr,
    std::vector<std::string> &names)
{
    if (auto const *exists = dynamic_cast<BoundExists const *>(&expr)) {
        if (exists->property.kind == BoundPropertyRef::Kind::filter) {
            names.push_back(exists->property.filter_name);
        }
        return;
    }
    if (auto const *has_value = dynamic_cast<BoundHasValue const *>(&expr)) {
        if (has_value->property.kind == BoundPropertyRef::Kind::filter) {
            names.push_back(has_value->property.filter_name);
        }
        collect_filter_references_from_value(*has_value->value, names);
        return;
    }
    if (auto const *negated = dynamic_cast<BoundElementNot const *>(&expr)) {
        collect_filter_references_from_element(*negated->expr, names);
        return;
    }
    if (auto const *conjunction = dynamic_cast<BoundElementAnd const *>(&expr)) {
        for (auto const &part : conjunction->parts) {
            collect_filter_references_from_element(*part, names);
        }
        return;
    }
    if (auto const *disjunction = dynamic_cast<BoundElementOr const *>(&expr)) {
        for (auto const &part : disjunction->parts) {
            collect_filter_references_from_element(*part, names);
        }
        return;
    }
}

void collect_filter_references_from_value(
    BoundValueExpr const &expr,
    std::vector<std::string> &names)
{
    if (auto const *negated = dynamic_cast<BoundValueNot const *>(&expr)) {
        collect_filter_references_from_value(*negated->expr, names);
        return;
    }
    if (auto const *disjunction = dynamic_cast<BoundValueOr const *>(&expr)) {
        for (auto const &part : disjunction->parts) {
            collect_filter_references_from_value(*part, names);
        }
        return;
    }
    if (auto const *projection = dynamic_cast<BoundProjection const *>(&expr)) {
        if (projection->selector.kind == BoundPropertyRef::Kind::filter) {
            names.push_back(projection->selector.filter_name);
        }
        collect_filter_references_from_element(*projection->source, names);
    }
}
} // namespace

BoundLaneQueryAst bind_lane_query_ast(
    RawLaneQueryAst const &raw_ast,
    LaneQuerySchema const &schema)
{
    return Binder(schema).bind(raw_ast);
}

std::vector<std::string> collect_filter_references(BoundLaneQueryAst const &ast)
{
    std::vector<std::string> names;
    if (ast.root) {
        collect_filter_references_from_element(*ast.root, names);
    }
    std::ranges::sort(names);
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}
} // namespace iv::query
