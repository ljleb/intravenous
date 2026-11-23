# Build a native LLVM node

Build a mono node that halves each input sample. You will compile a package
and see its public node in Intravenous. Start in the
[development shell](../../development_run.md).

1. Create a scratch project under the repository and add a package.

   ```sh
   mkdir -p build-release
   scratch=$(mktemp -d "$PWD/build-release/iv-node.XXXXXX")
   mkdir -p "$scratch/packages/half_gain"
   touch "$scratch/iv_project.jsonl"
   ```

2. Save this as `packages/half_gain/iv_package.json` in the scratch project.

<!-- check-example: iv_package.json -->
```json
{
  "schema": 2,
  "namespace": "example.half_gain",
  "entry": "module.cpp"
}
```

3. Save this as `packages/half_gain/module.cpp`.

<!-- check-example: module.cpp -->
```cpp
#include <intravenous/dsl.h>
#include <array>

struct HalfGain {
    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in")};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out")};
    }

    void tick(iv::TickSampleContext<HalfGain> const& ctx) const
    {
        ctx.output<"out">()(0) = ctx.input<"in">().get() * 0.5f;
    }
};

IV_NODE("example.half_gain", HalfGain, "Half Gain");
```

4. Run `scripts/run_dev.sh "$scratch"`. Open the project graph, press
   **Shift+A**, and add **Half Gain**. The lane has an input named
   `in` and an output named `out`.
5. Set the input knob to `1`. The output value is `0.5`. Change
   `0.5f` to `0.25f` in the source and save. After the native package
   reloads, the output value is `0.25`.

The package compiler turns the C++ source into LLVM bitcode. `IV_NODE`
gives the loader a public definition with a stable ID and a display name.
The named accessors bind the callback to the declared ports.
[Native node reference](../reference/llvm_nodes.md)
