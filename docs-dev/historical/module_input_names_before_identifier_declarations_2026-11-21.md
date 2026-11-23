# Earlier module input naming forms

These forms predate the owner decision on 2026-11-21 to infer a public input
name from its C++ identifier. See the
[current public input naming design](../iv_module_source_introspection_direction.md#public-input-names).

[Modules Follow-Up Notes](../modules_follow_up_notes.md) recorded the earlier
positional name and unnamed forms.

> `g.input()` accepts both named and unnamed configured forms:
> `g.input("name", default, min, max)` and `g.input(default, min, max)`.

[Vue Presentation Refinement Direction](../vue_presentation_refinement_direction.md)
used a string template argument in its frequency knob example.

```cpp
auto frequency = g.input<"freq", Sample>({
    .default_value = Sample{220.f}, .min = Sample{0.f},
    .scrub_speed = Sample{2.f},
});
```
