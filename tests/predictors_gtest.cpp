#include <intravenous/basic_nodes/predictors.h>
#include <intravenous/graph/reflected_node_description.h>
#include <intravenous/node/traits.h>

#include <gtest/gtest.h>

namespace {
    using namespace iv;

    TEST(PredictorPortContracts, StructuralConfigurationBelongsToTheNodeType)
    {
        using Nlms = NlmsPredictor<3, 8>;
        using Residual = TanhResidualPredictor<2, 7, 3, 5>;
        using ResidualAr2 = TanhResidualAR2Predictor<4, 9, 2, 6, 3>;
        using Poly = PolyResidualPredictor<1, 4>;

        static_assert(details::has_constexpr_port_configs<Nlms>);
        static_assert(details::has_constexpr_port_configs<Residual>);
        static_assert(details::has_constexpr_port_configs<ResidualAr2>);
        static_assert(details::has_constexpr_port_configs<Poly>);

        auto expect_history = []<class Node>(
                                  Node const& node,
                                  std::size_t input_history,
                                  std::size_t output_history) {
            auto const reflected = details::reflect_node(node);
            ASSERT_EQ(reflected.port_constraints.history_anchors.size(), 2u);
            EXPECT_EQ(
                reflected.port_constraints.history_anchors[0],
                (PortHistoryAnchorConstraint{
                    .port = {
                        .direction = PortConstraintDirection::input,
                        .kind = PortKind::sample,
                        .port_index = 0,
                    },
                    .value = input_history,
                }));
            EXPECT_EQ(
                reflected.port_constraints.history_anchors[1],
                (PortHistoryAnchorConstraint{
                    .port = {
                        .direction = PortConstraintDirection::output,
                        .kind = PortKind::sample,
                        .port_index = 0,
                    },
                    .value = output_history,
                }));
        };

        expect_history(Nlms{0.1f, 0.9f}, 7, 3);
        expect_history(Residual{0.1f}, 6, 5);
        expect_history(ResidualAr2{0.1f}, 8, 6);
        expect_history(Poly{0.1f}, 3, 1);
    }
}
