#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine/doc.h"
#include "engine/draw.h"
#include "engine/hit.h"
#include "engine/input.h"
#include "engine/present.h"
#include "engine/view.h"
#include "helper/log.h"
#include "helper/math3d.h"
#include "renderer/card.h"
#include "renderer/font.h"
#include "renderer/image.h"
#include "renderer/lit.h"
#include "renderer/dem.h"
#include "renderer/grid.h"
#include "renderer/mesh.h"
#include "renderer/obj.h"
#include "renderer/plot.h"
#include "renderer/pass.h"
#include "renderer/text.h"
#include "version.h"
#include "wayland/session.h"

static void print_help(const char *argv0)
{
    printf("usage: %s [--version] [--help] [--mesh FILE] [--dem FILE] [--plot FILE] [--image FILE]\n",
           argv0);
    printf("  --mesh FILE  Wavefront OBJ. --dem FILE  PGM P5 heightmap.\n");
    printf("  --plot FILE  ASCII floats (1D series). Default is a sine ribbon.\n");
    printf("  --image FILE  P6 PPM. Drapes on --dem; otherwise a ground under the scene.\n");
    printf("  Bad content prints an error; the window still opens (no silent cube).\n");
    printf("  wayplot %s (%s)\n", WP_VERSION_STRING, WP_BUILD_TYPE);
}

static void scene_add_aabb(struct wp_aabb *scene, int *have, const struct wp_aabb *b)
{
    if (!scene || !have || !wp_aabb_ok(b))
        return;
    if (!*have) {
        *scene = *b;
        *have = 1;
        return;
    }
    (void)wp_aabb_union(scene, scene, b);
}

static const char *fname(const char *p)
{
    const char *s;

    if (!p || !p[0])
        return "";
    s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static int fill_plot_mesh(struct wp_device *d, struct wp_plot *plot, struct wp_mesh *out,
                          const char *path)
{
    struct wp_mesh_cpu cpu;
    uint32_t i;
    int ret;

    memset(&cpu, 0, sizeof(cpu));
    if (path) {
        ret = wp_plot_load(path, plot);
        if (ret < 0)
            return ret;
    } else {
        ret = wp_plot_resize(plot, 64);
        if (ret < 0)
            return ret;
        for (i = 0; i < 64; i++)
            plot->y[i] = 0.5f + 0.5f * sinf((float)i * 6.2831853f / 63.0f);
    }
    ret = wp_plot_tessellate(plot, WP_PLOT_AMP, &cpu);
    if (ret < 0)
        return ret;
    ret = wp_mesh_upload(d, out, cpu.v, cpu.nv, cpu.idx, cpu.ni);
    wp_mesh_cpu_free(&cpu);
    return ret;
}

static int run(const char *mesh_path, const char *dem_path, const char *plot_path,
               const char *image_path)
{
    char path[256];
    struct wp_session *s;
    struct wp_present *p;
    struct wp_pass *pass;
    struct wp_draw_list draws;
    struct wp_hit_stack hits;
    struct wp_input input;
    struct wp_doc doc;
    struct wp_mesh *mesh;
    struct wp_mesh *plot_mesh;
    struct wp_mesh *image_mesh;
    struct wp_mesh *grid_mesh;
    struct wp_plot plot;
    struct wp_image image;
    struct wp_tex image_tex;
    struct wp_lit *lit;
    struct wp_card *card;
    struct wp_font *font;
    struct wp_text *text;
    struct wp_text_geom label;
    struct wp_text_geom cap[2];
    struct wp_view views[2];
    char line[128];
    float white[4] = { 0.92f, 0.93f, 0.94f, 1.0f };
    float card0_rgba[4] = { 0.14f, 0.28f, 0.34f, 1.0f };
    float card1_rgba[4] = { 0.22f, 0.24f, 0.16f, 1.0f };
    int ret;
    int have_dem = 0;
    int have_scene = 0;
    uint32_t dem_cols = 0, dem_rows = 0;
    float grid_step = 0.0f;
    struct wp_aabb scene;

    ret = wp_wl_display_path(path, sizeof(path));
    if (ret < 0) {
        fprintf(stderr, "no Wayland display (XDG_RUNTIME_DIR / WAYLAND_DISPLAY)\n");
        return 1;
    }

    s = calloc(1, sizeof(*s));
    p = calloc(1, sizeof(*p));
    pass = calloc(1, sizeof(*pass));
    mesh = calloc(1, sizeof(*mesh));
    plot_mesh = calloc(1, sizeof(*plot_mesh));
    image_mesh = calloc(1, sizeof(*image_mesh));
    grid_mesh = calloc(1, sizeof(*grid_mesh));
    lit = calloc(1, sizeof(*lit));
    card = calloc(1, sizeof(*card));
    font = calloc(1, sizeof(*font));
    text = calloc(1, sizeof(*text));
    if (!s || !p || !pass || !mesh || !plot_mesh || !image_mesh || !grid_mesh || !lit || !card ||
        !font || !text) {
        free(s);
        free(p);
        free(pass);
        free(mesh);
        free(plot_mesh);
        free(image_mesh);
        free(grid_mesh);
        free(lit);
        free(card);
        free(font);
        free(text);
        return 1;
    }
    memset(&label, 0, sizeof(label));
    memset(&cap, 0, sizeof(cap));
    memset(&plot, 0, sizeof(plot));
    memset(&image, 0, sizeof(image));
    memset(&image_tex, 0, sizeof(image_tex));
    wp_doc_init(&doc);
    wp_draw_list_init(&draws);
    wp_hit_clear(&hits);
    wp_input_init(&input);
    wp_view_init(&views[0]);
    wp_view_init(&views[1]);
    wp_view_plan(&views[1]);
    wp_aabb_reset(&scene);
    ret = wp_doc_add_card(&doc, (struct wp_rect){ 24, 56, 200, 88 }, card0_rgba);
    if (ret < 0)
        fprintf(stderr, "doc card 0: %s\n", strerror(-ret));
    ret = wp_doc_add_card(&doc, (struct wp_rect){ 240, 56, 200, 88 }, card1_rgba);
    if (ret < 0)
        fprintf(stderr, "doc card 1: %s\n", strerror(-ret));

    ret = wp_session_open(s);
    if (ret < 0) {
        fprintf(stderr, "open: %s\n", strerror(-ret));
        goto fail_free;
    }
    printf("wayplot %s  %s\n", WP_VERSION_STRING, path);
    wp_registry_print(&s->reg, stdout);

    ret = wp_session_setup_surface(s);
    if (ret < 0) {
        fprintf(stderr, "surface/feedback: %s\n", strerror(-ret));
        wp_session_close(s);
        goto fail_free;
    }
    wp_session_print(s, stdout);

    ret = wp_present_open(p, s);
    if (ret < 0) {
        fprintf(stderr, "present: %s\n", strerror(-ret));
        wp_session_close(s);
        goto fail_free;
    }
    if (dem_path) {
        struct wp_dem dem;
        struct wp_mesh_cpu cpu;
        memset(&dem, 0, sizeof(dem));
        memset(&cpu, 0, sizeof(cpu));
        ret = wp_dem_load(dem_path, &dem);
        if (ret < 0) {
            fprintf(stderr, "dem %s: %s\n", dem_path, strerror(-ret));
        } else {
            ret = wp_dem_tessellate(&dem, WP_DEM_AMP, &cpu);
            if (ret == 0) {
                dem_cols = dem.cols;
                dem_rows = dem.rows;
            }
            wp_dem_free(&dem);
            if (ret < 0) {
                fprintf(stderr, "dem tessellate: %s\n", strerror(-ret));
            } else {
                if (image_path) {
                    uint32_t vi;
                    for (vi = 0; vi < cpu.nv; vi++) {
                        cpu.v[vi].r = 1.0f;
                        cpu.v[vi].g = 1.0f;
                        cpu.v[vi].b = 1.0f;
                    }
                }
                ret = wp_mesh_upload(&p->device, mesh, cpu.v, cpu.nv, cpu.idx, cpu.ni);
                wp_mesh_cpu_free(&cpu);
                if (ret < 0) {
                    fprintf(stderr, "dem upload: %s\n", strerror(-ret));
                    wp_present_close(p);
                    wp_session_close(s);
                    goto fail_free;
                }
                have_dem = 1;
                scene_add_aabb(&scene, &have_scene, &mesh->aabb);
            }
        }
    } else if (mesh_path) {
        struct wp_mesh_cpu cpu;
        ret = wp_obj_load(mesh_path, &cpu);
        if (ret < 0) {
            fprintf(stderr, "mesh %s: %s\n", mesh_path, strerror(-ret));
        } else {
            ret = wp_mesh_upload(&p->device, mesh, cpu.v, cpu.nv, cpu.idx, cpu.ni);
            wp_mesh_cpu_free(&cpu);
            if (ret < 0) {
                fprintf(stderr, "mesh upload: %s\n", strerror(-ret));
                wp_present_close(p);
                wp_session_close(s);
                goto fail_free;
            }
            scene_add_aabb(&scene, &have_scene, &mesh->aabb);
        }
    } else {
        ret = wp_mesh_cube(&p->device, mesh);
        if (ret < 0) {
            fprintf(stderr, "mesh: %s\n", strerror(-ret));
            wp_present_close(p);
            wp_session_close(s);
            goto fail_free;
        }
        scene_add_aabb(&scene, &have_scene, &mesh->aabb);
    }
    if (mesh->index_count) {
        ret = wp_doc_add_mesh(&doc, mesh);
        if (ret < 0)
            fprintf(stderr, "doc mesh: %s\n", strerror(-ret));
    }
    ret = fill_plot_mesh(&p->device, &plot, plot_mesh, plot_path);
    if (ret < 0) {
        fprintf(stderr, "plot%s%s: %s\n", plot_path ? " " : "", plot_path ? plot_path : "",
                strerror(-ret));
        wp_plot_free(&plot);
    } else {
        ret = wp_doc_add_plot(&doc, &plot);
        if (ret < 0)
            fprintf(stderr, "doc plot: %s\n", strerror(-ret));
        if (plot_mesh->index_count) {
            ret = wp_doc_add_mesh(&doc, plot_mesh);
            if (ret < 0)
                fprintf(stderr, "doc plot mesh: %s\n", strerror(-ret));
            else
                scene_add_aabb(&scene, &have_scene, &plot_mesh->aabb);
        }
    }
    if (image_path) {
        struct wp_mesh_cpu cpu;
        memset(&cpu, 0, sizeof(cpu));
        ret = wp_image_load(image_path, &image);
        if (ret < 0) {
            fprintf(stderr, "image %s: %s\n", image_path, strerror(-ret));
        } else {
            ret = wp_tex_upload(&p->device, &image_tex, &image);
            if (ret < 0) {
                fprintf(stderr, "image upload: %s\n", strerror(-ret));
                wp_image_free(&image);
            } else if (have_dem) {
                ret = wp_doc_set_albedo(&doc, 0, &image_tex);
                if (ret < 0)
                    fprintf(stderr, "doc drape: %s\n", strerror(-ret));
            } else if (have_scene) {
                ret = wp_image_ground(&scene, &cpu);
                if (ret == 0)
                    ret = wp_mesh_upload(&p->device, image_mesh, cpu.v, cpu.nv, cpu.idx, cpu.ni);
                wp_mesh_cpu_free(&cpu);
                if (ret < 0) {
                    fprintf(stderr, "image ground: %s\n", strerror(-ret));
                    wp_tex_destroy(&image_tex);
                    wp_image_free(&image);
                } else {
                    ret = wp_doc_add_mesh(&doc, image_mesh);
                    if (ret == 0) {
                        scene_add_aabb(&scene, &have_scene, &image_mesh->aabb);
                        ret = wp_doc_set_albedo(&doc, wp_doc_nmesh(&doc) - 1, &image_tex);
                    }
                    if (ret < 0)
                        fprintf(stderr, "doc image: %s\n", strerror(-ret));
                }
            } else {
                ret = wp_image_quad(&cpu);
                if (ret == 0)
                    ret = wp_mesh_upload(&p->device, image_mesh, cpu.v, cpu.nv, cpu.idx, cpu.ni);
                wp_mesh_cpu_free(&cpu);
                if (ret < 0) {
                    fprintf(stderr, "image quad: %s\n", strerror(-ret));
                    wp_tex_destroy(&image_tex);
                    wp_image_free(&image);
                } else {
                    ret = wp_doc_add_mesh(&doc, image_mesh);
                    if (ret == 0) {
                        scene_add_aabb(&scene, &have_scene, &image_mesh->aabb);
                        ret = wp_doc_set_albedo(&doc, wp_doc_nmesh(&doc) - 1, &image_tex);
                    }
                    if (ret < 0)
                        fprintf(stderr, "doc image: %s\n", strerror(-ret));
                }
            }
        }
    }
    if (have_scene) {
        struct wp_grid grid;
        struct wp_mesh_cpu gcpu;

        memset(&grid, 0, sizeof(grid));
        memset(&gcpu, 0, sizeof(gcpu));
        ret = wp_grid_from_aabb(&scene, &grid);
        if (ret < 0) {
            fprintf(stderr, "grid: %s\n", strerror(-ret));
        } else {
            ret = wp_grid_tessellate(&grid, &gcpu);
            if (ret < 0) {
                fprintf(stderr, "grid tessellate: %s\n", strerror(-ret));
            } else {
                ret = wp_mesh_upload(&p->device, grid_mesh, gcpu.v, gcpu.nv, gcpu.idx, gcpu.ni);
                wp_mesh_cpu_free(&gcpu);
                if (ret < 0) {
                    fprintf(stderr, "grid upload: %s\n", strerror(-ret));
                } else {
                    ret = wp_doc_add_mesh(&doc, grid_mesh);
                    if (ret < 0)
                        fprintf(stderr, "doc grid: %s\n", strerror(-ret));
                    else
                        grid_step = grid.step;
                }
            }
        }
    }
    {
        char buf[WP_DOC_CAPTION];
        if (have_dem) {
            if (image_path)
                snprintf(buf, sizeof(buf), "DEM %ux%u\n+ %s", dem_cols, dem_rows, fname(image_path));
            else
                snprintf(buf, sizeof(buf), "DEM %ux%u\n%s", dem_cols, dem_rows, fname(dem_path));
        } else if (mesh_path) {
            snprintf(buf, sizeof(buf), "mesh\n%s", fname(mesh_path));
        } else if (image_path) {
            snprintf(buf, sizeof(buf), "cube\n+ %s", fname(image_path));
        } else {
            snprintf(buf, sizeof(buf), "cube");
        }
        if (grid_step > 0.0f) {
            char tail[24];
            int tlen = snprintf(tail, sizeof(tail), "\ngrid %.4g", grid_step);
            size_t n = strlen(buf);

            if (tlen > 0 && n + (size_t)tlen < sizeof(buf))
                memcpy(buf + n, tail, (size_t)tlen + 1);
        }
        (void)wp_doc_set_caption(&doc, 0, buf);
        if (plot.n) {
            float mn = plot.y[0], mx = plot.y[0];
            uint32_t i;
            for (i = 1; i < plot.n; i++) {
                if (plot.y[i] < mn)
                    mn = plot.y[i];
                if (plot.y[i] > mx)
                    mx = plot.y[i];
            }
            snprintf(buf, sizeof(buf), "plot n=%u\n%.2f .. %.2f", plot.n, mn, mx);
            (void)wp_doc_set_caption(&doc, 1, buf);
        }
    }
    ret = wp_pass_init(pass, &p->device, p->negotiated.vk_format, p->sc.width, p->sc.height);
    if (ret < 0) {
        fprintf(stderr, "pass: %s\n", strerror(-ret));
        wp_pass_destroy(pass);
        wp_mesh_destroy(&p->device, mesh);
        wp_mesh_destroy(&p->device, plot_mesh);
        wp_mesh_destroy(&p->device, image_mesh);
        wp_mesh_destroy(&p->device, grid_mesh);
        wp_tex_destroy(&image_tex);
        wp_present_close(p);
        wp_session_close(s);
        goto fail_free;
    }
    ret = wp_lit_init(lit, &p->device, p->negotiated.vk_format);
    if (ret < 0) {
        wp_pass_destroy(pass);
        wp_mesh_destroy(&p->device, mesh);
        wp_mesh_destroy(&p->device, plot_mesh);
        wp_mesh_destroy(&p->device, image_mesh);
        wp_mesh_destroy(&p->device, grid_mesh);
        wp_tex_destroy(&image_tex);
        wp_present_close(p);
        wp_session_close(s);
        goto fail_free;
    }
    ret = wp_font_open_default(font, 28.0f);
    if (ret < 0) {
        fprintf(stderr, "font: %s\n", strerror(-ret));
        wp_lit_destroy(lit);
        wp_pass_destroy(pass);
        wp_mesh_destroy(&p->device, mesh);
        wp_mesh_destroy(&p->device, plot_mesh);
        wp_mesh_destroy(&p->device, image_mesh);
        wp_mesh_destroy(&p->device, grid_mesh);
        wp_tex_destroy(&image_tex);
        wp_present_close(p);
        wp_session_close(s);
        goto fail_free;
    }
    ret = wp_font_upload(font, &p->device);
    if (ret < 0) {
        fprintf(stderr, "font upload: %s\n", strerror(-ret));
        wp_font_destroy(font);
        wp_lit_destroy(lit);
        wp_pass_destroy(pass);
        wp_mesh_destroy(&p->device, mesh);
        wp_mesh_destroy(&p->device, plot_mesh);
        wp_mesh_destroy(&p->device, image_mesh);
        wp_mesh_destroy(&p->device, grid_mesh);
        wp_tex_destroy(&image_tex);
        wp_present_close(p);
        wp_session_close(s);
        goto fail_free;
    }
    ret = wp_text_init(text, &p->device, p->negotiated.vk_format);
    if (ret < 0) {
        wp_font_destroy(font);
        wp_lit_destroy(lit);
        wp_pass_destroy(pass);
        wp_mesh_destroy(&p->device, mesh);
        wp_mesh_destroy(&p->device, plot_mesh);
        wp_mesh_destroy(&p->device, image_mesh);
        wp_mesh_destroy(&p->device, grid_mesh);
        wp_tex_destroy(&image_tex);
        wp_present_close(p);
        wp_session_close(s);
        goto fail_free;
    }
    ret = wp_card_init(card, &p->device, p->negotiated.vk_format);
    if (ret < 0) {
        fprintf(stderr, "card: %s\n", strerror(-ret));
        wp_card_destroy(card);
        wp_text_destroy(text);
        wp_font_destroy(font);
        wp_lit_destroy(lit);
        wp_pass_destroy(pass);
        wp_mesh_destroy(&p->device, mesh);
        wp_mesh_destroy(&p->device, plot_mesh);
        wp_mesh_destroy(&p->device, image_mesh);
        wp_mesh_destroy(&p->device, grid_mesh);
        wp_tex_destroy(&image_tex);
        wp_present_close(p);
        wp_session_close(s);
        goto fail_free;
    }
    if (have_scene) {
        int32_t lw = s->width > 0 ? s->width : WP_WL_DEFAULT_WIDTH;
        int32_t lh = s->height > 0 ? s->height : WP_WL_DEFAULT_HEIGHT;
        const float view_top = 152.0f;
        float h = (float)lh - view_top;
        float mid = (float)lw * 0.5f;

        if (h < 1.0f)
            h = (float)lh;
        views[0].rect = (struct wp_rect){ 0, view_top, mid, h };
        views[1].rect = (struct wp_rect){ mid, view_top, (float)lw - mid, h };
        ret = wp_view_fit(&views[0], &scene);
        if (ret < 0)
            fprintf(stderr, "fit 3D: %s\n", strerror(-ret));
        ret = wp_view_fit(&views[1], &scene);
        if (ret < 0)
            fprintf(stderr, "fit plan: %s\n", strerror(-ret));
    }
    snprintf(line, sizeof(line), "wayplot %s", WP_VERSION_STRING);
    {
        float sc = p->session->scale > 0 ? (float)p->session->scale : 1.0f;
        ret = wp_text_layout(font, line, 24.0f * sc, 16.0f * sc, &label);
        if (ret < 0)
            fprintf(stderr, "text layout: %s\n", strerror(-ret));
    }

    int32_t laid_scale = p->session->scale > 0 ? p->session->scale : 1;
    uint32_t laid_w = p->sc.width, laid_h = p->sc.height;
    uint32_t dragging = 0;
    int orbiting = 0;
    int panning = 0;
    while (!s->closed) {
        struct wp_present_frame f;
        float model[16];
        int32_t lw = s->width > 0 ? s->width : WP_WL_DEFAULT_WIDTH;
        int32_t lh = s->height > 0 ? s->height : WP_WL_DEFAULT_HEIGHT;
        const float view_top = 152.0f; /* below the cards (56+88) + gap */
        struct wp_rect vrect[2];
        int vi;
        ret = wp_present_poll(p, 50ull * 1000ull * 1000ull);
        if (ret < 0) {
            fprintf(stderr, "poll: %s\n", strerror(-ret));
            break;
        }
        {
            float h = (float)lh - view_top;
            float mid = (float)lw * 0.5f;
            if (h < 1.0f)
                h = (float)lh;
            views[0].rect = (struct wp_rect){ 0, view_top, mid, h };
            views[1].rect = (struct wp_rect){ mid, view_top, (float)lw - mid, h };
            if (!wp_rect_ok(&views[0].rect) || !wp_rect_ok(&views[1].rect)) {
                views[0].rect = (struct wp_rect){ 0, 0, mid, (float)lh };
                views[1].rect = (struct wp_rect){ mid, 0, (float)lw - mid, (float)lh };
            }
            vrect[0] = views[0].rect;
            vrect[1] = views[1].rect;
        }
        wp_hit_clear(&hits);
        ret = wp_doc_fill_hits(&doc, &hits);
        if (ret < 0)
            fprintf(stderr, "hit: %s\n", strerror(-ret));
        ret = wp_input_handle(&input, s, &hits, vrect, 2);
        if (ret < 0)
            fprintf(stderr, "input: %s\n", strerror(-ret));
        if (input.drag_id)
            dragging = input.drag_id;
        if (dragging) {
            ret = wp_doc_apply_drag(&doc, dragging, input.drag_rect);
            if (ret < 0)
                fprintf(stderr, "drag: %s\n", strerror(-ret));
        }
        if (!input.drag_id)
            dragging = 0;
        vi = input.view_i;
        if (vi < 0 || vi > 1)
            vi = 0;
        if (input.orbiting)
            orbiting = 1;
        if (orbiting)
            wp_view_orbit(&views[vi], (float)input.orbit_dx, (float)input.orbit_dy);
        if (!input.orbiting)
            orbiting = 0;
        if (input.panning)
            panning = 1;
        if (panning)
            wp_view_pan(&views[vi], (float)input.pan_dx, (float)input.pan_dy);
        if (!input.panning)
            panning = 0;
        if (input.axis_v) {
            int di = (orbiting || panning) ? vi : input.hover_i;
            if (di >= 0 && di < 2)
                wp_view_dolly(&views[di], input.axis_v);
        }
        if (!wp_present_begin(p, &f))
            continue;
        if (f.scale != laid_scale || f.extent.width != laid_w || f.extent.height != laid_h) {
            float sc = (float)(f.scale > 0 ? f.scale : 1);
            if (f.extent.width != laid_w || f.extent.height != laid_h) {
                ret = wp_pass_resize(pass, f.extent.width, f.extent.height);
                if (ret < 0)
                    fprintf(stderr, "pass resize: %s\n", strerror(-ret));
            }
            if (f.scale != laid_scale) {
                wp_font_destroy(font);
                ret = wp_font_open_default(font, 28.0f * sc);
                if (ret == 0)
                    ret = wp_font_upload(font, &p->device);
                if (ret < 0)
                    fprintf(stderr, "font reload: %s\n", strerror(-ret));
            }
            wp_text_geom_free(&label);
            (void)wp_text_layout(font, line, 24.0f * sc, 16.0f * sc, &label);
            laid_scale = f.scale;
            laid_w = f.extent.width;
            laid_h = f.extent.height;
        }
        wp_mat4_identity(model);
        wp_draw_list_clear(&draws);
        {
            float sc = (float)(f.scale > 0 ? f.scale : 1);
            uint32_t ci;
            ret = wp_doc_push_cards(&doc, &draws, sc);
            if (ret < 0)
                fprintf(stderr, "draw cards: %s\n", strerror(-ret));
            for (ci = 0; ci < 2 && ci < wp_doc_ncards(&doc); ci++) {
                const struct wp_doc_card *c = wp_doc_card(&doc, ci);
                wp_text_geom_free(&cap[ci]);
                if (!c || !c->caption[0])
                    continue;
                ret = wp_text_layout(font, c->caption, (c->rect.x + 8.0f) * sc,
                                     (c->rect.y + 6.0f) * sc, &cap[ci]);
                if (ret < 0 || cap[ci].nv == 0)
                    continue;
                ret = wp_draw_list_push_text(&draws, font, &cap[ci], white);
                if (ret < 0)
                    fprintf(stderr, "draw caption: %s\n", strerror(-ret));
            }
        }
        ret = wp_draw_list_push_text(&draws, font, &label, white);
        if (ret < 0)
            fprintf(stderr, "draw text: %s\n", strerror(-ret));
        {
            uint32_t i, mi;
            wp_pass_opaque_begin(pass, f.cmd, f.view, f.extent.width, f.extent.height, f.slot);
            if (lit && wp_doc_nmesh(&doc)) {
                wp_lit_reset(lit, f.slot);
                for (i = 0; i < 2; i++) {
                    int32_t vx, vy;
                    uint32_t vw, vh;
                    if (wp_view_bind(&views[i], f.cmd, f.scale, f.extent.width, f.extent.height) < 0)
                        continue;
                    if (wp_view_pixels(&views[i], f.scale, f.extent.width, f.extent.height, &vx, &vy,
                                       &vw, &vh) < 0)
                        continue;
                    for (mi = 0; mi < wp_doc_nmesh(&doc); mi++) {
                        struct wp_mesh *m = wp_doc_mesh(&doc, mi);
                        if (!m || !m->index_count || !wp_view_mesh_on(&views[i], mi))
                            continue;
                        wp_lit_draw_tex(lit, f.cmd, vw, vh, f.slot, m, &views[i].cam, model,
                                        wp_doc_albedo(&doc, mi));
                    }
                }
            }
            wp_pass_opaque_end(f.cmd);
            wp_pass_overlay_begin(pass, f.cmd, f.view, f.extent.width, f.extent.height);
            wp_draw_list_record_overlay(&draws, text, card, f.cmd, f.extent.width, f.extent.height,
                                        f.slot);
            wp_pass_overlay_end(f.cmd);
        }
        ret = wp_present_end(p, &f);
        if (ret < 0) {
            fprintf(stderr, "present_end: %s\n", strerror(-ret));
            break;
        }
    }

    wp_text_geom_free(&label);
    wp_text_geom_free(&cap[0]);
    wp_text_geom_free(&cap[1]);
    wp_draw_list_destroy(&draws);
    wp_doc_destroy(&doc);
    wp_text_destroy(text);
    wp_font_destroy(font);
    wp_card_destroy(card);
    wp_lit_destroy(lit);
    wp_pass_destroy(pass);
    wp_mesh_destroy(&p->device, mesh);
    wp_mesh_destroy(&p->device, plot_mesh);
    wp_mesh_destroy(&p->device, image_mesh);
    wp_mesh_destroy(&p->device, grid_mesh);
    wp_tex_destroy(&image_tex);
    wp_image_free(&image);
    wp_plot_free(&plot);
    wp_present_close(p);
    wp_session_close(s);
    free(text);
    free(font);
    free(card);
    free(lit);
    free(mesh);
    free(plot_mesh);
    free(image_mesh);
    free(grid_mesh);
    free(pass);
    free(p);
    free(s);
    return ret < 0 ? 1 : 0;

fail_free:
    wp_text_geom_free(&label);
    wp_text_geom_free(&cap[0]);
    wp_text_geom_free(&cap[1]);
    wp_draw_list_destroy(&draws);
    wp_doc_destroy(&doc);
    wp_plot_free(&plot);
    wp_image_free(&image);
    free(text);
    free(font);
    free(card);
    free(lit);
    free(mesh);
    free(plot_mesh);
    free(image_mesh);
    free(grid_mesh);
    free(pass);
    free(p);
    free(s);
    return 1;
}

int main(int argc, char **argv)
{
    int i;
    const char *mesh_path = NULL;
    const char *dem_path = NULL;
    const char *plot_path = NULL;
    const char *image_path = NULL;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("%s\n", WP_VERSION_STRING);
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--mesh") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--mesh needs a path\n");
                print_help(argv[0]);
                return 2;
            }
            mesh_path = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--dem") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--dem needs a path\n");
                print_help(argv[0]);
                return 2;
            }
            dem_path = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--plot") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--plot needs a path\n");
                print_help(argv[0]);
                return 2;
            }
            plot_path = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--image") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--image needs a path\n");
                print_help(argv[0]);
                return 2;
            }
            image_path = argv[++i];
            continue;
        }
        if (argv[i][0] != '-') {
            if (mesh_path) {
                fprintf(stderr, "unexpected argument: %s\n", argv[i]);
                print_help(argv[0]);
                return 2;
            }
            mesh_path = argv[i];
            continue;
        }
        fprintf(stderr, "unknown option: %s\n", argv[i]);
        print_help(argv[0]);
        return 2;
    }

    wp_debug("wayplot %s (%s) pid %d\n", WP_VERSION_STRING, WP_BUILD_TYPE, (int)getpid());
    return run(mesh_path, dem_path, plot_path, image_path);
}
