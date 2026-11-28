#pragma once

#include <intravenous/ports.h>

#include <algorithm>
#include <cstddef>
#include <span>

namespace iv {

class SystemAudioInputBinding {
    void* owner_ = nullptr;
    std::span<Sample const> (*captured_samples_)(void*) noexcept = nullptr;

public:
    SystemAudioInputBinding() = default;
    SystemAudioInputBinding(
        void* owner,
        std::span<Sample const> (*captured_samples)(void*) noexcept) noexcept
        : owner_(owner), captured_samples_(captured_samples)
    {}

    [[nodiscard]] std::span<Sample const> captured_samples() const noexcept
    {
        return captured_samples_ ? captured_samples_(owner_)
                                 : std::span<Sample const>{};
    }

    void read(std::span<Sample> destination) const noexcept
    {
        auto const source = captured_samples();
        auto const copied = std::min(destination.size(), source.size());
        std::copy_n(source.begin(), copied, destination.begin());
        std::fill(destination.begin() + static_cast<std::ptrdiff_t>(copied),
            destination.end(), Sample{});
    }
};

class SystemAudioOutputBinding {
    void* owner_ = nullptr;
    std::span<Sample> (*render_samples_)(void*) noexcept = nullptr;

public:
    SystemAudioOutputBinding() = default;
    SystemAudioOutputBinding(
        void* owner,
        std::span<Sample> (*render_samples)(void*) noexcept) noexcept
        : owner_(owner), render_samples_(render_samples)
    {}

    [[nodiscard]] std::span<Sample> render_samples() const noexcept
    {
        return render_samples_ ? render_samples_(owner_)
                               : std::span<Sample>{};
    }

    void write(std::span<Sample const> source) const noexcept
    {
        auto destination = render_samples();
        auto const copied = std::min(source.size(), destination.size());
        for (std::size_t index = 0; index < copied; ++index) {
            destination[index].value += source[index].value;
        }
    }
};

} // namespace iv
