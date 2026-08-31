#include "helper/math3d.h"
#include "renderer/font.h"
#include "renderer/text.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

static void expect(int cond, const char *what)
{
    if (cond)
        printf("PASS  %s\n", what);
    else {
        printf("FAIL  %s\n", what);
        g_fail++;
    }
}

int main(void)
{
    struct wp_font font;
    struct wp_text_geom g;
    const struct wp_glyph *a, *w, *i, *sp;
    float ortho[16];
    int ret;

    memset(&font, 0, sizeof(font));
    memset(&g, 0, sizeof(g));

    ret = wp_font_open(&font, "/no/such/font.ttf", 32.0f);
    expect(ret < 0, "missing TTF is an error, process lives");

    ret = wp_font_open_default(&font, 32.0f);
    expect(ret == 0, "open a system TTF");
    if (ret < 0) {
        printf("      no DejaVu/Noto/Liberation on this machine\n");
        return 1;
    }
    expect(font.atlas && font.atlas_w >= 64 && font.atlas_h >= 64, "CPU atlas packed");
    expect(font.ascent > 8.0f && font.line_height > font.ascent, "ascent / line height");

    a = wp_font_glyph(&font, (uint32_t)'A');
    w = wp_font_glyph(&font, (uint32_t)'W');
    i = wp_font_glyph(&font, (uint32_t)'i');
    sp = wp_font_glyph(&font, (uint32_t)' ');
    expect(a && a->present && a->w > 0 && a->h > 0, "'A' has a bitmap");
    expect(w && i && w->advance > i->advance, "'W' advances more than 'i'");
    expect(sp && sp->advance > 0.0f, "space advances with no requirement to draw");

    ret = wp_text_layout(&font, "", 0, 0, &g);
    expect(ret == 0 && g.ni == 0, "empty string → no quads");
    wp_text_geom_free(&g);

    ret = wp_text_layout(&font, "Hello", 10, 20, &g);
    expect(ret == 0 && g.ni == 5 * 6 && g.nv == 5 * 4, "\"Hello\" is 5 glyphs");
    expect(g.x1 > g.x0 && g.y1 > g.y0, "layout AABB is a rectangle");
    expect(g.x0 >= 10.0f - 8.0f, "origin is near the left of the run");
    {
        float tl[3] = { g.v[0].x, g.v[0].y, 0 };
        float bl[3] = { g.v[1].x, g.v[1].y, 0 };
        float br[3] = { g.v[2].x, g.v[2].y, 0 };
        wp_mat4_ortho_pixel(ortho, 1280.0f, 720.0f);
        expect(wp_triangle_front_facing(ortho, tl, bl, br),
               "glyph quad TL-BL-BR is a raster front face (not culled)");
    }
    wp_text_geom_free(&g);

    ret = wp_text_layout(&font, "A\nB", 0, 0, &g);
    expect(ret == 0 && g.ni == 2 * 6, "newline is two glyphs");
    expect(g.y1 - g.y0 > font.size_px * 1.2f, "newline grows the AABB down");
    wp_text_geom_free(&g);

    ret = wp_text_layout(&font, "AV", 0, 0, &g);
    expect(ret == 0 && g.ni == 12, "two-letter run");
    wp_text_geom_free(&g);

    ret = wp_text_layout(&font, "\xE2\x82\xAC", 0, 0, &g); /* euro sign, not in ASCII set */
    expect(ret == 0 && g.ni == 6, "unknown UTF-8 becomes one replacement glyph, not a crash");
    wp_text_geom_free(&g);

    wp_font_destroy(&font);
    printf("%s  %d failure(s)\n", g_fail ? "RESULT FAIL" : "RESULT PASS", g_fail);
    return g_fail ? 1 : 0;
}
