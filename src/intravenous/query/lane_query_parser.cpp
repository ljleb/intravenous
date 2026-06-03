#include <intravenous/query/lane_query_parser.h>

#include <intravenous/query/lane_query_tokenizer.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace iv::query {
namespace {
constexpr std::string_view reserved_unit = "unit";

struct PathIdentComponent {
    std::string name {};
};

struct PathGroupComponent {
    std::unique_ptr<RawElementExpr> expr {};
};

using PathComponent = std::variant<PathIdentComponent, PathGroupComponent>;

std::string join_key(std::vector<std::string> const &parts)
{
    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            result += '.';
        }
        result += parts[i];
    }
    return result;
}

std::vector<std::string> split_key(std::string_view key)
{
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= key.size(); ++i) {
        if (i == key.size() || key[i] == '.') {
            parts.push_back(std::string(key.substr(start, i - start)));
            start = i + 1;
        }
    }
    return parts;
}

std::unique_ptr<RawElementExpr> clone_element(RawElementExpr const &expr);
std::unique_ptr<RawValueExpr> clone_value(RawValueExpr const &expr);

std::unique_ptr<RawElementExpr> make_exists(std::string key)
{
    auto expr = std::make_unique<RawExists>();
    expr->key = std::move(key);
    return expr;
}

std::unique_ptr<RawElementExpr> make_and(std::vector<std::unique_ptr<RawElementExpr>> parts)
{
    if (parts.size() == 1) {
        return std::move(parts.front());
    }
    auto expr = std::make_unique<RawElementAnd>();
    expr->parts = std::move(parts);
    return expr;
}

std::unique_ptr<RawElementExpr> make_or(std::vector<std::unique_ptr<RawElementExpr>> parts)
{
    if (parts.size() == 1) {
        return std::move(parts.front());
    }
    auto expr = std::make_unique<RawElementOr>();
    expr->parts = std::move(parts);
    return expr;
}

std::unique_ptr<RawElementExpr> compile_path_components(std::vector<PathComponent> const &components);
std::unique_ptr<RawElementExpr> apply_context(
    RawElementExpr const &expr,
    std::vector<std::string> const &prefix,
    std::vector<PathComponent> const &suffix);
std::unique_ptr<RawElementExpr> element_keys_to_value_tests(
    RawElementExpr const &expr,
    RawValueExpr const &value);

std::unique_ptr<RawElementExpr> compile_key_with_context(
    std::string const &key,
    std::vector<std::string> const &prefix,
    std::vector<PathComponent> const &suffix)
{
    std::vector<PathComponent> contextualized;
    contextualized.reserve(prefix.size() + split_key(key).size() + suffix.size());
    for (auto const &part : prefix) {
        contextualized.push_back(PathIdentComponent{.name = part});
    }
    for (auto const &part : split_key(key)) {
        contextualized.push_back(PathIdentComponent{.name = part});
    }
    for (auto const &component : suffix) {
        if (auto const *ident = std::get_if<PathIdentComponent>(&component)) {
            contextualized.push_back(PathIdentComponent{.name = ident->name});
        } else {
            auto const &group = std::get<PathGroupComponent>(component);
            contextualized.push_back(PathGroupComponent{.expr = clone_element(*group.expr)});
        }
    }
    return compile_path_components(contextualized);
}

std::unique_ptr<RawElementExpr> compile_path_components(std::vector<PathComponent> const &components)
{
    auto const group_it = std::find_if(components.begin(), components.end(), [](auto const &component) {
        return std::holds_alternative<PathGroupComponent>(component);
    });
    if (group_it == components.end()) {
        std::vector<std::string> parts;
        parts.reserve(components.size());
        for (auto const &component : components) {
            parts.push_back(std::get<PathIdentComponent>(component).name);
        }
        return make_exists(join_key(parts));
    }

    std::vector<std::string> prefix;
    for (auto it = components.begin(); it != group_it; ++it) {
        prefix.push_back(std::get<PathIdentComponent>(*it).name);
    }

    std::vector<PathComponent> suffix;
    for (auto it = std::next(group_it); it != components.end(); ++it) {
        if (auto const *ident = std::get_if<PathIdentComponent>(&*it)) {
            suffix.push_back(PathIdentComponent{.name = ident->name});
        } else {
            auto const &group = std::get<PathGroupComponent>(*it);
            suffix.push_back(PathGroupComponent{.expr = clone_element(*group.expr)});
        }
    }

    auto const &group = std::get<PathGroupComponent>(*group_it);
    return apply_context(*group.expr, prefix, suffix);
}

std::unique_ptr<RawElementExpr> apply_context(
    RawElementExpr const &expr,
    std::vector<std::string> const &prefix,
    std::vector<PathComponent> const &suffix)
{
    if (auto const *exists = dynamic_cast<RawExists const *>(&expr)) {
        return compile_key_with_context(exists->key, prefix, suffix);
    }
    if (auto const *has_value = dynamic_cast<RawHasValue const *>(&expr)) {
        return element_keys_to_value_tests(
            *compile_key_with_context(has_value->key, prefix, suffix),
            *has_value->value);
    }
    if (auto const *negated = dynamic_cast<RawElementNot const *>(&expr)) {
        auto result = std::make_unique<RawElementNot>();
        result->expr = apply_context(*negated->expr, prefix, suffix);
        return result;
    }
    if (auto const *conjunction = dynamic_cast<RawElementAnd const *>(&expr)) {
        std::vector<std::unique_ptr<RawElementExpr>> parts;
        parts.reserve(conjunction->parts.size());
        for (auto const &part : conjunction->parts) {
            parts.push_back(apply_context(*part, prefix, suffix));
        }
        return make_and(std::move(parts));
    }
    if (auto const *disjunction = dynamic_cast<RawElementOr const *>(&expr)) {
        std::vector<std::unique_ptr<RawElementExpr>> parts;
        parts.reserve(disjunction->parts.size());
        for (auto const &part : disjunction->parts) {
            parts.push_back(apply_context(*part, prefix, suffix));
        }
        return make_or(std::move(parts));
    }
    throw std::runtime_error("unsupported raw element expression");
}

std::unique_ptr<RawElementExpr> element_keys_to_value_tests(
    RawElementExpr const &expr,
    RawValueExpr const &value)
{
    if (auto const *exists = dynamic_cast<RawExists const *>(&expr)) {
        auto result = std::make_unique<RawHasValue>();
        result->key = exists->key;
        result->value = clone_value(value);
        return result;
    }
    if (dynamic_cast<RawHasValue const *>(&expr) != nullptr) {
        throw std::runtime_error("cannot apply '= value' to an expression that already contains value tests");
    }
    if (auto const *negated = dynamic_cast<RawElementNot const *>(&expr)) {
        auto result = std::make_unique<RawElementNot>();
        result->expr = element_keys_to_value_tests(*negated->expr, value);
        return result;
    }
    if (auto const *conjunction = dynamic_cast<RawElementAnd const *>(&expr)) {
        std::vector<std::unique_ptr<RawElementExpr>> parts;
        parts.reserve(conjunction->parts.size());
        for (auto const &part : conjunction->parts) {
            parts.push_back(element_keys_to_value_tests(*part, value));
        }
        return make_and(std::move(parts));
    }
    if (auto const *disjunction = dynamic_cast<RawElementOr const *>(&expr)) {
        std::vector<std::unique_ptr<RawElementExpr>> parts;
        parts.reserve(disjunction->parts.size());
        for (auto const &part : disjunction->parts) {
            parts.push_back(element_keys_to_value_tests(*part, value));
        }
        return make_or(std::move(parts));
    }
    throw std::runtime_error("unsupported raw element expression");
}

std::unique_ptr<RawElementExpr> clone_element(RawElementExpr const &expr)
{
    if (auto const *exists = dynamic_cast<RawExists const *>(&expr)) {
        auto result = std::make_unique<RawExists>();
        result->key = exists->key;
        return result;
    }
    if (auto const *has_value = dynamic_cast<RawHasValue const *>(&expr)) {
        auto result = std::make_unique<RawHasValue>();
        result->key = has_value->key;
        result->value = clone_value(*has_value->value);
        return result;
    }
    if (auto const *negated = dynamic_cast<RawElementNot const *>(&expr)) {
        auto result = std::make_unique<RawElementNot>();
        result->expr = clone_element(*negated->expr);
        return result;
    }
    if (auto const *conjunction = dynamic_cast<RawElementAnd const *>(&expr)) {
        std::vector<std::unique_ptr<RawElementExpr>> parts;
        parts.reserve(conjunction->parts.size());
        for (auto const &part : conjunction->parts) {
            parts.push_back(clone_element(*part));
        }
        return make_and(std::move(parts));
    }
    if (auto const *disjunction = dynamic_cast<RawElementOr const *>(&expr)) {
        std::vector<std::unique_ptr<RawElementExpr>> parts;
        parts.reserve(disjunction->parts.size());
        for (auto const &part : disjunction->parts) {
            parts.push_back(clone_element(*part));
        }
        return make_or(std::move(parts));
    }
    throw std::runtime_error("unsupported raw element expression");
}

std::unique_ptr<RawValueExpr> clone_value(RawValueExpr const &expr)
{
    if (auto const *literal = dynamic_cast<RawLiteral const *>(&expr)) {
        auto result = std::make_unique<RawLiteral>();
        result->value = literal->value;
        return result;
    }
    if (auto const *range = dynamic_cast<RawRange const *>(&expr)) {
        auto result = std::make_unique<RawRange>();
        result->lo = range->lo;
        result->hi = range->hi;
        return result;
    }
    if (auto const *negated = dynamic_cast<RawValueNot const *>(&expr)) {
        auto result = std::make_unique<RawValueNot>();
        result->expr = clone_value(*negated->expr);
        return result;
    }
    if (auto const *disjunction = dynamic_cast<RawValueOr const *>(&expr)) {
        auto result = std::make_unique<RawValueOr>();
        result->parts.reserve(disjunction->parts.size());
        for (auto const &part : disjunction->parts) {
            result->parts.push_back(clone_value(*part));
        }
        return result;
    }
    if (auto const *projection = dynamic_cast<RawProjection const *>(&expr)) {
        auto result = std::make_unique<RawProjection>();
        result->source = clone_element(*projection->source);
        result->selector_key = projection->selector_key;
        return result;
    }
    throw std::runtime_error("unsupported raw value expression");
}

class ParserImpl {
    std::vector<LaneQueryToken> tokens;
    size_t index = 0;

    LaneQueryToken const &peek() const
    {
        return tokens[index];
    }

    std::string_view peek_text() const
    {
        return peek().text;
    }

    LaneQueryToken pop(std::optional<std::string_view> expected = std::nullopt)
    {
        auto token = peek();
        if (expected.has_value() && token.text != *expected) {
            throw std::runtime_error("expected '" + std::string(*expected) + "', got '" + token.text + "'");
        }
        ++index;
        return token;
    }

    bool accept(std::string_view text)
    {
        if (peek_text() == text) {
            ++index;
            return true;
        }
        return false;
    }

    static std::unordered_set<std::string> with_stop(
        std::unordered_set<std::string> stop,
        std::string extra)
    {
        stop.insert(std::move(extra));
        return stop;
    }

    std::unique_ptr<RawElementExpr> parse_element_or(std::unordered_set<std::string> const &stop)
    {
        std::vector<std::unique_ptr<RawElementExpr>> parts;
        parts.push_back(parse_element_and(with_stop(stop, "|")));
        while (peek_text() == "|" && !stop.contains("|")) {
            pop("|");
            parts.push_back(parse_element_and(with_stop(stop, "|")));
        }
        return make_or(std::move(parts));
    }

    std::unique_ptr<RawElementExpr> parse_element_and(std::unordered_set<std::string> const &stop)
    {
        std::vector<std::unique_ptr<RawElementExpr>> parts;
        parts.push_back(parse_element_unary(stop));
        while (!stop.contains(std::string(peek_text())) && peek().kind != LaneQueryToken::Kind::end) {
            parts.push_back(parse_element_unary(stop));
        }
        return make_and(std::move(parts));
    }

    std::unique_ptr<RawElementExpr> parse_element_unary(std::unordered_set<std::string> const &stop)
    {
        if (accept("!")) {
            auto result = std::make_unique<RawElementNot>();
            result->expr = parse_element_unary(stop);
            return result;
        }
        return parse_element_primary(stop);
    }

    std::unique_ptr<RawElementExpr> parse_element_primary(std::unordered_set<std::string> const &stop)
    {
        if (accept("(")) {
            auto inner = parse_element_or({")"});
            pop(")");
            if (peek_text() == ".") {
                std::vector<PathComponent> components;
                components.push_back(PathGroupComponent{.expr = std::move(inner)});
                while (accept(".")) {
                    components.push_back(parse_path_component());
                }
                auto expr = compile_path_components(components);
                if (accept("=")) {
                    expr = element_keys_to_value_tests(*expr, *parse_value_or(stop));
                }
                return expr;
            }
            if (accept("=")) {
                return element_keys_to_value_tests(*inner, *parse_value_or(stop));
            }
            return inner;
        }

        auto expr = compile_path_components(parse_path_components());
        if (accept("=")) {
            expr = element_keys_to_value_tests(*expr, *parse_value_or(stop));
        }
        return expr;
    }

    std::vector<PathComponent> parse_path_components()
    {
        std::vector<PathComponent> components;
        components.push_back(parse_path_component());
        while (accept(".")) {
            components.push_back(parse_path_component());
        }
        return components;
    }

    PathComponent parse_path_component()
    {
        if (accept("(")) {
            auto inner = parse_element_or({")"});
            pop(")");
            return PathGroupComponent{.expr = std::move(inner)};
        }

        auto token = pop();
        if (token.kind != LaneQueryToken::Kind::ident || token.text == reserved_unit) {
            throw std::runtime_error("expected key segment, got '" + token.text + "'");
        }
        return PathIdentComponent{.name = token.text};
    }

    std::unique_ptr<RawValueExpr> parse_value_or(std::unordered_set<std::string> const &stop)
    {
        std::vector<std::unique_ptr<RawValueExpr>> parts;
        parts.push_back(parse_value_unary(with_stop(stop, "|")));
        while (peek_text() == "|" && !stop.contains("|")) {
            pop("|");
            parts.push_back(parse_value_unary(with_stop(stop, "|")));
        }
        if (parts.size() == 1) {
            return std::move(parts.front());
        }
        auto result = std::make_unique<RawValueOr>();
        result->parts = std::move(parts);
        return result;
    }

    std::unique_ptr<RawValueExpr> parse_value_unary(std::unordered_set<std::string> const &stop)
    {
        if (accept("!")) {
            auto result = std::make_unique<RawValueNot>();
            result->expr = parse_value_unary(stop);
            return result;
        }
        return parse_value_primary(stop);
    }

    std::unique_ptr<RawValueExpr> parse_value_primary(std::unordered_set<std::string> const&)
    {
        if (accept("(")) {
            auto saved = index;
            try {
                auto source = parse_element_or({")"});
                pop(")");
                if (accept(":")) {
                    auto projection = std::make_unique<RawProjection>();
                    projection->source = std::move(source);
                    projection->selector_key = parse_concrete_key();
                    return projection;
                }
            } catch (...) {
            }
            index = saved;
            auto value = parse_value_or({")"});
            pop(")");
            return value;
        }

        if (accept("..")) {
            auto range = std::make_unique<RawRange>();
            range->lo = std::nullopt;
            range->hi = parse_number();
            return range;
        }

        if (peek().kind == LaneQueryToken::Kind::ident && peek().text == reserved_unit) {
            pop();
            auto literal = std::make_unique<RawLiteral>();
            literal->value = std::monostate{};
            return literal;
        }

        if (peek().kind == LaneQueryToken::Kind::int_number || peek().kind == LaneQueryToken::Kind::float_number) {
            auto const token = pop();
            double const number = token.kind == LaneQueryToken::Kind::int_number
                ? static_cast<double>(std::stoi(token.text))
                : std::stod(token.text);
            if (accept("..")) {
                auto range = std::make_unique<RawRange>();
                range->lo = number;
                if (peek().kind == LaneQueryToken::Kind::int_number || peek().kind == LaneQueryToken::Kind::float_number) {
                    range->hi = parse_number();
                }
                return range;
            }
            auto literal = std::make_unique<RawLiteral>();
            literal->value = token.kind == LaneQueryToken::Kind::int_number
                ? RawLaneQueryLiteral{std::stoi(token.text)}
                : RawLaneQueryLiteral{std::stof(token.text)};
            return literal;
        }

        if (peek().kind == LaneQueryToken::Kind::ident) {
            auto source = compile_path_components(parse_path_components());
            if (!accept(":")) {
                throw std::runtime_error("value expression key source must be followed by ':' selector");
            }
            auto projection = std::make_unique<RawProjection>();
            projection->source = std::move(source);
            projection->selector_key = parse_concrete_key();
            return projection;
        }

        throw std::runtime_error("expected value expression, got '" + std::string(peek_text()) + "'");
    }

    double parse_number()
    {
        auto token = pop();
        if (token.kind == LaneQueryToken::Kind::int_number) {
            return static_cast<double>(std::stoi(token.text));
        }
        if (token.kind == LaneQueryToken::Kind::float_number) {
            return std::stod(token.text);
        }
        throw std::runtime_error("expected number, got '" + token.text + "'");
    }

    std::string parse_concrete_key()
    {
        std::vector<std::string> parts;
        auto token = pop();
        if (token.kind != LaneQueryToken::Kind::ident || token.text == reserved_unit) {
            throw std::runtime_error("expected selector key segment, got '" + token.text + "'");
        }
        parts.push_back(token.text);
        while (accept(".")) {
            token = pop();
            if (token.kind != LaneQueryToken::Kind::ident || token.text == reserved_unit) {
                throw std::runtime_error("expected selector key segment, got '" + token.text + "'");
            }
            parts.push_back(token.text);
        }
        return join_key(parts);
    }

public:
    explicit ParserImpl(std::string_view source) :
        tokens(tokenize_lane_query(source))
    {}

    std::unique_ptr<RawElementExpr> parse()
    {
        auto expr = parse_element_or({""});
        if (peek().kind != LaneQueryToken::Kind::end) {
            throw std::runtime_error("trailing token in lane query: '" + peek().text + "'");
        }
        return expr;
    }
};
} // namespace

RawLaneQueryAst LaneQueryParser::parse(std::string_view source) const
{
    return RawLaneQueryAst{
        .source = std::string(source),
        .root = ParserImpl(source).parse(),
    };
}
} // namespace iv::query
