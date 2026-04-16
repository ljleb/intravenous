#include "dsl.h"

inline void event_loader_project(iv::ModuleContext const& context)
{
    using namespace iv;
    auto& g = context.builder();

    auto const voice_builder = context.load_builder("iv.test.event_loader_voice");
    auto const root_trigger = g.event_input("trigger", EventTypeId::trigger);
    auto const voice = g.embed_subgraph(voice_builder);

    voice.connect_event_input("trigger", root_trigger);
    g.event_outputs("trigger"_P = ((voice >> events) >> "trigger"_P));
    g.outputs();
}

IV_EXPORT_MODULE("iv.test.event_loader_project", event_loader_project);
