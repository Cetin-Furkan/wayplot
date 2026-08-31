# Document (CPU owner)

This is the step after `docs/DEM.md`. A document is a CPU array of live
cards and mesh pointers. Views observe it. `main` is a caller that fills
it. See `docs/HOST.md`.

---

## The hole

Two views, two cards, one mesh/DEM, one font — all stack locals in
`run()`. Adding a second DEM or a plot meant editing `main` again. That
is how the cube almost became “the 3D object.” Views existed. Draws
existed. The document did not.

## Contract

- **`wp_doc`** lives in `src/engine`. Fixed arrays (`WP_DOC_MAX_MESH` 8,
  `WP_DOC_MAX_CARD` 64). Not ECS, not a scene graph, not bindless.
- **Cards** are `{wp_rect, rgba}`. Copied on add. The rect is still the
  source of truth; changing `w` on the doc and recording again is how a
  card resizes. Hit ids are `card index + 1` (`0` is never a hit).
- **Meshes** are `wp_mesh *`. The doc does **not** own GPU memory. It
  does not call `wp_mesh_destroy`. `wp_doc_destroy` drops the slots.
- **No `Vk*` / `wl_*` members.** The document does not begin rendering.
  Overlay cards go through `wp_doc_push_cards` → the existing list.
  Lit meshes are read by the caller (`wp_doc_mesh`) inside an already
  begun opaque pass, after `wp_view_bind`.
- **Hits:** `wp_doc_fill_hits` pushes each card onto a caller-owned
  stack (does not clear). Logical pixels, same space as the pointer.
- **Drag:** `wp_doc_apply_drag` writes `x,y` for `hit_id - 1`. `w,h`
  stay. `hit_id == 0` is a no-op.
- Overflow is `-ENOSPC`. Empty/`w<=0` rect, NULL mesh, NULL rgba:
  `-EINVAL`.

Option B for v1: GPU mesh pointers, not a CPU triangle owner. DEM
retessellate stays a load-time producer until a measured need.

`main` fills the doc with two cards and one mesh. The loop rebuilds the
hit stack and the overlay list from the doc. Views stay two panes of the
same doc.

## Tests

| Test | Lock |
|---|---|
| `test-engine-doc` CPU | Null / empty / overflow. Two cards + one mesh pointer. Copies rect and rgba. Destroy does not free the mesh. `fill_hits` pick A then B. `apply_drag` moves B’s `x,y` only. 64 cards, `fill_hits`: **351.75 ns/fill** (this i7-1165G7). |
| `test-engine-doc` GPU | Cards and cube from the doc. Magenta A + green B. Shrink B’s `w` **on the doc**; A unmoved; the strip B no longer owns has no green. Cube center is +Z inside a full-FB view (not a fullscreen panel). Heap `session` / `present`. |

A window that still names `panel0` / `panel1` as the architecture, with
the list and hit stack reading those locals, is not this step.

## What we did not do

No `wp_entity`, UUID, JSON, undo. No third `BeginRendering`. No
instancing because the doc has N cards. No present / io_uring inside the
doc. No plan camera. No HarfBuzz, glTF PBR, VMA. No `CULL_NONE`.
