#include <intravenous/dsl.h>

void event_loader_project(iv::GraphBuilder& g)
{
    using namespace iv;
    auto const trigger = g.event_input({"Trigger"}, EventTypeId::trigger);
    auto const voice = g.node<"iv.test.event_loader_voice">();
    voice.connect_event_input("trigger", trigger);
    g.event_outputs("trigger"_F.with_display_name("Trigger") = voice.event_port("trigger"));
    g.outputs();
}

IV_MODULE("iv.test.event_loader_project", event_loader_project, "Event loader project");
