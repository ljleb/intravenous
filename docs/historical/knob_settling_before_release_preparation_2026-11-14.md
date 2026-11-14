# Knob Settling Before Release-Time Preparation

Historical contract preserved from `node_presentation_and_manual_controls_direction.md`
on 2026-11-14. The owner clarified that release prepares the settled variant while
hover leave permits activation. The replacement is in the source document.

```text
mouse releases while still over the knob:
    close the undo gesture
    keep the dynamic graph and latest target active

pointer leaves the knob after release:
    capture the latest stored value
    immediately request a settled replacement (no debounce timer)

settled graph becomes active:
    use the manual value as an exact constant where it participates
    otherwise restore the authored connection
```

The Vue-stage checkpoint also previously stated: “Hover, release and hover leave
remain separate events: release commits; hover leave requests settled code.” The
updated checkpoint separates release-time preparation from permission to activate.
