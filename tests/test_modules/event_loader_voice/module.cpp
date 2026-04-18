#include "dsl.h"

inline void event_loader_voice(iv::ModuleContext const& context)
{
    using namespace iv;
    auto& g = context.builder();

    auto const trigger = g.event_input("trigger", EventTypeId::trigger);
    g.event_outputs("trigger"_F = trigger);
    g.outputs();
}

IV_EXPORT_MODULE("iv.test.event_loader_voice", event_loader_voice);
