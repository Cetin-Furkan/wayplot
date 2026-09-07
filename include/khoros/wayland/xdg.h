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
constexpr uint16_t KHR_XDG_WM_BASE_GET_XDG_SURFACE = 2;
constexpr uint16_t KHR_XDG_WM_BASE_PONG           = 3;
constexpr uint16_t KHR_XDG_WM_BASE_EVENT_PING     = 0;

/* xdg_surface: get_toplevel=1, ack_configure=4, event configure=0 */
constexpr uint16_t KHR_XDG_SURFACE_GET_TOPLEVEL   = 1;
constexpr uint16_t KHR_XDG_SURFACE_ACK_CONFIGURE  = 4;
constexpr uint16_t KHR_XDG_SURFACE_EVENT_CONFIGURE = 0;

/* xdg_toplevel: set_title=2, set_app_id=3, events configure=0, close=1 */
constexpr uint16_t KHR_XDG_TOPLEVEL_SET_TITLE     = 2;
constexpr uint16_t KHR_XDG_TOPLEVEL_SET_APP_ID    = 3;
constexpr uint16_t KHR_XDG_TOPLEVEL_EVENT_CONFIGURE = 0;
constexpr uint16_t KHR_XDG_TOPLEVEL_EVENT_CLOSE   = 1;

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
    uint32_t        ping_count;
    uint32_t        configure_count;
    int32_t         width;           /* latest toplevel configure (0 = unset) */
    int32_t         height;
    bool            configured;      /* first ack_configure sent */
    bool            closed;          /* close event received */
    khr_xdg_state_t state;
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
 * registry parser): ping -> immediate pong, xdg_surface.configure -> immediate
 * ack_configure, toplevel configure -> store w/h, close -> mark closed.
 * Unknown object IDs are ignored. Returns XDG messages handled.
 */
uint32_t khr_xdg_consume(khr_wl_client_t* client, khr_xdg_shell_t* shell,
                         const uint8_t* data, size_t len);

/* Attach gate: true only after the first configure was acknowledged. */
[[nodiscard]]
static inline bool khr_xdg_can_attach(const khr_xdg_shell_t* shell) {
    return shell != nullptr && shell->configured && !shell->closed;
}

#endif /* KHOROS_WAYLAND_XDG_H */
