#ifndef KHOROS_WAYLAND_XDG_H
#define KHOROS_WAYLAND_XDG_H

#if __STDC_VERSION__ < 202311L
#error "Khoros requires pure ISO C23 (compile with -std=c23)"
#endif

#ifdef _STDBOOL_H
#error "<stdbool.h> is strictly banned. In C23, bool, true, and false are native language keywords."
#endif

#ifdef _STDALIGN_H
#error "<stdalign.h> is strictly banned. In C23, alignas and alignof are native language keywords."
#endif

#include <stdint.h>
#include "khoros/core/attributes.h"
#include "khoros/wayland/client.h"

/*
 * Minimal XDG shell state machine over the raw io_uring Wayland socket.
 * No libwayland. Opcodes verified against /usr/share/wayland/wayland.xml and
 * /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml (request and
 * event opcodes count from 0 independently per interface).
 *
 * Lifecycle enforced here:
 *   bind (registry) -> create_surface -> get_xdg_surface -> get_toplevel
 *   -> set_title/app_id -> empty commit -> configure -> ack_configure.
 * Buffer attach before the first ack is a fatal compositor error
 * (unconfigured_buffer); check khr_xdg_can_attach() before any attach.
 * Ping is answered with pong immediately inside khr_xdg_consume().
 */

/* wl_compositor / wl_surface (wayland.xml). Registry bind lives in wire.h. */
constexpr uint16_t KHR_WL_COMPOSITOR_CREATE_SURFACE = 0;
constexpr uint16_t KHR_WL_SURFACE_DESTROY        = 0;
constexpr uint16_t KHR_WL_SURFACE_ATTACH         = 1;
constexpr uint16_t KHR_WL_SURFACE_DAMAGE         = 2;
constexpr uint16_t KHR_WL_SURFACE_COMMIT         = 6;

/* xdg_wm_base (xdg-shell.xml): get_xdg_surface=2, pong=3, event ping=0 */
constexpr uint16_t KHR_XDG_WM_BASE_CREATE_POSITIONER = 1;
constexpr uint16_t KHR_XDG_WM_BASE_GET_XDG_SURFACE = 2;
constexpr uint16_t KHR_XDG_WM_BASE_PONG           = 3;
constexpr uint16_t KHR_XDG_WM_BASE_EVENT_PING     = 0;

/* xdg_surface: get_toplevel=1, get_popup=2, ack_configure=4, event configure=0 */
constexpr uint16_t KHR_XDG_SURFACE_DESTROY        = 0;
constexpr uint16_t KHR_XDG_SURFACE_GET_TOPLEVEL   = 1;
constexpr uint16_t KHR_XDG_SURFACE_GET_POPUP      = 2;
constexpr uint16_t KHR_XDG_SURFACE_ACK_CONFIGURE  = 4;
constexpr uint16_t KHR_XDG_SURFACE_EVENT_CONFIGURE = 0;

/* xdg_positioner / xdg_popup (context menus that may hang outside the parent). */
constexpr uint16_t KHR_XDG_POS_DESTROY            = 0;
constexpr uint16_t KHR_XDG_POS_SET_SIZE           = 1;
constexpr uint16_t KHR_XDG_POS_SET_ANCHOR_RECT    = 2;
constexpr uint16_t KHR_XDG_POS_SET_ANCHOR         = 3;
constexpr uint16_t KHR_XDG_POS_SET_GRAVITY        = 4;
constexpr uint16_t KHR_XDG_POS_SET_CONSTRAINT     = 5;
constexpr uint16_t KHR_XDG_POPUP_DESTROY          = 0;
constexpr uint16_t KHR_XDG_POPUP_GRAB             = 1;
constexpr uint16_t KHR_XDG_POPUP_EVENT_CONFIGURE  = 0;
constexpr uint16_t KHR_XDG_POPUP_EVENT_DONE       = 1;
/* xdg_positioner.anchor / gravity are sequential enums, not edge bitfields. */
constexpr uint32_t KHR_XDG_ANCHOR_NONE            = 0;
constexpr uint32_t KHR_XDG_ANCHOR_TOP             = 1;
constexpr uint32_t KHR_XDG_ANCHOR_BOTTOM          = 2;
constexpr uint32_t KHR_XDG_ANCHOR_LEFT            = 3;
constexpr uint32_t KHR_XDG_ANCHOR_RIGHT           = 4;
constexpr uint32_t KHR_XDG_ANCHOR_TOP_LEFT        = 5;
constexpr uint32_t KHR_XDG_ANCHOR_BOTTOM_LEFT     = 6;
constexpr uint32_t KHR_XDG_ANCHOR_TOP_RIGHT       = 7;
constexpr uint32_t KHR_XDG_ANCHOR_BOTTOM_RIGHT    = 8;
constexpr uint32_t KHR_XDG_GRAVITY_NONE           = 0;
constexpr uint32_t KHR_XDG_GRAVITY_TOP            = 1;
constexpr uint32_t KHR_XDG_GRAVITY_BOTTOM         = 2;
constexpr uint32_t KHR_XDG_GRAVITY_LEFT           = 3;
constexpr uint32_t KHR_XDG_GRAVITY_RIGHT          = 4;
constexpr uint32_t KHR_XDG_GRAVITY_TOP_LEFT       = 5;
constexpr uint32_t KHR_XDG_GRAVITY_BOTTOM_LEFT    = 6;
constexpr uint32_t KHR_XDG_GRAVITY_TOP_RIGHT      = 7;
constexpr uint32_t KHR_XDG_GRAVITY_BOTTOM_RIGHT   = 8;
constexpr uint32_t KHR_XDG_CONSTRAINT_SLIDE_X     = 1;
constexpr uint32_t KHR_XDG_CONSTRAINT_SLIDE_Y     = 2;
constexpr uint32_t KHR_XDG_CONSTRAINT_FLIP_X      = 4;
constexpr uint32_t KHR_XDG_CONSTRAINT_FLIP_Y      = 8;

/* xdg_surface extra: set_window_geometry=3 */
constexpr uint16_t KHR_XDG_SURFACE_SET_WINDOW_GEOMETRY = 3;

/* xdg_toplevel: set_title=2, set_app_id=3, events configure=0, close=1 */
constexpr uint16_t KHR_XDG_TOPLEVEL_SET_TITLE        = 2;
constexpr uint16_t KHR_XDG_TOPLEVEL_SET_APP_ID       = 3;
constexpr uint16_t KHR_XDG_TOPLEVEL_MOVE             = 5;
constexpr uint16_t KHR_XDG_TOPLEVEL_RESIZE           = 6;
constexpr uint16_t KHR_XDG_TOPLEVEL_SET_MAX_SIZE     = 7;
constexpr uint16_t KHR_XDG_TOPLEVEL_SET_MIN_SIZE     = 8;
constexpr uint16_t KHR_XDG_TOPLEVEL_SET_MAXIMIZED    = 9;
constexpr uint16_t KHR_XDG_TOPLEVEL_UNSET_MAXIMIZED  = 10;
constexpr uint16_t KHR_XDG_TOPLEVEL_SET_FULLSCREEN   = 11;
constexpr uint16_t KHR_XDG_TOPLEVEL_UNSET_FULLSCREEN = 12;
constexpr uint16_t KHR_XDG_TOPLEVEL_EVENT_CONFIGURE  = 0;
constexpr uint16_t KHR_XDG_TOPLEVEL_EVENT_CLOSE      = 1;

constexpr uint32_t KHR_XDG_RESIZE_NONE          = 0;
constexpr uint32_t KHR_XDG_RESIZE_TOP           = 1;
constexpr uint32_t KHR_XDG_RESIZE_BOTTOM        = 2;
constexpr uint32_t KHR_XDG_RESIZE_LEFT          = 4;
constexpr uint32_t KHR_XDG_RESIZE_TOP_LEFT      = 5;
constexpr uint32_t KHR_XDG_RESIZE_BOTTOM_LEFT   = 6;
constexpr uint32_t KHR_XDG_RESIZE_RIGHT         = 8;
constexpr uint32_t KHR_XDG_RESIZE_TOP_RIGHT     = 9;
constexpr uint32_t KHR_XDG_RESIZE_BOTTOM_RIGHT  = 10;

constexpr uint32_t KHR_XDG_STATE_MAXIMIZED  = 1;
constexpr uint32_t KHR_XDG_STATE_FULLSCREEN = 2;
constexpr uint32_t KHR_XDG_STATE_RESIZING   = 3;
constexpr uint32_t KHR_XDG_STATE_ACTIVATED  = 4;

/* zxdg_decoration_manager_v1 / zxdg_toplevel_decoration_v1 */
constexpr uint16_t KHR_XDG_DECO_MGR_GET_TOPLEVEL = 1;
constexpr uint16_t KHR_XDG_DECO_SET_MODE         = 1;
constexpr uint32_t KHR_XDG_DECO_CLIENT_SIDE      = 1;

typedef enum {
    KHR_XDG_UNBOUND = 0,
    KHR_XDG_BOUND,
    KHR_XDG_TOPLEVEL,
    KHR_XDG_CONFIGURED,
} khr_xdg_state_t;

typedef struct {
    uint32_t        compositor_id;   /* bound wl_compositor object id */
    uint32_t        wm_base_id;      /* bound xdg_wm_base object id */
    uint32_t        surface_id;      /* wl_surface */
    uint32_t        xdg_surface_id;  /* xdg_surface */
    uint32_t        xdg_toplevel_id; /* xdg_toplevel */
    uint32_t        last_ping_serial;
    uint32_t        last_ack_serial;
    uint32_t        pending_ack_serial; /* 0 = none; ack with the matching commit */
    uint32_t        ping_count;
    uint32_t        configure_count;
    int32_t         width;           /* latest toplevel configure (0 = unset) */
    int32_t         height;
    uint32_t        states;          /* bitmask of KHR_XDG_STATE_* */
    uint32_t        deco_mgr_id;
    uint32_t        deco_id;
    bool            configured;      /* first xdg_surface.configure seen */
    bool            closed;          /* close event received */
    bool            maximized;
    bool            fullscreen;
    uint32_t        size_seq;        /* bumped when width/height/states change */
    khr_xdg_state_t state;

    /* Right-click cart: separate xdg_popup surface, may hang outside parent. */
    uint32_t        popup_surface_id;
    uint32_t        popup_xdg_id;
    uint32_t        popup_id;
    uint32_t        popup_ack_serial;
    int32_t         popup_x;
    int32_t         popup_y;
    int32_t         popup_w;
    int32_t         popup_h;
    bool            popup_live;
    bool            popup_configured;
    bool            popup_mapped;
    bool            popup_done;
} khr_xdg_shell_t;

void khr_xdg_init(khr_xdg_shell_t* shell);

/* Bind wl_compositor + xdg_wm_base from registry globals. One batched send. */
[[nodiscard]]
bool khr_xdg_bind(khr_wl_client_t* client, khr_xdg_shell_t* shell);

/* create_surface -> get_xdg_surface -> get_toplevel -> title/app_id ->
 * empty commit (triggers the initial configure). No buffer attached. */
[[nodiscard]]
bool khr_xdg_create_toplevel(khr_wl_client_t* client, khr_xdg_shell_t* shell,
                             const char* title, const char* app_id);

/*
 * Steady-state event consumer over a wire byte stream (same pattern as the
 * registry parser): ping -> immediate pong, xdg_surface.configure -> store
 * pending serial (ack is khr_xdg_ack_pending, with the matching attach),
 * toplevel configure -> store w/h, close -> mark closed.
 * Unknown object IDs are ignored. Returns XDG messages handled.
 */
uint32_t khr_xdg_consume(khr_wl_client_t* client, khr_xdg_shell_t* shell,
                         const uint8_t* data, size_t len);

/* Send ack_configure for the latest pending serial. Call immediately
 * before the attach+commit that matches that configure. */
[[nodiscard]]
bool khr_xdg_ack_pending(khr_wl_client_t* client, khr_xdg_shell_t* shell);

/* Attach gate: true after the first configure (ack before the buffer). */
[[nodiscard]]
static inline bool khr_xdg_can_attach(const khr_xdg_shell_t* shell) {
    return shell != nullptr && shell->configured && !shell->closed;
}

/* Client-side decorations when zxdg_decoration_manager_v1 is advertised. */
[[nodiscard]]
bool khr_xdg_request_csd(khr_wl_client_t* client, khr_xdg_shell_t* shell);

[[nodiscard]]
bool khr_xdg_set_min_size(khr_wl_client_t* client, const khr_xdg_shell_t* shell,
                          int32_t w, int32_t h);

[[nodiscard]]
bool khr_xdg_set_window_geometry(khr_wl_client_t* client,
                                 const khr_xdg_shell_t* shell,
                                 int32_t x, int32_t y, int32_t w, int32_t h);

[[nodiscard]]
bool khr_xdg_move(khr_wl_client_t* client, const khr_xdg_shell_t* shell,
                  uint32_t seat_id, uint32_t serial);

[[nodiscard]]
bool khr_xdg_resize(khr_wl_client_t* client, const khr_xdg_shell_t* shell,
                    uint32_t seat_id, uint32_t serial, uint32_t edges);

[[nodiscard]]
bool khr_xdg_set_maximized(khr_wl_client_t* client, const khr_xdg_shell_t* shell,
                           bool on);

[[nodiscard]]
bool khr_xdg_set_fullscreen(khr_wl_client_t* client, const khr_xdg_shell_t* shell,
                            bool on);

/* Context-menu popup: new surface + positioner + grab. Hangs outside the
 * parent window; compositor may flip/slide to stay on the output. */
[[nodiscard]]
bool khr_xdg_popup_open(khr_wl_client_t* client, khr_xdg_shell_t* shell,
                        uint32_t seat_id, uint32_t serial,
                        int32_t anchor_x, int32_t anchor_y,
                        int32_t w, int32_t h);

[[nodiscard]]
bool khr_xdg_popup_ack(khr_wl_client_t* client, khr_xdg_shell_t* shell);

void khr_xdg_popup_destroy(khr_wl_client_t* client, khr_xdg_shell_t* shell);

#endif /* KHOROS_WAYLAND_XDG_H */
