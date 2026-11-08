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
#include <utility>

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
        // The transform size plan was created for.
        std::size_t plan_size = 0;
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
        create_plan(ctx.state());
    }

    // Keeps the predecessor's plan when it was created for this transform
    // size, and otherwise replaces it with a plan for this size.
    void move(iv::MoveContext<RealFFT> const& ctx) const
    {
        auto& state = ctx.state();
        auto& previous = ctx.previous_state();
        if (previous.plan_size == transform_size) {
            state.plan = std::exchange(previous.plan, nullptr);
            state.plan_size = transform_size;
            return;
        }
        iv::fftw_package::iv_fftw_destroy_forward_plan(
            std::exchange(previous.plan, nullptr));
        create_plan(state);
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
        auto const& state = ctx.state();

        for (std::size_t spectrum = 0; spectrum < spectra.size(); ++spectrum) {
            // Spectrum index S is located at project coordinate S * N and
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

            iv::fftw_package::iv_fftw_execute_forward_plan(
                state.plan,
                samples.subspan(input_offset, transform_size).data(),
                spectra[spectrum].data());
        }
    }

    void create_plan(State& state) const
    {
        state.plan = iv::fftw_package::iv_fftw_create_forward_plan(
            transform_size);
        if (!state.plan) {
            throw std::runtime_error("FFTW could not create a forward real-transform plan");
        }
        state.plan_size = transform_size;
    }
};

// The spectrum at coordinate T reconstructs N samples beginning at T. Paired
// with RealFFT, which describes [T - N, T), this makes the transform round trip
// causal with exactly one transform-length delay.
struct InverseRealFFT {
    std::size_t transform_size = 1024;

    struct State {
        iv::fftw_package::InversePlan* plan = nullptr;
        // The transform size plan was created for.
        std::size_t plan_size = 0;
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
        create_plan(ctx.state());
    }

    // Keeps the predecessor's plan when it was created for this transform
    // size, and otherwise replaces it with a plan for this size.
    void move(iv::MoveContext<InverseRealFFT> const& ctx) const
    {
        auto& state = ctx.state();
        auto& previous = ctx.previous_state();
        if (previous.plan_size == transform_size) {
            state.plan = std::exchange(previous.plan, nullptr);
            state.plan_size = transform_size;
            return;
        }
        iv::fftw_package::iv_fftw_destroy_inverse_plan(
            std::exchange(previous.plan, nullptr));
        create_plan(state);
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
        auto const& state = ctx.state();

        auto const pace_shift = static_cast<std::uint32_t>(
            std::countr_zero(transform_size));
        auto const phase_mask = static_cast<std::uint64_t>(transform_size - 1);
        auto const spectra_begin_index =
            input.value_index() - input.history();

        std::size_t cursor = 0;
        while (cursor < samples.size()) {
            auto const output_index = output.value_index() + cursor;
            auto const spectrum_index = output_index >> pace_shift;
            auto const spectrum_offset_bits =
                spectrum_index - spectra_begin_index;
            IV_ASSERT(spectrum_offset_bits < spectra.size(),
                "FFTW inverse input history does not contain the required spectrum");
            auto const spectrum = spectra[
                static_cast<std::size_t>(spectrum_offset_bits)];
            auto const* reconstructed =
                iv::fftw_package::iv_fftw_execute_inverse_plan(
                state.plan,
                spectrum.data(),
                spectrum_index);

            auto const phase = static_cast<std::size_t>(
                output_index & phase_mask);
            auto const count = std::min(
                transform_size - phase,
                samples.size() - cursor);
            for (std::size_t sample = 0; sample < count; ++sample) {
                samples[cursor + sample] = reconstructed[phase + sample];
            }
            cursor += count;
        }
    }

    void create_plan(State& state) const
    {
        state.plan = iv::fftw_package::iv_fftw_create_inverse_plan(
            transform_size);
        if (!state.plan) {
            throw std::runtime_error("FFTW could not create an inverse real-transform plan");
        }
        state.plan_size = transform_size;
    }
};

} // namespace

IV_NODE("iv.fftw.real_fft", RealFFT);
IV_NODE("iv.fftw.inverse_real_fft", InverseRealFFT);
