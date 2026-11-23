#include "beats_events.hpp"

namespace {
constexpr uint32_t bpm_scale = 1'000'000;
constexpr uint32_t max_divisions = 1024;
constexpr uint32_t max_levels = 10;
constexpr uint32_t max_tempo = UINT32_MAX;
constexpr uint32_t edit_capacity = IV_MAX_EDITS + 1;

constexpr IvString string(char const* text)
{
    uint32_t size = 0;
    while (text[size]) ++size;
    return {text, size};
}

bool equal(IvString left, IvString right)
{
    if (left.size != right.size) return false;
    for (uint32_t i = 0; i < left.size; ++i)
        if (left.data[i] != right.data[i]) return false;
    return true;
}

struct Text {
    char bytes[128]{};
    uint32_t size = 0;

    void append(IvString value) {
        for (uint32_t i = 0; i < value.size && size < sizeof(bytes); ++i) bytes[size++] = value.data[i];
    }
    void append(char const* value) { append(string(value)); }
    void append(uint32_t value) {
        char digits[10];
        uint32_t count = 0;
        do { digits[count++] = '0' + value % 10; value /= 10; } while (value);
        while (count && size < sizeof(bytes)) bytes[size++] = digits[--count];
    }
    void append_tempo(uint32_t value) {
        append(value / bpm_scale);
        auto fraction = value % bpm_scale;
        if (!fraction) return;
        append(".");
        for (auto place = bpm_scale / 10; place && fraction; place /= 10) {
            append(fraction / place);
            fraction %= place;
        }
    }
    IvString view() const { return {bytes, size}; }
};

uint32_t clamp_integer(double value, uint32_t low, uint32_t high)
{
    if (value <= low) return low;
    if (value >= high) return high;
    return static_cast<uint32_t>(value + .5);
}

bool set_count(uint32_t& count, double value, uint32_t low, uint32_t high)
{
    if (value <= low) value = low;
    if (value >= high) value = high;
    auto const integer = static_cast<uint32_t>(value);
    if (integer != value) return false;
    count = integer;
    return true;
}

struct BeatGrid {
    uint32_t tempo = 120 * bpm_scale, beats = 4;
    uint32_t subdivisions[max_levels]{2, 2};
    uint32_t size = 2;

    uint32_t divisions() const {
        uint32_t product = 1;
        for (uint32_t i = 0; i < size; ++i) product *= subdivisions[i];
        return product;
    }
    bool same(BeatGrid const& other) const {
        if (tempo != other.tempo || beats != other.beats || size != other.size) return false;
        for (uint32_t i = 0; i < size; ++i)
            if (subdivisions[i] != other.subdivisions[i]) return false;
        return true;
    }
};

bool read_grid(iv::presentation::Span<IvNumericValue const> arguments, BeatGrid& grid)
{
    if (arguments.size > 3) return false;
    for (uint32_t i = 0; i < arguments.size && i < 2; ++i) {
        auto const field = arguments.data[i];
        if (field.kind != IV_NUMERIC_SCALAR || field.values.size != 1) return false;
        auto const value = field.values.data[0] * (i == 0 ? bpm_scale : 1);
        if (!(value >= (i == 0 ? .5 : 1.0) && value < double(UINT32_MAX) + .5)) return false;
        if (i == 0) grid.tempo = clamp_integer(value, 1, max_tempo);
        else {
            grid.beats = static_cast<uint32_t>(value);
            if (grid.beats != value) return false;
        }
    }
    if (arguments.size < 3) return true;
    auto const subdivisions = arguments.data[2];
    if (subdivisions.kind != IV_NUMERIC_SEQUENCE || subdivisions.values.size > max_levels) return false;
    uint32_t product = 1;
    grid.size = subdivisions.values.size;
    for (uint32_t i = 0; i < grid.size; ++i) {
        auto const factor = subdivisions.values.data[i];
        if (!(factor >= 2 && factor <= max_divisions / product)) return false;
        grid.subdivisions[i] = static_cast<uint32_t>(factor);
        if (grid.subdivisions[i] != factor) return false;
        product *= grid.subdivisions[i];
    }
    return true;
}

uint64_t period_numerator(uint32_t sample_rate) { return uint64_t{sample_rate} * 60 * bpm_scale; }

uint32_t tempo_limit(uint32_t divisions, uint32_t sample_rate)
{
    auto const limit = sample_rate ? period_numerator(sample_rate) / divisions : max_tempo;
    return limit < max_tempo ? static_cast<uint32_t>(limit) : max_tempo;
}

uint32_t division_limit(BeatGrid const& grid, uint32_t sample_rate)
{
    auto const limit = sample_rate ? period_numerator(sample_rate) / grid.tempo : max_divisions;
    return limit < max_divisions ? static_cast<uint32_t>(limit) : max_divisions;
}

Text indexed_id(char const* category, uint32_t index)
{
    Text id;
    id.append(category);
    id.append(index);
    return id;
}

bool apply_control(BeatGrid& grid, IvControlInput input, uint32_t sample_rate)
{
    auto const divisions = grid.divisions();
    auto const limit = division_limit(grid, sample_rate);
    if (equal(input.id, string("add"))) {
        if (grid.size == max_levels || divisions * 2 > limit) return false;
        grid.subdivisions[grid.size++] = 2;
        return true;
    }
    for (uint32_t i = 0; i < grid.size; ++i) {
        if (equal(input.id, indexed_id("remove:", i).view())) {
            for (uint32_t next = i + 1; next < grid.size; ++next) grid.subdivisions[next - 1] = grid.subdivisions[next];
            --grid.size;
            return true;
        }
        if (input.value_kind == IV_CONTROL_NUMBER && equal(input.id, indexed_id("subdivision:", i).view())) {
            auto const maximum = limit / (divisions / grid.subdivisions[i]);
            if (maximum < 2) return false;
            return set_count(grid.subdivisions[i], input.value, 2, maximum);
        }
    }
    if (input.value_kind != IV_CONTROL_NUMBER) return false;
    if (equal(input.id, string("bpm"))) {
        auto const maximum = tempo_limit(divisions, sample_rate);
        grid.tempo = clamp_integer(input.value * bpm_scale, 1, maximum);
        return true;
    }
    if (equal(input.id, string("beatsPerBar"))) return set_count(grid.beats, input.value, 1, UINT32_MAX);
    return false;
}

class Settings {
    struct RowText { Text label_id, value_id, value, id, label, remove_id; };
    RowText rows_[max_levels + 2]{};
    IvSetting items_[4 * max_levels + 7]{};
    uint32_t size_ = 0;

    void knob(uint32_t row, IvString id, IvString label, uint32_t value, double low, double high,
        double default_value, bool tempo = false)
    {
        auto& text = rows_[row];
        text.label_id.append(id);
        text.label_id.append(":label");
        text.value_id.append(id);
        text.value_id.append(":value");
        if (tempo) text.value.append_tempo(value);
        else text.value.append(value);
        items_[size_++] = {.id = text.label_id.view(), .label = label, .row = row, .kind = IV_SETTING_TEXT};
        items_[size_++] = {.id = text.value_id.view(), .label = text.value.view(), .row = row, .kind = IV_SETTING_TEXT};
        items_[size_++] = {.id = id, .label = label, .row = row, .kind = IV_SETTING_KNOB, .flags = IV_SETTING_INTEGER,
            .value = tempo ? double(value) / bpm_scale : value, .min = low, .max = high, .default_value = default_value};
    }

public:
    void record(iv::presentation::Context& frame, BeatGrid const& grid, uint32_t sample_rate, IvString title)
    {
        auto const divisions = grid.divisions();
        auto const limit = division_limit(grid, sample_rate);
        knob(0, string("bpm"), string("BPM"), grid.tempo, 1.0 / bpm_scale,
            double(tempo_limit(divisions, sample_rate)) / bpm_scale, 120, true);
        knob(1, string("beatsPerBar"), string("Beats per bar"), grid.beats, 1, UINT32_MAX, 4);
        for (uint32_t i = 0; i < grid.size; ++i) {
            auto& text = rows_[i + 2];
            text.id = indexed_id("subdivision:", i);
            text.label = indexed_id("Subdivision ", i + 1);
            knob(i + 2, text.id.view(), text.label.view(), grid.subdivisions[i], 2,
                limit / (divisions / grid.subdivisions[i]), 2);
            text.remove_id = indexed_id("remove:", i);
            items_[size_++] = {.id = text.remove_id.view(), .label = string("×"), .row = i + 2, .kind = IV_SETTING_ACTION};
        }
        items_[size_++] = {.id = string("add"), .label = string("Add subdivision"), .row = grid.size + 2,
            .kind = IV_SETTING_ACTION, .flags = divisions * 2 > limit ? uint32_t{IV_SETTING_DISABLED} : 0};
        frame.set_settings_panel({title, {items_, size_}});
    }
};

void record_grid(iv::presentation::Context& frame, BeatGrid const& grid, uint32_t sample_rate)
{
    if (!sample_rate) return;
    uint64_t strides[max_levels + 2]{};
    double starts[max_levels + 2]{0, .4};
    auto const divisions = grid.divisions();
    strides[0] = uint64_t{grid.beats} * divisions;
    strides[1] = divisions;
    for (uint32_t i = 0; i < grid.size; ++i) {
        strides[i + 2] = strides[i + 1] / grid.subdivisions[i];
        starts[i + 2] = 1 - .4 / (i + 2);
    }
    auto const numerator = period_numerator(sample_rate);
    auto const denominator = uint64_t{grid.tempo} * divisions;
    if (numerator < denominator) return;
    frame.set_timeline_grid({numerator, denominator, {strides, grid.size + 2}, {starts, grid.size + 2},
        8, .84, IV_INK_OUTPUT, IV_GRID_SEEK | IV_GRID_LABEL_MAJOR});
}

struct GridEdit {
    BeatGrid grid;
    iv::presentation::EditGesture gesture;
    uint32_t flags;
};

void record_edits(iv::presentation::Context& frame, GridEdit const* edits, uint32_t size)
{
    using presentation_beats_events::SetBeatGrid;
    for (uint32_t i = 0; i < size; ++i) {
        auto const& edit = edits[i];
        if (edit.flags == IV_EDIT_FINISH) {
            frame.finish_edit(edit.gesture);
            continue;
        }
        auto const& grid = edit.grid;
        SetBeatGrid const event{double(grid.tempo) / bpm_scale, grid.beats, {grid.subdivisions, grid.size}};
        if (edit.flags == IV_EDIT_REPLACE_PENDING) frame.replace_pending(event, edit.gesture);
        else frame.send(event);
    }
}

uint32_t knob_index(IvString id)
{
    if (equal(id, string("bpm"))) return 0;
    if (equal(id, string("beatsPerBar"))) return 1;
    for (uint32_t i = 0; i < max_levels; ++i)
        if (equal(id, indexed_id("subdivision:", i).view())) return 2 + i;
    return max_levels + 2;
}

class BeatsPresentation {
    BeatGrid draft_;
    bool has_draft_ = false;
    iv::presentation::EditGesture gestures_[max_levels + 2]{};
    Text edit_error_;

public:
    void present(iv::presentation::Context& frame)
    {
        BeatGrid accepted;
        if (!read_grid(frame.numeric_arguments(), accepted)) {
            frame.set_status(string("Waiting for accepted Beats arguments"));
            return;
        }
        if (frame.edit_failed() || !frame.edits_pending()) has_draft_ = false;
        for (auto const& reply : frame.edit_replies()) {
            if (reply.status != IV_EDIT_REJECTED) continue;
            has_draft_ = false;
            edit_error_ = {};
            edit_error_.append(reply.error);
        }
        for (auto const& gesture : frame.gestures()) {
            if (gesture.id || gesture.phase != IV_GESTURE_CANCEL) continue;
            has_draft_ = false;
            edit_error_ = {};
            for (auto& edit : gestures_) edit = {};
        }
        auto grid = has_draft_ ? draft_ : accepted;
        auto const sample_rate = frame.timeline_view().sample_rate;
        GridEdit edits[edit_capacity];
        uint32_t count = 0;
        for (auto const& input : frame.control_inputs()) {
            if (count == edit_capacity) break; // The SDK rejects an over-limit frame in full.
            auto const knob = knob_index(input.id);
            auto* gesture = knob < max_levels + 2 ? &gestures_[knob] : nullptr;
            if (input.phase == IV_CONTROL_FINISH) {
                if (gesture && gesture->id) {
                    edits[count++] = {.gesture = *gesture, .flags = IV_EDIT_FINISH};
                    *gesture = {};
                }
                continue;
            }
            if (!frame.can_edit()) continue;
            auto next = grid;
            if (!apply_control(next, input, sample_rate) || next.same(grid)) continue;
            bool const preview = input.phase == IV_CONTROL_PREVIEW && gesture;
            if (preview && !gesture->id) *gesture = frame.begin_edit();
            auto const flags = preview ? uint32_t{IV_EDIT_REPLACE_PENDING} : 0;
            auto const replace = preview && count && edits[count - 1].flags == flags
                && edits[count - 1].gesture.id == gesture->id;
            auto& edit = replace ? edits[count - 1] : edits[count++];
            edit = {next, preview ? *gesture : iv::presentation::EditGesture{}, flags};
            grid = draft_ = next;
            edit_error_ = {};
            has_draft_ = true;
        }
        if (count) record_edits(frame, edits, count);

        Text tempo;
        tempo.append_tempo(grid.tempo);
        tempo.append(" BPM");
        Settings settings;
        settings.record(frame, grid, sample_rate, tempo.view());

        Text status = tempo;
        status.append(" · ");
        status.append(grid.beats);
        status.append(" beats/bar · ");
        status.append(grid.size ? "Subdivision " : "No subdivisions");
        for (uint32_t i = 0; i < grid.size; ++i) {
            if (i) status.append(" × ");
            status.append(grid.subdivisions[i]);
        }
        frame.set_status(edit_error_.size ? edit_error_.view() : status.view());
        record_grid(frame, grid, sample_rate);
    }
};
}

IV_PRESENTATION_CLASS(BeatsPresentation, IV_PRESENTATION_TIMELINE, "M2 3v10m4-7v7m4-4v4m4-7v7")
