#ifndef WAYLAND_SESSION_H
#define WAYLAND_SESSION_H

#include "wayland/conn.h"
#include "wayland/feedback.h"
#include "wayland/proto.h"
#include "wayland/registry.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define WP_WL_DEFAULT_WIDTH  1280
#define WP_WL_DEFAULT_HEIGHT 720
#define WP_WL_MAX_OUTPUTS    8
#define WP_WL_RESIZE_MARGIN  12
#define WP_WL_MOVE_BAND      36
#define WP_POINTER_LEFT      1u
#define WP_POINTER_MIDDLE    2u
#define WP_POINTER_RIGHT     4u

enum {
    WP_CURSOR_DEFAULT = 1,
    WP_CURSOR_E_RESIZE = 18,
    WP_CURSOR_N_RESIZE = 19,
    WP_CURSOR_NE_RESIZE = 20,
    WP_CURSOR_NW_RESIZE = 21,
    WP_CURSOR_S_RESIZE = 22,
    WP_CURSOR_SE_RESIZE = 23,
    WP_CURSOR_SW_RESIZE = 24,
    WP_CURSOR_W_RESIZE = 25,
};

enum {
    WP_RESIZE_NONE = 0,
    WP_RESIZE_TOP = 1,
    WP_RESIZE_BOTTOM = 2,
    WP_RESIZE_LEFT = 4,
    WP_RESIZE_TOP_LEFT = 5,
    WP_RESIZE_BOTTOM_LEFT = 6,
    WP_RESIZE_RIGHT = 8,
    WP_RESIZE_TOP_RIGHT = 9,
    WP_RESIZE_BOTTOM_RIGHT = 10,
};

enum {
    WP_TOPLEVEL_MAXIMIZED = 1u << 0,
    WP_TOPLEVEL_FULLSCREEN = 1u << 1,
    WP_TOPLEVEL_RESIZING = 1u << 2,
    WP_TOPLEVEL_ACTIVATED = 1u << 3,
    WP_TOPLEVEL_TILED = 1u << 4,
};

struct wp_session {
    struct wp_wl_conn conn;
    struct wp_proto proto;
    struct wp_registry reg;
    struct wp_feedback fb;
    uint32_t compositor;
    uint32_t xdg_wm;
    uint32_t dmabuf;
    uint32_t syncobj_mgr;
    uint32_t surface;
    uint32_t xdg_surface;
    uint32_t xdg_toplevel;
    uint32_t feedback;
    uint32_t syncobj_surface;
    uint32_t acquire_timeline;
    uint32_t configure_serial;
    uint32_t frame_cb;
    int32_t width, height; /* logical (xdg) */
    uint32_t buf_w, buf_h; /* logical × scale */
    int32_t scale;
    int32_t preferred_scale;
    uint32_t toplevel_states;
    uint32_t outputs[WP_WL_MAX_OUTPUTS];
    int32_t output_scale[WP_WL_MAX_OUTPUTS];
    uint8_t output_on[WP_WL_MAX_OUTPUTS];
    uint32_t noutputs;
    uint32_t seat;
    uint32_t pointer;
    uint32_t pointer_serial;
    uint32_t pointer_enter_serial;
    int32_t pointer_x, pointer_y;
    bool pointer_inside;
    uint32_t pointer_buttons;  /* WP_POINTER_LEFT / MIDDLE / RIGHT */
    uint32_t pointer_pressed;  /* edges this pump; host must read before next pump */
    uint32_t pointer_released;
    int32_t pointer_axis_v; /* wl_fixed, accumulated this pump; + is down */
    int32_t pointer_axis_h;
    uint32_t cursor_shape_mgr;
    uint32_t cursor_shape_dev;
    uint32_t cursor_shape;
    bool configured;
    bool closed;
    bool size_dirty;
    bool configure_dirty;
    bool frame_done;
};

struct wp_wl_export {
    int dma_fd;
    uint32_t plane_count;
    uint32_t offset[4];
    uint32_t stride[4];
    uint64_t modifier;
    uint32_t drm_format;
    int32_t width;
    int32_t height;
};

[[nodiscard]] int wp_session_open(struct wp_session *s);
[[nodiscard]] int wp_session_setup_surface(struct wp_session *s);
[[nodiscard]] int wp_session_dispatch(struct wp_session *s, const struct wp_wl_msg *m);
[[nodiscard]] int wp_session_pump(struct wp_session *s, uint64_t timeout_ns);
[[nodiscard]] int wp_session_setup_explicit_sync(struct wp_session *s, int acquire_fd);
[[nodiscard]] int wp_session_import_timeline(struct wp_session *s, int fd, uint32_t *out_id);
[[nodiscard]] int wp_session_create_wl_buffer(struct wp_session *s, const struct wp_wl_export *e,
                                              uint32_t *out_id);
[[nodiscard]] int wp_session_destroy_wl_buffer(struct wp_session *s, uint32_t id);
[[nodiscard]] int wp_session_commit_buffer(struct wp_session *s, uint32_t buffer,
                                           uint32_t acq_timeline, uint64_t acq_pt,
                                           uint32_t rel_timeline, uint64_t rel_pt);
[[nodiscard]] int wp_session_request_frame(struct wp_session *s);
[[nodiscard]] int wp_session_ack(struct wp_session *s);
void wp_session_buffer_size(const struct wp_session *s, uint32_t *w, uint32_t *h);
[[nodiscard]] int wp_session_toplevel_move(struct wp_session *s);
[[nodiscard]] int wp_session_toplevel_resize(struct wp_session *s, uint32_t edges);
[[nodiscard]] int wp_session_toplevel_set_maximized(struct wp_session *s, bool on);
[[nodiscard]] int wp_session_toplevel_set_fullscreen(struct wp_session *s, bool on);
uint32_t wp_xdg_states_mask(const uint32_t *states, uint32_t nbytes);
uint32_t wp_pointer_edge_mask(int32_t x, int32_t y, int32_t w, int32_t h);
uint32_t wp_pointer_cursor_shape(uint32_t edges);
[[nodiscard]] int wp_session_set_cursor(struct wp_session *s, uint32_t shape);
void wp_session_close(struct wp_session *s);
void wp_session_print(const struct wp_session *s, FILE *out);

#endif /* WAYLAND_SESSION_H */
