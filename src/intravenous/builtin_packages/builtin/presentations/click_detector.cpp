#include <intravenous/presentation/presentation.hpp>

namespace {
bool equal(IvString text, char const* expected)
{
    uint32_t index = 0;
    while (index < text.size && expected[index] && text.data[index] == expected[index]) ++index;
    return index == text.size && !expected[index];
}

class Status {
    char bytes_[512]{};
    uint32_t size_ = 0;

public:
    void append(char const* text)
    {
        while (*text && size_ < sizeof(bytes_)) bytes_[size_++] = *text++;
    }
    void append(IvString text)
    {
        for (uint32_t i = 0; i < text.size && size_ < sizeof(bytes_); ++i) bytes_[size_++] = text.data[i];
    }
    void append(uint64_t count)
    {
        char digits[20];
        uint32_t size = 0;
        do {
            digits[size++] = '0' + count % 10;
            count /= 10;
        } while (count);
        while (size) bytes_[size_++] = digits[--size];
    }
    IvString view() const { return {bytes_, size_}; }
};

class ClickDetector {
public:
    void present(iv::presentation::Context& frame)
    {
        auto const bounds = frame.content_bounds();
        IvObservedTriggerColumns const* clicks = nullptr;
        for (auto const& observation : frame.observed_trigger_columns()) {
            if (equal(observation.id, "clicks")) {
                clicks = &observation;
                break;
            }
        }
        if (clicks && clicks->columns.size) {
            auto const size = clicks->columns.size;
            auto batch = frame.reserve_polylines(size, 2 * size);
            if (!batch.polylines) return;
            for (uint32_t i = 0; i < size; ++i) {
                double const x = clicks->columns.data[i] + .5;
                auto* points = batch.points + 2 * i;
                points[0] = {x, 0};
                points[1] = {x, bounds.height};
                batch.polylines[i] = {i + 1, IV_INK_WARNING, {points, 2}};
            }
        }

        Status status;
        if (clicks && (clicks->count || clicks->status == IV_OBSERVATION_READY)) {
            if (clicks->partial) status.append("At least ");
            status.append(clicks->count);
            status.append(clicks->count == 1 ? " recorded click in view" : " recorded clicks in view");
        } else if (clicks && clicks->error.size) {
            status.append(clicks->error);
        } else if (clicks && clicks->status == IV_OBSERVATION_UNAVAILABLE) {
            status.append("Click observation is unavailable");
        } else {
            bool connected = false;
            for (auto const& port : frame.connected_inputs()) connected |= equal(port, "audio");
            status.append(connected ? "Waiting for click observations" : "Connect the audio to inspect");
        }

        auto const label = status.view();
        frame.set_status(label);
        if (frame.lane_mode() == IV_LANE_EDITABLE) {
            double const x = bounds.width - label.size * 7.0 - 8;
            IvText const text{1025, IV_INK_MUTED, {x > 4 ? x : 4, 16}, label};
            frame.draw_texts({&text, 1});
        }
    }
};
}

IV_PRESENTATION_CLASS(ClickDetector, IV_PRESENTATION_TIMELINE, "M1 8h4l2-5 2 10 2-5h4")
