#include "backend.h"

#include <fftw3.h>

#include <bit>
#include <climits>
#include <limits>
#include <mutex>
#include <new>

namespace iv::fftw_package {

struct ForwardPlan {
    std::size_t transform_size = 0;
    float* input = nullptr;
    fftwf_complex* output = nullptr;
    fftwf_plan plan = nullptr;
};

struct InversePlan {
    std::size_t transform_size = 0;
    fftwf_complex* input = nullptr;
    float* output = nullptr;
    FFTBin* cached_spectrum = nullptr;
    Sample* normalized_output = nullptr;
    fftwf_plan plan = nullptr;
    std::uint64_t cached_spectrum_ordinal = 0;
    bool cache_valid = false;
};

namespace {

std::mutex& planner_mutex()
{
    static std::mutex mutex;
    return mutex;
}

[[nodiscard]] bool valid_transform_size(std::size_t transform_size) noexcept
{
    auto const bin_count = transform_size / 2 + 1;
    return transform_size >= 2
        && std::has_single_bit(transform_size)
        && transform_size <= static_cast<std::size_t>(INT_MAX)
        && transform_size <= std::numeric_limits<std::size_t>::max()
            / sizeof(float)
        && transform_size <= std::numeric_limits<std::size_t>::max()
            / sizeof(Sample)
        && bin_count
            <= std::numeric_limits<std::size_t>::max()
                / sizeof(fftwf_complex)
        && bin_count
            <= std::numeric_limits<std::size_t>::max()
                / sizeof(FFTBin);
}

void destroy(ForwardPlan* storage) noexcept
{
    if (!storage) return;
    {
        std::lock_guard lock(planner_mutex());
        if (storage->plan) fftwf_destroy_plan(storage->plan);
    }
    fftwf_free(storage->output);
    fftwf_free(storage->input);
    delete storage;
}

void destroy(InversePlan* storage) noexcept
{
    if (!storage) return;
    {
        std::lock_guard lock(planner_mutex());
        if (storage->plan) fftwf_destroy_plan(storage->plan);
    }
    delete[] storage->normalized_output;
    delete[] storage->cached_spectrum;
    fftwf_free(storage->output);
    fftwf_free(storage->input);
    delete storage;
}

} // namespace

extern "C" ForwardPlan* iv_fftw_create_forward_plan(
    std::size_t transform_size) noexcept
{
    if (!valid_transform_size(transform_size)) return nullptr;
    auto* storage = new (std::nothrow) ForwardPlan{
        .transform_size = transform_size,
    };
    if (!storage) return nullptr;

    storage->input = fftwf_alloc_real(transform_size);
    storage->output = fftwf_alloc_complex(transform_size / 2 + 1);
    if (!storage->input || !storage->output) {
        destroy(storage);
        return nullptr;
    }
    {
        std::lock_guard lock(planner_mutex());
        storage->plan = fftwf_plan_dft_r2c_1d(
            static_cast<int>(transform_size),
            storage->input,
            storage->output,
            FFTW_ESTIMATE);
    }
    if (!storage->plan) {
        destroy(storage);
        return nullptr;
    }
    return storage;
}

extern "C" void iv_fftw_destroy_forward_plan(ForwardPlan* storage) noexcept
{
    destroy(storage);
}

extern "C" bool iv_fftw_execute_forward_plan(
    ForwardPlan* storage,
    Sample const* input,
    std::size_t input_size,
    FFTBin* output,
    std::size_t output_size) noexcept
{
    if (!storage || !storage->plan || !input || !output
        || input_size != storage->transform_size
        || output_size != storage->transform_size / 2 + 1) {
        return false;
    }

    for (std::size_t sample = 0; sample < input_size; ++sample) {
        storage->input[sample] = static_cast<float>(input[sample]);
    }
    fftwf_execute(storage->plan);
    for (std::size_t bin = 0; bin < output_size; ++bin) {
        output[bin] = FFTBin{
            storage->output[bin][0],
            storage->output[bin][1],
        };
    }
    return true;
}

extern "C" InversePlan* iv_fftw_create_inverse_plan(
    std::size_t transform_size) noexcept
{
    if (!valid_transform_size(transform_size)) return nullptr;
    auto* storage = new (std::nothrow) InversePlan{
        .transform_size = transform_size,
    };
    if (!storage) return nullptr;

    auto const bin_count = transform_size / 2 + 1;
    storage->input = fftwf_alloc_complex(bin_count);
    storage->output = fftwf_alloc_real(transform_size);
    storage->cached_spectrum = new (std::nothrow) FFTBin[bin_count];
    storage->normalized_output = new (std::nothrow) Sample[transform_size];
    if (!storage->input || !storage->output || !storage->cached_spectrum
        || !storage->normalized_output) {
        destroy(storage);
        return nullptr;
    }
    {
        std::lock_guard lock(planner_mutex());
        storage->plan = fftwf_plan_dft_c2r_1d(
            static_cast<int>(transform_size),
            storage->input,
            storage->output,
            FFTW_ESTIMATE);
    }
    if (!storage->plan) {
        destroy(storage);
        return nullptr;
    }
    return storage;
}

extern "C" void iv_fftw_destroy_inverse_plan(InversePlan* storage) noexcept
{
    destroy(storage);
}

extern "C" Sample const* iv_fftw_execute_inverse_plan(
    InversePlan* storage,
    FFTBin const* input,
    std::size_t input_size,
    std::uint64_t spectrum_ordinal) noexcept
{
    if (!storage || !storage->plan || !input
        || input_size != storage->transform_size / 2 + 1) {
        return nullptr;
    }

    bool unchanged = storage->cache_valid
        && storage->cached_spectrum_ordinal == spectrum_ordinal;
    for (std::size_t bin = 0; unchanged && bin < input_size; ++bin) {
        unchanged = storage->cached_spectrum[bin] == input[bin];
    }
    if (unchanged) return storage->normalized_output;

    for (std::size_t bin = 0; bin < input_size; ++bin) {
        auto const value = input[bin];
        storage->cached_spectrum[bin] = value;
        storage->input[bin][0] = value.real();
        storage->input[bin][1] = value.imag();
    }
    fftwf_execute(storage->plan);
    auto const normalization = 1.0f
        / static_cast<float>(storage->transform_size);
    for (std::size_t sample = 0; sample < storage->transform_size; ++sample) {
        storage->normalized_output[sample] = Sample{
            storage->output[sample] * normalization};
    }
    storage->cached_spectrum_ordinal = spectrum_ordinal;
    storage->cache_valid = true;
    return storage->normalized_output;
}

} // namespace iv::fftw_package
