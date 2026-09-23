#include <intravenous/basic_nodes/predictors.h>
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

        static_assert(port_history(Nlms::inputs()[0]) == 7);
        static_assert(port_history(Nlms::outputs()[0]) == 3);
        static_assert(port_history(Residual::inputs()[0]) == 6);
        static_assert(port_history(Residual::outputs()[0]) == 5);
        static_assert(port_history(ResidualAr2::inputs()[0]) == 8);
        static_assert(port_history(ResidualAr2::outputs()[0]) == 6);
        static_assert(port_history(Poly::inputs()[0]) == 3);
        static_assert(port_history(Poly::outputs()[0]) == 1);

        [[maybe_unused]] Nlms nlms { 0.1f, 0.9f };
        [[maybe_unused]] Residual residual { 0.1f };
        [[maybe_unused]] ResidualAr2 residual_ar2 { 0.1f };
        [[maybe_unused]] Poly poly { 0.1f };
    }
}
