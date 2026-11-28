#pragma once

#include <intravenous/graph/port_ids.h>

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace iv {

namespace details {
enum class TickCoverageOp : unsigned char { input, union_, intersection, extend_right };

// Postorder expression. The operand indices of an instruction precede it.
struct TickCoverageTerm {
    TickCoverageOp op = TickCoverageOp::input;
    PortKind kind = PortKind::sample;
    std::size_t first = 0;
    std::size_t second = 0;
    bool operator==(TickCoverageTerm const&) const = default;
};

struct TickOutputCoverageRule {
    PortKind kind = PortKind::sample;
    std::size_t output = 0;
    std::vector<TickCoverageTerm> terms{};
    bool operator==(TickOutputCoverageRule const&) const = default;
};

inline bool valid_tick_coverage_terms(std::span<TickCoverageTerm const> terms,
    std::size_t sample_inputs, std::size_t event_inputs)
{
    if (terms.empty()) return false;
    for (std::size_t index = 0; index < terms.size(); ++index) {
        auto const& term = terms[index];
        switch (term.op) {
        case TickCoverageOp::input:
            if (term.kind == PortKind::sample ? term.first >= sample_inputs
                : term.kind != PortKind::event || term.first >= event_inputs) return false;
            break;
        case TickCoverageOp::union_:
        case TickCoverageOp::intersection:
            if (term.first >= index || term.second >= index) return false;
            break;
        case TickCoverageOp::extend_right:
            if (term.first >= index || term.second == 0) return false;
            break;
        default:
            return false;
        }
    }
    return true;
}
}

class TickCoverageExpression {
    using Op = details::TickCoverageOp;
    std::vector<details::TickCoverageTerm> terms_;

    static TickCoverageExpression combine(TickCoverageExpression lhs,
        TickCoverageExpression const& rhs, Op op)
    {
        auto const offset = lhs.terms_.size();
        for (auto term : rhs.terms_) {
            if (term.op != Op::input) term.first += offset;
            if (term.op == Op::union_ || term.op == Op::intersection) term.second += offset;
            lhs.terms_.push_back(term);
        }
        lhs.terms_.push_back({.op = op, .first = offset - 1, .second = lhs.terms_.size() - 1});
        return lhs;
    }

public:
    TickCoverageExpression(PortKind kind, std::size_t input)
        : terms_{{.kind = kind, .first = input}} {}

    [[nodiscard]] TickCoverageExpression extend_right(std::size_t values) const
    {
        auto result = *this;
        if (values != 0) result.terms_.push_back({
            .op = Op::extend_right, .first = terms_.size() - 1, .second = values});
        return result;
    }

    friend TickCoverageExpression operator|(TickCoverageExpression lhs, TickCoverageExpression const& rhs)
    { return combine(std::move(lhs), rhs, Op::union_); }

    friend TickCoverageExpression operator&(TickCoverageExpression lhs, TickCoverageExpression const& rhs)
    { return combine(std::move(lhs), rhs, Op::intersection); }

    [[nodiscard]] std::span<details::TickCoverageTerm const> terms() const { return terms_; }
};

} // namespace iv
