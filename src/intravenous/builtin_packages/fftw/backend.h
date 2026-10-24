#pragma once

#include <intravenous/continuous_value.h>

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#if defined(IV_FFTW_BACKEND_BUILD)
#define IV_FFTW_BACKEND_EXPORT __declspec(dllexport)
#else
#define IV_FFTW_BACKEND_EXPORT __declspec(dllimport)
#endif
#else
#define IV_FFTW_BACKEND_EXPORT __attribute__((visibility("default")))
#endif

namespace iv::fftw_package {

struct ForwardPlan;
struct InversePlan;

extern "C" IV_FFTW_BACKEND_EXPORT ForwardPlan*
iv_fftw_create_forward_plan(std::size_t transform_size) noexcept;

extern "C" IV_FFTW_BACKEND_EXPORT void
iv_fftw_destroy_forward_plan(ForwardPlan*) noexcept;

extern "C" IV_FFTW_BACKEND_EXPORT bool iv_fftw_execute_forward_plan(
    ForwardPlan*,
    Sample const* input,
    std::size_t input_size,
    FFTBin* output,
    std::size_t output_size) noexcept;

extern "C" IV_FFTW_BACKEND_EXPORT InversePlan*
iv_fftw_create_inverse_plan(std::size_t transform_size) noexcept;

extern "C" IV_FFTW_BACKEND_EXPORT void
iv_fftw_destroy_inverse_plan(InversePlan*) noexcept;

// Returns one normalized N-sample time-domain block. Repeating the same
// ordinal and canonical spectrum reuses the previously executed inverse plan.
extern "C" IV_FFTW_BACKEND_EXPORT Sample const* iv_fftw_execute_inverse_plan(
    InversePlan*,
    FFTBin const* input,
    std::size_t input_size,
    std::uint64_t spectrum_ordinal) noexcept;

} // namespace iv::fftw_package

#undef IV_FFTW_BACKEND_EXPORT
