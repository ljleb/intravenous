#include <intravenous/query/tag_query_engine.h>

#include <cstdint>
#include <ranges>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace iv::query {
namespace {
std::vector<std::uint8_t> evaluate_element(
    BoundElementExpr const &expr,
    TagQueryDataset const &dataset);

bool value_matches_unit(
    BoundValueExpr const &expr,
    TagQueryDataset const &dataset,
    size_t entry_index);

bool value_matches_int(
    BoundValueExpr const &expr,
    TagQueryDataset const &dataset,
    size_t entry_index,
    int value);

bool value_matches_float(
    BoundValueExpr const &expr,
    TagQueryDataset const &dataset,
    size_t entry_index,
    float value);

std::vector<std::uint8_t> evaluate_element(
    BoundElementExpr const &expr,
    TagQueryDataset const &dataset)
{
    if (auto const *exists = dynamic_cast<BoundExists const *>(&expr)) {
        std::vector<std::uint8_t> matches(dataset.entry_count(), 0);
        for (size_t entry_index = 0; entry_index < dataset.entry_count(); ++entry_index) {
            if (exists->property.kind == BoundPropertyRef::Kind::filter) {
                matches[entry_index] = dataset.in_filter(entry_index, exists->property.filter_name) ? 1 : 0;
                continue;
            }
            switch (exists->property.value_type) {
            case TagQueryValueType::unit:
                matches[entry_index] = dataset.has_unit(entry_index, exists->property.property) ? 1 : 0;
                break;
            case TagQueryValueType::int_:
                matches[entry_index] = dataset.int_value(entry_index, exists->property.property).has_value() ? 1 : 0;
                break;
            case TagQueryValueType::float_:
                matches[entry_index] = dataset.float_value(entry_index, exists->property.property).has_value() ? 1 : 0;
                break;
            }
        }
        return matches;
    }
    if (auto const *has_value = dynamic_cast<BoundHasValue const *>(&expr)) {
        std::vector<std::uint8_t> matches(dataset.entry_count(), 0);
        for (size_t entry_index = 0; entry_index < dataset.entry_count(); ++entry_index) {
            if (has_value->property.kind == BoundPropertyRef::Kind::filter) {
                if (dataset.in_filter(entry_index, has_value->property.filter_name)
                    && value_matches_unit(*has_value->value, dataset, entry_index)) {
                    matches[entry_index] = 1;
                }
                continue;
            }
            switch (has_value->property.value_type) {
            case TagQueryValueType::unit:
                if (dataset.has_unit(entry_index, has_value->property.property)
                    && value_matches_unit(*has_value->value, dataset, entry_index)) {
                    matches[entry_index] = 1;
                }
                break;
            case TagQueryValueType::int_:
                if (auto const value = dataset.int_value(entry_index, has_value->property.property);
                    value.has_value() && value_matches_int(*has_value->value, dataset, entry_index, *value)) {
                    matches[entry_index] = 1;
                }
                break;
            case TagQueryValueType::float_:
                if (auto const value = dataset.float_value(entry_index, has_value->property.property);
                    value.has_value() && value_matches_float(*has_value->value, dataset, entry_index, *value)) {
                    matches[entry_index] = 1;
                }
                break;
            }
        }
        return matches;
    }
    if (auto const *negated = dynamic_cast<BoundElementNot const *>(&expr)) {
        auto matches = evaluate_element(*negated->expr, dataset);
        for (auto &match : matches) {
            match = match == 0 ? 1 : 0;
        }
        return matches;
    }
    if (auto const *conjunction = dynamic_cast<BoundElementAnd const *>(&expr)) {
        std::vector<std::uint8_t> matches(dataset.entry_count(), 1);
        for (auto const &part : conjunction->parts) {
            auto const part_matches = evaluate_element(*part, dataset);
            for (size_t entry_index = 0; entry_index < matches.size(); ++entry_index) {
                matches[entry_index] = matches[entry_index] && part_matches[entry_index];
            }
        }
        return matches;
    }
    if (auto const *disjunction = dynamic_cast<BoundElementOr const *>(&expr)) {
        std::vector<std::uint8_t> matches(dataset.entry_count(), 0);
        for (auto const &part : disjunction->parts) {
            auto const part_matches = evaluate_element(*part, dataset);
            for (size_t entry_index = 0; entry_index < matches.size(); ++entry_index) {
                matches[entry_index] = matches[entry_index] || part_matches[entry_index];
            }
        }
        return matches;
    }
    throw std::runtime_error("unsupported bound tag query element expression");
}

bool value_matches_unit(
    BoundValueExpr const &expr,
    TagQueryDataset const &dataset,
    size_t entry_index)
{
    (void)entry_index;
    if (dynamic_cast<BoundLiteralUnit const *>(&expr) != nullptr) {
        return true;
    }
    if (auto const *negated = dynamic_cast<BoundValueNot const *>(&expr)) {
        return !value_matches_unit(*negated->expr, dataset, entry_index);
    }
    if (auto const *disjunction = dynamic_cast<BoundValueOr const *>(&expr)) {
        return std::ranges::any_of(disjunction->parts, [&](auto const &part) {
            return value_matches_unit(*part, dataset, entry_index);
        });
    }
    if (auto const *projection = dynamic_cast<BoundProjection const *>(&expr)) {
        auto const source_matches = evaluate_element(*projection->source, dataset);
        for (size_t source_index = 0; source_index < source_matches.size(); ++source_index) {
            if (source_matches[source_index] == 0) {
                continue;
            }
            if (projection->selector.kind == BoundPropertyRef::Kind::filter
                && dataset.in_filter(source_index, projection->selector.filter_name)) {
                return true;
            }
            if (projection->selector.kind == BoundPropertyRef::Kind::base
                && dataset.has_unit(source_index, projection->selector.property)) {
                return true;
            }
        }
        return false;
    }
    throw std::runtime_error("unsupported unit tag query value expression");
}

bool value_matches_int(
    BoundValueExpr const &expr,
    TagQueryDataset const &dataset,
    size_t entry_index,
    int value)
{
    (void)entry_index;
    if (auto const *literal = dynamic_cast<BoundLiteralInt const *>(&expr)) {
        return value == literal->value;
    }
    if (auto const *range = dynamic_cast<BoundRangeInt const *>(&expr)) {
        if (range->lo.has_value() && value < *range->lo) {
            return false;
        }
        if (range->hi.has_value() && value > *range->hi) {
            return false;
        }
        return true;
    }
    if (auto const *negated = dynamic_cast<BoundValueNot const *>(&expr)) {
        return !value_matches_int(*negated->expr, dataset, entry_index, value);
    }
    if (auto const *disjunction = dynamic_cast<BoundValueOr const *>(&expr)) {
        return std::ranges::any_of(disjunction->parts, [&](auto const &part) {
            return value_matches_int(*part, dataset, entry_index, value);
        });
    }
    if (auto const *projection = dynamic_cast<BoundProjection const *>(&expr)) {
        auto const source_matches = evaluate_element(*projection->source, dataset);
        std::unordered_set<int> projected_values;
        for (size_t source_index = 0; source_index < source_matches.size(); ++source_index) {
            if (source_matches[source_index] == 0) {
                continue;
            }
            if (auto const projected = dataset.int_value(source_index, projection->selector.property)) {
                projected_values.insert(*projected);
            }
        }
        return projected_values.contains(value);
    }
    throw std::runtime_error("unsupported integer tag query value expression");
}

bool value_matches_float(
    BoundValueExpr const &expr,
    TagQueryDataset const &dataset,
    size_t entry_index,
    float value)
{
    (void)entry_index;
    if (auto const *literal = dynamic_cast<BoundLiteralFloat const *>(&expr)) {
        return value == literal->value;
    }
    if (auto const *range = dynamic_cast<BoundRangeFloat const *>(&expr)) {
        if (range->lo.has_value() && value < *range->lo) {
            return false;
        }
        if (range->hi.has_value() && value > *range->hi) {
            return false;
        }
        return true;
    }
    if (auto const *negated = dynamic_cast<BoundValueNot const *>(&expr)) {
        return !value_matches_float(*negated->expr, dataset, entry_index, value);
    }
    if (auto const *disjunction = dynamic_cast<BoundValueOr const *>(&expr)) {
        return std::ranges::any_of(disjunction->parts, [&](auto const &part) {
            return value_matches_float(*part, dataset, entry_index, value);
        });
    }
    if (auto const *projection = dynamic_cast<BoundProjection const *>(&expr)) {
        auto const source_matches = evaluate_element(*projection->source, dataset);
        std::unordered_set<float> projected_values;
        for (size_t source_index = 0; source_index < source_matches.size(); ++source_index) {
            if (source_matches[source_index] == 0) {
                continue;
            }
            if (auto const projected = dataset.float_value(source_index, projection->selector.property)) {
                projected_values.insert(*projected);
            }
        }
        return projected_values.contains(value);
    }
    throw std::runtime_error("unsupported float tag query value expression");
}
} // namespace

TagQueryExecutionResult execute_tag_query(
    BoundTagQueryAst const &query,
    TagQueryDataset const &dataset)
{
    auto const matches = evaluate_element(*query.root, dataset);
    TagQueryExecutionResult result;
    result.matching_entry_indexes.reserve(matches.size());
    for (size_t index = 0; index < matches.size(); ++index) {
        if (matches[index] != 0) {
            result.matching_entry_indexes.push_back(index);
        }
    }
    return result;
}
} // namespace iv::query
