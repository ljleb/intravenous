#pragma once

#include <intravenous/query/lane_query_ast.h>
#include <intravenous/query/lane_query_schema.h>
#include <intravenous/query/lane_query_types.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace iv::query {
struct BoundPropertyRef {
    enum class Kind : std::uint8_t {
        base,
        filter,
    };

    Kind kind = Kind::base;
    LaneQueryPropertyId property {};
    std::string filter_name {};
    LaneQueryValueType value_type = LaneQueryValueType::unit;
};

class BoundValueExpr {
public:
    virtual ~BoundValueExpr() = default;
    [[nodiscard]] virtual LaneQueryExprType expr_type() const = 0;
};

class BoundElementExpr {
public:
    virtual ~BoundElementExpr() = default;
};

struct BoundExists final : BoundElementExpr {
    BoundPropertyRef property {};
};

struct BoundHasValue final : BoundElementExpr {
    BoundPropertyRef property {};
    std::unique_ptr<BoundValueExpr> value {};
};

struct BoundElementNot final : BoundElementExpr {
    std::unique_ptr<BoundElementExpr> expr {};
};

struct BoundElementAnd final : BoundElementExpr {
    std::vector<std::unique_ptr<BoundElementExpr>> parts {};
};

struct BoundElementOr final : BoundElementExpr {
    std::vector<std::unique_ptr<BoundElementExpr>> parts {};
};

struct BoundLiteralUnit final : BoundValueExpr {
    [[nodiscard]] LaneQueryExprType expr_type() const override
    {
        return LaneQueryExprType::unit_value_set;
    }
};

struct BoundLiteralInt final : BoundValueExpr {
    int value = 0;

    [[nodiscard]] LaneQueryExprType expr_type() const override
    {
        return LaneQueryExprType::int_value_set;
    }
};

struct BoundLiteralFloat final : BoundValueExpr {
    float value = 0.0f;

    [[nodiscard]] LaneQueryExprType expr_type() const override
    {
        return LaneQueryExprType::float_value_set;
    }
};

struct BoundRangeInt final : BoundValueExpr {
    std::optional<int> lo {};
    std::optional<int> hi {};

    [[nodiscard]] LaneQueryExprType expr_type() const override
    {
        return LaneQueryExprType::int_value_set;
    }
};

struct BoundRangeFloat final : BoundValueExpr {
    std::optional<float> lo {};
    std::optional<float> hi {};

    [[nodiscard]] LaneQueryExprType expr_type() const override
    {
        return LaneQueryExprType::float_value_set;
    }
};

struct BoundValueNot final : BoundValueExpr {
    LaneQueryExprType type = LaneQueryExprType::unit_value_set;
    std::unique_ptr<BoundValueExpr> expr {};

    [[nodiscard]] LaneQueryExprType expr_type() const override
    {
        return type;
    }
};

struct BoundValueOr final : BoundValueExpr {
    LaneQueryExprType type = LaneQueryExprType::unit_value_set;
    std::vector<std::unique_ptr<BoundValueExpr>> parts {};

    [[nodiscard]] LaneQueryExprType expr_type() const override
    {
        return type;
    }
};

struct BoundProjection final : BoundValueExpr {
    LaneQueryExprType type = LaneQueryExprType::unit_value_set;
    BoundPropertyRef selector {};
    std::unique_ptr<BoundElementExpr> source {};

    [[nodiscard]] LaneQueryExprType expr_type() const override
    {
        return type;
    }
};

struct BoundLaneQueryAst {
    std::uint64_t schema_revision = 0;
    std::unique_ptr<BoundElementExpr> root {};
};

struct CompiledLaneQueryProgram {
    std::string source {};
    RawLaneQueryAst raw_ast {};
    BoundLaneQueryAst bound_ast {};
};

[[nodiscard]] BoundLaneQueryAst bind_lane_query_ast(
    RawLaneQueryAst const &raw_ast,
    LaneQuerySchema const &schema);
[[nodiscard]] std::vector<std::string> collect_filter_references(
    BoundLaneQueryAst const &ast);
} // namespace iv::query
