# View pan and dolly

This is the step after `docs/VIEW.md`. Orbit was yaw/pitch only. Pitch is
clamped on purpose (turntable: yaw wraps, pitch cannot flip through the
poles). Pan moves the look-at. Scroll changes distance. Same view, same
pass. See `docs/HOST.md`.

---

## The hole

Left-drag orbits. There was no way to slide the look-at or zoom. A second
view would still be a camera you cannot frame.

## Contract

- **Pan:** right or middle button, inside the view, not a card, not window
  chrome. Sticky until that button releases. Content follows the pointer
  (`center += (-right * dx + up * dy) * dist * 0.0015`). Distance and
  yaw/pitch stay.
- **Dolly:** `wl_pointer.axis` vertical, wl_fixed, accumulated per pump.
  Positive = zoom out. `dist *= exp(axis/256 * 0.08)`, clamped to
  `[0.15, 18]`. Applies whenever the pointer is inside the view (scroll
  while orbiting is allowed).
- Session snapshots **middle/right** bits and axis. Host still decides.
- Pitch clamp is unchanged: **not a bug**. Yaw is a circle around world
  +Y; pitch ±89° keeps `up` from going parallel to the view axis.

## Tests

| Test | Lock |
|---|---|
| `test-wayland-proto` | `wl_pointer.axis` opcode 4 |
| `test-engine-view` CPU | Pan changes center, not dist. Dolly changes dist, not center. Dist clamp. Right press in the pane pans; left still orbits. Axis copied only inside the view. |
| measure | 100k `wp_view_pan(1,0)`: **49.53 ns/call** (this i7-1165G7). |

## What we did not do

No second view. No keyboard modifiers. No document type. No DEM. No
`CULL_NONE`.
