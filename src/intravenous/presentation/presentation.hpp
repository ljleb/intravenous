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

class Context {
public:
    explicit Context(IvPresentationContext& context) : context_(&context) {}
    IvRectangle content_bounds() const { return iv_content_bounds(context_); }
    bool can_edit() const { return iv_can_edit(context_); }
    uint32_t lane_mode() const { return iv_lane_mode(context_); }
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
    Span<IvGestureInput const> gestures() const
    {
        auto const input = iv_gesture_inputs(context_);
        return {input.data, input.size};
    }
    bool draw_polylines(Span<IvPolyline const> lines) const
    {
        return iv_draw_polylines(context_, {lines.data, lines.size});
    }
    bool draw_texts(Span<IvText const> texts) const
    {
        return iv_draw_texts(context_, {texts.data, texts.size});
    }
    IvPolylineBatch reserve_polylines(uint32_t lines, uint32_t points) const
    {
        return iv_reserve_polylines(context_, lines, points);
    }
    bool register_drag_targets(Span<IvDragTarget const> targets) const
    {
        return iv_register_drag_targets(context_, {targets.data, targets.size});
    }
    void reuse_drawing() const { iv_reuse_drawing(context_); }
    void set_status(IvString status) const { iv_set_status(context_, status); }
    void request_save() const { iv_request_save(context_); }

private:
    IvPresentationContext* context_;
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
        auto* memory = iv_session_allocate(sizeof(Presentation));
        return memory ? ::new (SessionStorage{memory}) Presentation{} : nullptr;
    }
    static void destroy(void* state)
    {
        static_cast<Presentation*>(state)->~Presentation();
        iv_session_free(state);
    }
    static void present(void* state, IvPresentationContext* raw)
    {
        Context context(*raw);
        static_cast<Presentation*>(state)->present(context);
    }
    static int32_t save(void const* state, char* bytes, uint32_t capacity)
    {
        if constexpr (can_save) return static_cast<Presentation const*>(state)->save(bytes, capacity);
        return -1;
    }
    static int32_t restore(void* state, IvString serialized)
    {
        if constexpr (can_restore) return static_cast<Presentation*>(state)->restore(serialized);
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
