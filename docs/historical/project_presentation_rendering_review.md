# Initial project presentation rendering review

This preserves the rendering discussion and proposed sequence from
“Project-authored presentation lanes (2026-11-20)” in
[`implementation_fronts_direction.md`](../implementation_fronts_direction.md#project-authored-presentation-lanes-2026-11-20).
That section now records the accepted direction and pilot.

> The current `PresentationRegistry` only selects Vue components built into the
> client. `AutomationCurvePresentation` receives the internal graph editor
> context and hands it a DOM pointer event to start a drag. `BeatsSettings`
> uses the internal beats editing controller. A separate execution context needs
> a serializable boundary for those operations.
>
> The rendering boundary is an open design choice. A sandboxed Vue renderer
> allows custom layouts. It needs a bridge for pointer gestures, coordinates,
> host menus, and cursor return. Host-rendered controls and drawing primitives
> put those interactions in the host and constrain the UI plugins can build.
> Importing project Vue components directly into the graph editor would share
> its browser context. A TypeScript interface alone does not enforce isolation.
>
> After the rendering choice, define host-to-plugin snapshots and plugin-to-host
> commands, including their version, ownership, and disposal rules. Use the
> automation curve and Beats lanes to prove the interface. Then add project
> discovery and loading through the isolated runtime.

## Initial remote artifact proposal

This excerpt preserves the earlier remote delivery sequence from the same
section. The implementation front now records WebAssembly as the first
distributed presentation format.

> Prototype distribution with the portable script format first. A WebAssembly
> artifact would also need an explicit host ABI for snapshots, input, and
> results. LLVM bitcode would add compiler and target ABI requirements.

## Original project presentation folder

The pilot first loaded presentation files from a hidden project folder. On
2026-11-21, the owner chose a folder inside each package. See the current
[project presentation guide](../project_presentation_plugins.md#add-a-presentation).

The setup guide originally said:

> In a trusted Intravenous workspace, place a manifest and a single JavaScript
> file of the same name in `.intravenous/presentations/`. The graph editor
> reloads the catalog when those files change. Use a stable plugin ID and one
> or more backend definition IDs from the project. This example attaches a
> draggable marker to Beats for trying the interface. Change `definitionIds`
> to attach it to your own node.
>
> `.intravenous/presentations/marker.json`
>
> `.intravenous/presentations/marker.js`

The implementation front described its original discovery path:

> The pilot discovers `.intravenous/presentations/` and takes one candidate per
> definition. Package discovery should publish a catalog of candidates from
> the resolved packages.
