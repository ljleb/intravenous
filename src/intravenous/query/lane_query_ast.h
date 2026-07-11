#pragma once

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace iv::query {
using RawLaneQueryLiteral = std::variant<std::monostate, int, float>;

class RawValueExpr {
public:
    virtual ~RawValueExpr() = default;
};

class RawElementExpr {
public:
    virtual ~RawElementExpr() = default;
};

struct RawExists final : RawElementExpr {
    std::string key {};
};

struct RawHasValue final : RawElementExpr {
    std::string key {};
    std::unique_ptr<RawValueExpr> value {};
};

struct RawElementNot final : RawElementExpr {
    std::unique_ptr<RawElementExpr> expr {};
};

struct RawElementAnd final : RawElementExpr {
    std::vector<std::unique_ptr<RawElementExpr>> parts {};
};

struct RawElementOr final : RawElementExpr {
    std::vector<std::unique_ptr<RawElementExpr>> parts {};
};

struct RawLiteral final : RawValueExpr {
    RawLaneQueryLiteral value {};
};

struct RawRange final : RawValueExpr {
    std::optional<double> lo {};
    std::optional<double> hi {};
};

struct RawValueNot final : RawValueExpr {
    std::unique_ptr<RawValueExpr> expr {};
};

struct RawValueOr final : RawValueExpr {
    std::vector<std::unique_ptr<RawValueExpr>> parts {};
};

struct RawProjection final : RawValueExpr {
    std::unique_ptr<RawElementExpr> source {};
    std::string selector_key {};
};

struct RawLaneQueryAst {
    std::string source {};
    std::unique_ptr<RawElementExpr> root {};
};
} // namespace iv::query
