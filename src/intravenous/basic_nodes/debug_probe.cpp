#include <intravenous/basic_nodes/debug_probe.h>

#include <iostream>

namespace iv::details {
void write_debug_probe_sample(char const* label, size_t tick_index, Sample sample)
{
    std::cout << label << "[" << tick_index << "] = " << sample << '\n';
}
} // namespace iv::details
