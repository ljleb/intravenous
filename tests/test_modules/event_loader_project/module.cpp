#include <intravenous/dsl.h>

void event_loader_project(iv::GraphBuilder& g)
{
    using namespace iv;
    auto const root_trigger = g.event_input<"trigger">(EventTypeId::trigger);
    auto const voice = g.node<"iv.test.event_loader_voice">();
    voice.connect_event_input("trigger", root_trigger);
    g.event_outputs("trigger"_F = voice.event_port("trigger"));
    g.outputs();
}

IV_MODULE("iv.test.event_loader_project", event_loader_project);
