# VST Hosting And Presentation Direction

_Status: follow-up ideas from the 2026-11-14/15 discussion; **not** an
implementation contract for native/remote plugin editors or binary lifting._

VST integration is a separate product feature family from the built-in IV node
ecosystem. The snapshot contains JUCE/VST discovery/runtime infrastructure, but
`src/intravenous/juce/vst_wrapper.h` currently rejects `iv::juce::vst(...)`,
including when `IV_ENABLE_JUCE_VST=1`, pending discovery-generated node types.
Do not present the end-to-end node wrapper as implemented.

## Graph and automation integration

A VST wrapper should make its available audio buses, event interfaces, and
plugin parameters available through IV's ordinary graph/port and automation
mechanisms, with the applicable parameter metadata. Plugin state beyond simple
parameters also needs a representation consistent with the project's persistence
requirements. The exact wrapper-generated node types, state format, and port
mapping have not been settled here. A plugin should be useful from generated
controls and normal IV connections even when its custom editor is inaccessible.
No separate port-enablement gesture should be required to connect automation.

## Custom plugin editors: possible approaches, not selected designs

A native VST editor is generally an operating-system window/view rather than an
HTML/Vue component that a VSCodium webview can directly embed. Possible paths
for future investigation are:

- a **local native popup** managed by the host/helper process;
- a **remote editor stream** from the machine hosting the VST, with frames and
  input relayed to remote webview/browser clients;
- a **generic Vue editor** built from available plugin parameter metadata.

A remote editor could be valuable for live collaboration where clients do not
have the plugin installed. Questions about OS window capture, popups/focus,
input arbitration, security, latency, and the plugin's state synchronization
remain open. A possible initial interaction policy is multiple viewers and one
active operator per native editor, but that is **not an accepted protocol**.
Regardless of GUI approach, the host-side plugin/parameter state must not be
inferred from captured editor pixels.

## Compiling plugin DSP into GraphJit

Binary lifting of arbitrary compiled VSTs into LLVM IR is theoretically
conceivable but was **not adopted as a roadmap feature**: recovered binaries
are not source-level IR, and correctness, dynamic behavior, state, optimization,
and plugin licensing make this a research problem. Source-available DSP may
present a different, more credible future optimization opportunity. Ordinary
VSTs can remain native while GraphJit optimizes the surrounding IV graph.
