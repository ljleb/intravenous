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

        static_assert(details::has_constexpr_sample_port_configs<Nlms>);
        static_assert(details::has_constexpr_sample_port_configs<Residual>);
        static_assert(details::has_constexpr_sample_port_configs<ResidualAr2>);
        static_assert(details::has_constexpr_sample_port_configs<Poly>);

        static_assert(Nlms::inputs()[0].history == 7);
        static_assert(Nlms::outputs()[0].history == 3);
        static_assert(Residual::inputs()[0].history == 6);
        static_assert(Residual::outputs()[0].history == 5);
        static_assert(ResidualAr2::inputs()[0].history == 8);
        static_assert(ResidualAr2::outputs()[0].history == 6);
        static_assert(Poly::inputs()[0].history == 3);
        static_assert(Poly::outputs()[0].history == 1);

        [[maybe_unused]] Nlms nlms { 0.1f, 0.9f };
        [[maybe_unused]] Residual residual { 0.1f };
        [[maybe_unused]] ResidualAr2 residual_ar2 { 0.1f };
        [[maybe_unused]] Poly poly { 0.1f };
    }
}
