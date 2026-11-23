#ifndef INTRAVENOUS_PRESENTATION_HPP
#define INTRAVENOUS_PRESENTATION_HPP

#include "presentation.h"
#include <stddef.h>

namespace iv::presentation {
namespace detail {
struct SessionStorage { void* address; };
}

template<class Type>
struct Span {
    Type* data;
    uint32_t size;
    Type* begin() const { return data; }
    Type* end() const { return size ? data + size : data; }
};

struct EditGesture { uint32_t id = 0; };
struct EditRequest {
    uint32_t id = 0;
    explicit operator bool() const { return id != 0; }
};

inline IvConvertedScalar converted_string(IvString value) { return {IV_SCALAR_STRING, 0, value}; }

namespace detail {
// Generated bindings supply the index in the artifact's event schema section
// and the spans to retain for each aggregate.
template<class Event> struct EventBinding;
template<class Value> struct EventRecord;

class EditArena {
public:
    explicit EditArena(IvPresentationContext* context) : context_(context) {}

    bool retain(IvString& value) const
    {
        if (!value.size) { value.data = nullptr; return true; }
        value.data = static_cast<char const*>(iv_copy_edit_payload(context_, value.data, value.size));
        return value.data != nullptr;
    }

    bool retain(IvConvertedScalar& value) const
    {
        if (value.kind == IV_SCALAR_STRING) return retain(value.string);
        value.string = {};
        return true;
    }

    template<class Value>
    bool retain(Span<Value const>& span) const
    {
        static_assert(__is_trivially_copyable(Value) && alignof(Value) <= 8);
        if (!span.size) { span.data = nullptr; return true; }
        if (span.size > UINT32_MAX / sizeof(Value)) return false;
        auto* values = static_cast<Value*>(iv_copy_edit_payload(context_, span.data, span.size * sizeof(Value)));
        if (!values) return false;
        span.data = values;
        for (uint32_t i = 0; i < span.size; ++i)
            if (!retain(values[i])) return false;
        return true;
    }

    template<class Value>
    bool retain(Value& value) const
    {
        if constexpr (requires { EventRecord<Value>::retain(*this, value); })
            return EventRecord<Value>::retain(*this, value);
        return true;
    }

    template<class Event>
    Event const* copy(Event const& value) const
    {
        static_assert(__is_trivially_copyable(Event) && alignof(Event) <= 8);
        auto* copy = static_cast<Event*>(iv_copy_edit_payload(context_, &value, sizeof(Event)));
        return copy && retain(*copy) ? copy : nullptr;
    }

private:
    IvPresentationContext* context_;
};
}

class Context {
public:
    Context(IvPresentationContext& context, uint32_t& edit_ids) : context_(&context), edit_ids_(&edit_ids) {}
    IvRectangle content_bounds() const { return iv_content_bounds(context_); }
    bool can_edit() const { return iv_can_edit(context_); }
    uint32_t lane_mode() const { return iv_lane_mode(context_); }
    IvTimelineView timeline_view() const { return iv_timeline_view(context_); }
    uint32_t construction_snapshot() const { return iv_construction_snapshot(context_); }
    template<class Binding>
    uint32_t construction_argument_state() const { return iv_construction_argument_state(context_, Binding::index); }
    template<class Binding>
    typename Binding::Value const* construction_argument() const
    {
        return static_cast<typename Binding::Value const*>(iv_construction_argument(context_, Binding::index));
    }
    Span<IvNumericValue const> numeric_arguments() const
    {
        auto const args = iv_numeric_arguments(context_);
        return {args.data, args.size};
    }
    Span<IvControlInput const> control_inputs() const
    {
        auto const inputs = iv_control_inputs(context_);
        return {inputs.data, inputs.size};
    }
    Span<IvEditorInput const> editor_inputs() const
    {
        auto const inputs = iv_editor_inputs(context_);
        return {inputs.data, inputs.size};
    }
    Span<IvManualInput const> manual_inputs() const
    {
        auto const inputs = iv_manual_inputs(context_);
        return {inputs.data, inputs.size};
    }
    double frame_time() const { return iv_frame_time(context_); }
    bool edits_pending() const { return iv_edits_pending(context_); }
    bool edit_failed() const { return iv_edit_failed(context_); }
    uint64_t project_revision() const { return iv_project_revision(context_); }
    Span<IvEditReply const> edit_replies() const
    {
        auto const replies = iv_edit_replies(context_);
        return {replies.data, replies.size};
    }
    Span<IvString const> connected_inputs() const
    {
        auto const inputs = iv_connected_inputs(context_);
        return {inputs.data, inputs.size};
    }
    Span<IvObservedTriggerColumns const> observed_trigger_columns() const
    {
        auto const observations = iv_observed_trigger_columns(context_);
        return {observations.data, observations.size};
    }
    Span<IvObservedWaveform const> observed_waveforms() const
    {
        auto const observations = iv_observed_waveforms(context_);
        return {observations.data, observations.size};
    }
    IvObservedWaveform const* observed_waveform(IvString id) const
    {
        for (auto const& waveform : observed_waveforms()) {
            if (waveform.id.size != id.size) continue;
            uint32_t index = 0;
            while (index < id.size && waveform.id.data[index] == id.data[index]) ++index;
            if (index == id.size) return &waveform;
        }
        return nullptr;
    }
    Span<IvGestureInput const> gestures() const
    {
        auto const input = iv_gesture_inputs(context_);
        return {input.data, input.size};
    }
    template<class Gesture, class Control, class Editor>
    void visit_interactions(Gesture gesture, Control control, Editor editor) const
    {
        auto const pointer = gestures();
        auto const settings = control_inputs();
        auto const actions = editor_inputs();
        uint32_t g = 0, c = 0, e = 0;
        while (g < pointer.size || c < settings.size || e < actions.size) {
            auto const gesture_order = g < pointer.size ? pointer.data[g].sequence : UINT32_MAX;
            auto const control_order = c < settings.size ? settings.data[c].sequence : UINT32_MAX;
            auto const action_order = e < actions.size ? actions.data[e].sequence : UINT32_MAX;
            if (gesture_order < control_order && gesture_order < action_order) gesture(pointer.data[g++]);
            else if (control_order < action_order) control(settings.data[c++]);
            else editor(actions.data[e++]);
        }
    }
    bool draw_polylines(Span<IvPolyline const> lines) const
    {
        return iv_draw_polylines(context_, {lines.data, lines.size});
    }
    bool draw_texts(Span<IvText const> texts) const
    {
        return iv_draw_texts(context_, {texts.data, texts.size});
    }
    bool draw_shapes(Span<IvShape const> shapes) const
    {
        return iv_draw_shapes(context_, {shapes.data, shapes.size});
    }
    IvPolylineBatch reserve_polylines(uint32_t lines, uint32_t points) const
    {
        return iv_reserve_polylines(context_, lines, points);
    }
    bool register_drag_targets(Span<IvDragTarget const> targets) const
    {
        return iv_register_drag_targets(context_, {targets.data, targets.size});
    }
    bool set_settings_panel(IvSettingsPanel panel) const { return iv_set_settings_panel(context_, panel); }
    bool set_context_menu(IvSettingsPanel panel) const { return iv_set_context_menu(context_, panel); }
    bool set_editor_feedback(IvEditorFeedback feedback) const { return iv_set_editor_feedback(context_, feedback); }
    EditGesture begin_edit() const { return {next_edit_id()}; }

    template<class Event>
    EditRequest send(Event const& event, EditGesture gesture = {}) const
    {
        if (!can_edit()) return {};
        auto const request = next_edit_id();
        return {request && submit(event, gesture, request, 0) ? request : 0};
    }

    template<class Event>
    bool replace_pending(Event const& event, EditGesture gesture) const
    {
        return can_edit() && gesture.id && submit(event, gesture, 0, IV_EDIT_REPLACE_PENDING);
    }

    EditRequest finish_edit(EditGesture gesture) const
    {
        if (!gesture.id) return {};
        auto const request = next_edit_id();
        return {request && iv_submit_edit(context_, {0, gesture.id, request, IV_EDIT_FINISH, nullptr}) ? request : 0};
    }
    bool set_timeline_grid(IvTimelineGrid const& grid) const { return iv_set_timeline_grid(context_, &grid); }
    void reuse_drawing() const { iv_reuse_drawing(context_); }
    void set_status(IvString status) const { iv_set_status(context_, status); }
    void request_save() const { iv_request_save(context_); }

private:
    uint32_t next_edit_id() const { return *edit_ids_ == UINT32_MAX ? 0 : ++*edit_ids_; }

    template<class Event>
    bool submit(Event const& event, EditGesture gesture, uint32_t request, uint32_t flags) const
    {
        auto const* payload = detail::EditArena(context_).copy(event);
        return iv_submit_edit(context_, {detail::EventBinding<Event>::index, gesture.id, request, flags, payload});
    }

    IvPresentationContext* context_;
    uint32_t* edit_ids_;
};

class SessionBuffer {
public:
    explicit SessionBuffer(uint32_t size) : memory_(iv_session_allocate(size)) {}
    ~SessionBuffer() { iv_session_free(memory_); }
    SessionBuffer(SessionBuffer const&) = delete;
    SessionBuffer& operator=(SessionBuffer const&) = delete;
    SessionBuffer(SessionBuffer&& other) noexcept : memory_(other.memory_) { other.memory_ = nullptr; }
    void* data() const { return memory_; }

private:
    void* memory_;
};
}

inline void* operator new(size_t, iv::presentation::detail::SessionStorage storage) noexcept
{
    return storage.address;
}

namespace iv::presentation::detail {
template<class Presentation>
struct ClassExports {
    struct Session { Presentation instance{}; uint32_t edit_ids = 0; };
    static_assert(alignof(Presentation) <= 8, "Presentation session alignment exceeds the SDK allocator's alignment");
    static_assert(requires { Presentation{}; }, "Presentation must have a public default constructor and destructor");
    static_assert(requires(Presentation& object, Context& context) {
        object.present(context);
        requires __is_same(decltype(object.present(context)), void);
    }, "Presentation must define void present(Context&)");

    static constexpr bool can_save = requires(Presentation const& object, char* bytes, uint32_t capacity) {
        object.save(bytes, capacity);
        requires __is_same(decltype(object.save(bytes, capacity)), int32_t);
    };
    static constexpr bool can_restore = requires(Presentation& object, IvString serialized) {
        object.restore(serialized);
        requires __is_same(decltype(object.restore(serialized)), int32_t);
    };
    static_assert(can_save || !requires { &Presentation::save; }, "Presentation save must be int32_t save(char*, uint32_t) const");
    static_assert(can_restore || !requires { &Presentation::restore; }, "Presentation restore must be int32_t restore(IvString)");
    static_assert(can_save == can_restore, "Presentation must define both save and restore or omit both");

    static void* create()
    {
        auto* memory = iv_session_allocate(sizeof(Session));
        return memory ? ::new (SessionStorage{memory}) Session{} : nullptr;
    }
    static void destroy(void* state)
    {
        static_cast<Session*>(state)->~Session();
        iv_session_free(state);
    }
    static void present(void* state, IvPresentationContext* raw)
    {
        auto& session = *static_cast<Session*>(state);
        Context context(*raw, session.edit_ids);
        session.instance.present(context);
    }
    static int32_t save(void const* state, char* bytes, uint32_t capacity)
    {
        if constexpr (can_save) return static_cast<Session const*>(state)->instance.save(bytes, capacity);
        return -1;
    }
    static int32_t restore(void* state, IvString serialized)
    {
        if constexpr (can_restore) return static_cast<Session*>(state)->instance.restore(serialized);
        return serialized.size ? -1 : 0;
    }
};
}

#define IV_PRESENTATION_DETAIL_CLASS(Presentation, Flags, ...) \
    IV_PRESENTATION_DETAIL_DESCRIPTOR(Flags, __VA_OPT__(".ascii " IV_PRESENTATION_STRINGIFY(__VA_ARGS__))); \
    IV_PRESENTATION_EXPORT void* iv_create_session(void) { \
        return ::iv::presentation::detail::ClassExports<Presentation>::create(); \
    } \
    IV_PRESENTATION_EXPORT void iv_destroy_session(void* state) { \
        ::iv::presentation::detail::ClassExports<Presentation>::destroy(state); \
    } \
    IV_PRESENTATION_EXPORT void iv_present(void* state, IvPresentationContext* context) { \
        ::iv::presentation::detail::ClassExports<Presentation>::present(state, context); \
    } \
    IV_PRESENTATION_EXPORT int32_t iv_save_state(void const* state, char* bytes, uint32_t capacity) { \
        return ::iv::presentation::detail::ClassExports<Presentation>::save(state, bytes, capacity); \
    } \
    IV_PRESENTATION_EXPORT int32_t iv_restore_state(void* state, char const* bytes, uint32_t size) { \
        return ::iv::presentation::detail::ClassExports<Presentation>::restore(state, {bytes, size}); \
    }

#define IV_PRESENTATION_DETAIL_DEFAULT_CLASS(Presentation) \
    IV_PRESENTATION_DETAIL_CLASS(Presentation, IV_PRESENTATION_NO_FLAGS)

#define IV_PRESENTATION_DETAIL_SELECT_CLASS(Type, Flags, Icon, Register, ...) Register

#define IV_PRESENTATION_CLASS(...) \
    IV_PRESENTATION_DETAIL_SELECT_CLASS(__VA_ARGS__, IV_PRESENTATION_DETAIL_CLASS, \
        IV_PRESENTATION_DETAIL_CLASS, IV_PRESENTATION_DETAIL_DEFAULT_CLASS)(__VA_ARGS__)

#endif
