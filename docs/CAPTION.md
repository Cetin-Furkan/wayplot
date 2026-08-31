# Card captions (instrument data)

This is the step after `docs/DRAPE.md`. A card is still `wp_rect` + rgba.
It now also holds a short ASCII caption from the document. Overlay draws
the card, then glyph quads on top. Not a widget toolkit. See `docs/HOST.md`.

---

## The hole

Two cards were empty color. The version string sat in the corner. Nothing
on the panel said what the document was: cube vs DEM, plot length, min/max.
Instrument-style cards that do not show data are paint.

## Contract

- **`wp_doc_card.caption`:** copied C string, cap `WP_DOC_CAPTION` (80).
  `wp_doc_set_caption` truncates. Empty is allowed (no glyphs).
- **Layout** is still `wp_text_layout`. Origin is the card’s logical
  `x,y` plus padding, times buffer scale. `\n` is a new line. Relayout
  every frame so a drag moves the letters with the rect.
- **Overlay order unchanged:** cards, then text. Caption color is the
  existing overlay white.
- **`main`:** card 0 names the 3D object (cube / mesh file / `DEM WxH`).
  Card 1 is the plot (`n`, min, max). Image drape is a second line on
  card 0.

A window that still only prints `wayplot 0.x.x` in the corner, with blank
cards, is not this step.

## Tests

| Test | Lock |
|---|---|
| `test-engine-doc` CPU | Set caption copies. Truncate. Empty OK. |
| `test-engine-doc` GPU | Card + `"HHHH"`: **120** white pixels in the AABB. Empty caption: **0**. Cube center still +Z. Heap `session` / `present`. |

## What we did not do

No HarfBuzz. No editable text field. No third pass. No ImGui. No
trackball.
