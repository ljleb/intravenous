# Lane View Layout Persistence Note

Lane-view persistence is not just an ordering problem.

What actually needs to be preserved is explicit UI layout structure:

- 2D placement
- sizing
- any additional editor layout state needed to reopen the project as it was

Important implications:

- command ordering is not sufficient to represent this
- the structure should be stored explicitly
- restoring that structure is VS Code / app-module work, not just runtime
  persistence work

Current direction:

- keep stable lane-view ids in project persistence
- later add explicit persisted layout structure for those views
- restore that layout from the VS Code side on project load

This item is intentionally deferred for now.
