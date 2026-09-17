#include <intravenous/graph_jit/lowering.h>

namespace iv::graph_jit {
std::expected<LoweringOutput, std::string> lower_configured_graph_to_llvm(
    LoweringInput const&,
    llvm::Module&)
{
    return std::unexpected(
        "ConfiguredGraph -> LLVM IR lowering has not been implemented yet");
}
} // namespace iv::graph_jit
