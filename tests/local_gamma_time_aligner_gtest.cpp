#include "local_gamma_time_aligner.h"

#include <gtest/gtest.h>

#include <cmath>

namespace {
    TEST(LocalGammaTimeAligner, InitializesFromFirstCallback)
    {
        iv::LocalGammaTimeAligner aligner;
        double const dt = 0.001;

        EXPECT_FALSE(aligner.initialized());

        aligner.observe_callback(10.0, 1000, dt);

        EXPECT_TRUE(aligner.initialized());
        EXPECT_DOUBLE_EQ(aligner.beta(), 0.0);
        EXPECT_DOUBLE_EQ(aligner.predict_sample_offset(10.25, dt), 250.0);
    }

    TEST(LocalGammaTimeAligner, PreservesStableBlockAlignment)
    {
        iv::LocalGammaTimeAligner aligner;
        double const dt = 0.001;

        aligner.observe_callback(10.0, 1000, dt);
        aligner.observe_callback(10.010, 1010, dt);

        EXPECT_NEAR(aligner.beta(), 0.0, 1.0e-12);
        EXPECT_NEAR(aligner.predict_sample_offset(10.015, dt), 5.0, 1.0e-9);
    }

    TEST(LocalGammaTimeAligner, SurprisingCallbackInflatesSigmaAndMovesBeta)
    {
        iv::LocalGammaTimeAligner aligner;
        double const dt = 0.001;

        aligner.observe_callback(10.0, 1000, dt);
        aligner.observe_callback(10.010, 1010, dt);
        double const beta_before = aligner.beta();
        double const sigma2_before = aligner.sigma2();

        aligner.observe_callback(10.015, 1020, dt);

        EXPECT_GT(aligner.beta(), beta_before);
        EXPECT_GT(aligner.sigma2(), sigma2_before);
    }
}
