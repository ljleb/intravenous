#include <intravenous/dsl.h>

consteval void event_loader_voice(iv::GraphBuilder& g)
{
    using namespace iv;
    auto const trigger = g.event_input<"trigger">(EventTypeId::trigger);
    g.event_outputs("trigger"_F = trigger);
    g.outputs();
}
