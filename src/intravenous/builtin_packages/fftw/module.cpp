#include <intravenous/dsl.h>

#include <intravenous/assert.h>
#include <intravenous/node/lifecycle.h>
#include <intravenous/node/port_constraints.h>

#include "backend.h"

#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace {

void validate_transform_size(std::size_t transform_size)
{
    if (transform_size < 2 || !std::has_single_bit(transform_size)
        || transform_size > static_cast<std::size_t>(INT_MAX)) {
        throw std::invalid_argument(
            "FFTW real-transform size must be a power of two in [2, INT_MAX]");
    }
}

struct RealFFT {
    std::size_t transform_size = 1024;

    struct State {
        iv::fftw_package::ForwardPlan* plan = nullptr;
    };

    explicit RealFFT(std::size_t transform_size_ = 1024)
        : transform_size(transform_size_)
    {
        validate_transform_size(transform_size);
    }

    static constexpr auto inputs()
    {
        return std::array{
            iv::input<iv::Sample>("samples", iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::output<iv::FFTBlock>("spectrum", iv::OutputProduction::tick),
        };
    }

    void constrain_ports(iv::ConstrainPortsContext<RealFFT>& ctx) const
    {
        auto input = ctx.input<"samples">();
        auto output = ctx.output<"spectrum">();
        input.history() = transform_size;
        input.pace() = iv::PortPace{transform_size};
        output.pace() = iv::PortPace{1};
        output.size() = transform_size / 2 + 1;
    }

    void initialize(iv::InitializationContext<RealFFT> const& ctx) const
    {
        auto& state = ctx.state();
        state.plan = iv::fftw_package::iv_fftw_create_forward_plan(
            transform_size);
        if (!state.plan) {
            throw std::runtime_error("FFTW could not create a forward real-transform plan");
        }
    }

    void release(iv::ReleaseContext<RealFFT> const& ctx) const noexcept
    {
        auto& state = ctx.state();
        iv::fftw_package::iv_fftw_destroy_forward_plan(state.plan);
        state.plan = nullptr;
    }

    void tick_block(iv::TickBlockContext<RealFFT> const& ctx) const
    {
        auto const input = ctx.input<"samples">();
        auto const output = ctx.output<"spectrum">();
        auto const samples = input.block_extended().elements();
        auto spectra = output.block();
        auto& state = ctx.state();
        IV_ASSERT(state.plan, "FFTW forward plan was not initialized");

        for (std::size_t spectrum = 0; spectrum < spectra.size(); ++spectrum) {
            // Spectrum ordinal S is located at project coordinate S * N and
            // describes the N input samples immediately preceding it.
            auto const transform_end =
                (output.value_index() + spectrum) * transform_size;
            auto const input_offset_bits = transform_end - input.value_index();
            IV_ASSERT(input_offset_bits <= input.block_size(),
                "FFTW spectrum lies outside its projected Sample input block");
            auto const input_offset = static_cast<std::size_t>(input_offset_bits);
            IV_ASSERT(input_offset <= samples.size()
                    && transform_size <= samples.size() - input_offset,
                "FFTW input history does not contain its transform window");

            auto const window = samples.subspan(input_offset, transform_size);
            auto destination = spectra[spectrum];
            IV_ASSERT(iv::fftw_package::iv_fftw_execute_forward_plan(
                    state.plan,
                    window.data(),
                    window.size(),
                    destination.data(),
                    destination.size()),
                "FFTW rejected a resolved forward transform block");
        }
    }
};

// The spectrum at coordinate T reconstructs N samples beginning at T. Paired
// with RealFFT, which describes [T - N, T), this makes the transform round trip
// causal with exactly one transform-length delay.
struct InverseRealFFT {
    std::size_t transform_size = 1024;

    struct State {
        iv::fftw_package::InversePlan* plan = nullptr;
    };

    explicit InverseRealFFT(std::size_t transform_size_ = 1024)
        : transform_size(transform_size_)
    {
        validate_transform_size(transform_size);
    }

    static constexpr auto inputs()
    {
        return std::array{
            iv::input<iv::FFTBlock>("spectrum", iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::output<iv::Sample>("samples", iv::OutputProduction::tick),
        };
    }

    void constrain_ports(iv::ConstrainPortsContext<InverseRealFFT>& ctx) const
    {
        auto input = ctx.input<"spectrum">();
        auto output = ctx.output<"samples">();
        input.size() = transform_size / 2 + 1;
        input.history() = 1;
        input.pace() = iv::PortPace{1};
        output.pace() = iv::PortPace{transform_size};
    }

    void initialize(iv::InitializationContext<InverseRealFFT> const& ctx) const
    {
        auto& state = ctx.state();
        state.plan = iv::fftw_package::iv_fftw_create_inverse_plan(
            transform_size);
        if (!state.plan) {
            throw std::runtime_error("FFTW could not create an inverse real-transform plan");
        }
    }

    void release(iv::ReleaseContext<InverseRealFFT> const& ctx) const noexcept
    {
        auto& state = ctx.state();
        iv::fftw_package::iv_fftw_destroy_inverse_plan(state.plan);
        state.plan = nullptr;
    }

    void tick_block(iv::TickBlockContext<InverseRealFFT> const& ctx) const
    {
        auto const input = ctx.input<"spectrum">();
        auto const spectra = input.block_extended();
        auto const output = ctx.output<"samples">();
        auto samples = output.block();
        auto& state = ctx.state();
        IV_ASSERT(state.plan, "FFTW inverse plan was not initialized");

        auto const pace_shift = static_cast<std::uint32_t>(
            std::countr_zero(transform_size));
        auto const phase_mask = static_cast<std::uint64_t>(transform_size - 1);
        auto const spectra_begin_ordinal =
            input.value_index() - input.history();

        std::size_t cursor = 0;
        while (cursor < samples.size()) {
            auto const output_ordinal = output.value_index() + cursor;
            auto const spectrum_ordinal = output_ordinal >> pace_shift;
            auto const spectrum_offset_bits =
                spectrum_ordinal - spectra_begin_ordinal;
            IV_ASSERT(spectrum_offset_bits < spectra.size(),
                "FFTW inverse input history does not contain the required spectrum");
            auto const spectrum = spectra[
                static_cast<std::size_t>(spectrum_offset_bits)];
            auto const* reconstructed =
                iv::fftw_package::iv_fftw_execute_inverse_plan(
                state.plan,
                spectrum.data(),
                spectrum.size(),
                spectrum_ordinal);
            IV_ASSERT(reconstructed,
                "FFTW rejected a resolved inverse transform block");

            auto const phase = static_cast<std::size_t>(
                output_ordinal & phase_mask);
            auto const count = std::min(
                transform_size - phase,
                samples.size() - cursor);
            for (std::size_t sample = 0; sample < count; ++sample) {
                samples[cursor + sample] = reconstructed[phase + sample];
            }
            cursor += count;
        }
    }
};

} // namespace

IV_NODE("iv.fftw.real_fft", RealFFT);
IV_NODE("iv.fftw.inverse_real_fft", InverseRealFFT);
