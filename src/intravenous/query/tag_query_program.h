#pragma once

#include <intravenous/query/tag_query_ast.h>
#include <intravenous/query/tag_query_schema.h>
#include <intravenous/query/tag_query_types.h>

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
    TagQueryPropertyId property {};
    std::string filter_name {};
    TagQueryValueType value_type = TagQueryValueType::unit;
};

class BoundValueExpr {
public:
    virtual ~BoundValueExpr() = default;
    [[nodiscard]] virtual TagQueryExprType expr_type() const = 0;
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
    [[nodiscard]] TagQueryExprType expr_type() const override
    {
        return TagQueryExprType::unit_value_set;
    }
};

struct BoundLiteralInt final : BoundValueExpr {
    int value = 0;

    [[nodiscard]] TagQueryExprType expr_type() const override
    {
        return TagQueryExprType::int_value_set;
    }
};

struct BoundLiteralFloat final : BoundValueExpr {
    float value = 0.0f;

    [[nodiscard]] TagQueryExprType expr_type() const override
    {
        return TagQueryExprType::float_value_set;
    }
};

struct BoundRangeInt final : BoundValueExpr {
    std::optional<int> lo {};
    std::optional<int> hi {};

    [[nodiscard]] TagQueryExprType expr_type() const override
    {
        return TagQueryExprType::int_value_set;
    }
};

struct BoundRangeFloat final : BoundValueExpr {
    std::optional<float> lo {};
    std::optional<float> hi {};

    [[nodiscard]] TagQueryExprType expr_type() const override
    {
        return TagQueryExprType::float_value_set;
    }
};

struct BoundValueNot final : BoundValueExpr {
    TagQueryExprType type = TagQueryExprType::unit_value_set;
    std::unique_ptr<BoundValueExpr> expr {};

    [[nodiscard]] TagQueryExprType expr_type() const override
    {
        return type;
    }
};

struct BoundValueOr final : BoundValueExpr {
    TagQueryExprType type = TagQueryExprType::unit_value_set;
    std::vector<std::unique_ptr<BoundValueExpr>> parts {};

    [[nodiscard]] TagQueryExprType expr_type() const override
    {
        return type;
    }
};

struct BoundProjection final : BoundValueExpr {
    TagQueryExprType type = TagQueryExprType::unit_value_set;
    BoundPropertyRef selector {};
    std::unique_ptr<BoundElementExpr> source {};

    [[nodiscard]] TagQueryExprType expr_type() const override
    {
        return type;
    }
};

struct BoundTagQueryAst {
    std::uint64_t schema_revision = 0;
    std::unique_ptr<BoundElementExpr> root {};
};

struct CompiledTagQueryProgram {
    std::string source {};
    RawTagQueryAst raw_ast {};
    BoundTagQueryAst bound_ast {};
};

[[nodiscard]] BoundTagQueryAst bind_tag_query_ast(
    RawTagQueryAst const &raw_ast,
    TagQuerySchema const &schema);
[[nodiscard]] std::vector<std::string> collect_filter_references(
    BoundTagQueryAst const &ast);
} // namespace iv::query
