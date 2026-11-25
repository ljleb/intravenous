# Lane Parents Before General Nesting

_Historical wording from `module_interoperability_and_graph_macros_direction.md`,
preserved on 2026-11-25. The owner extended lane nesting to non-module parents
and chose nearest-ancestor fallback. See the [current lane nesting design](../module_interoperability_and_graph_macros_direction.md#project-lane-nesting)._

### Project lanes within modules

A project lane can sit under a module instance in the graph editor. The
project presentation stores the parent address and the lane order. The node
keeps its project instance ID, ports, and connections. Operators can still
edit, wire, duplicate, or delete the project node from its nested lane.
If the parent address becomes unavailable, the editor shows the lane at the
root until the address resolves again.

Shift+Right moves each selected run into the module immediately above it at
the same level. Shift+Left moves nodes one module up. Root lanes stay in
place on Shift+Left. Shift+Up and Shift+Down reorder siblings. Shift+drag
from a selected header moves the selection to a module or a sibling insertion
point. Module Ports and C++ children keep their source-defined order. A
project edit saves the parent changes and order together for undo and redo.
Project lanes can move past C++ siblings. Module Ports stays at the top.
