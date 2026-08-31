#define _GNU_SOURCE
#include "wayland/conn.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

static void put_hdr(uint32_t *w, uint32_t obj, uint16_t op, uint16_t size)
{
    w[0] = obj;
    w[1] = ((uint32_t)size << 16) | op;
}

static int wait_msg(struct wp_wl_conn *rx, struct wp_wl_msg *m)
{
    int spins = 0;

    while (!wp_wl_peek(rx, m)) {
        int ret = wp_wl_pump(rx, 1);
        if (ret < 0)
            return ret;
        if (++spins > 200000)
            return -ETIMEDOUT;
    }
    return 0;
}

int main(void)
{
    struct wp_wl_conn a, b;
    struct wp_wl_msg m;
    uint32_t small[3];
    uint32_t *big;
    const uint32_t big_size = 65532;
    int ret;

    printf("wayplot wayland-conn  pbuf %u x %u\n", WP_WL_PBUF_COUNT, WP_WL_PBUF_SIZE);

    ret = wp_wl_conn_pair(&a, &b);
    expect(ret == 0, "conn pair (two 7.2 rings, unix stream)");
    if (ret < 0) {
        printf("      pair %s\n", strerror(-ret));
        return 1;
    }

    put_hdr(small, 1, 0, 12);
    small[2] = 0x11223344;
    ret = wp_wl_send(&a, small, sizeof(small), NULL, 0);
    expect(ret >= 0, "send 12-byte display.sync-shaped message");
    ret = wait_msg(&b, &m);
    expect(ret == 0, "recv that message");
    expect(m.obj == 1 && m.opcode == 0 && m.size == 12, "obj/opcode/size");
    expect(m.body_len == 4 && m.body[0] == 0x44, "body (le uint32)");
    wp_wl_consume(&b);

    /* 64 KiB wire message: several 8 KiB recvs, assembled in conn */
    big = calloc(1, big_size);
    expect(big != NULL, "alloc 64KiB message");
    if (big) {
        put_hdr(big, 2, 7, (uint16_t)big_size);
        memset((uint8_t *)big + 8, 0xab, big_size - 8);
        ((uint8_t *)big)[big_size - 1] = 0xcd;
        ret = wp_wl_send(&a, big, big_size, NULL, 0);
        expect(ret >= 0, "send 65532-byte message (max-ish wayland)");
        ret = wait_msg(&b, &m);
        expect(ret == 0 && m.size == big_size && m.obj == 2 && m.opcode == 7,
               "reassembled 65532-byte message");
        if (ret == 0)
            expect(m.body[m.body_len - 1] == 0xcd, "last payload byte intact");
        wp_wl_consume(&b);
        free(big);
    }

    /* 20 ms wait with no peer traffic must return, not hang in enter(). */
    {
        uint64_t t0, t1;
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        t0 = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        ret = wp_wl_pump_wait(&b, 1, 20ull * 1000ull * 1000ull);
        clock_gettime(CLOCK_MONOTONIC, &ts);
        t1 = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        expect(ret == -ETIME, "pump_wait 20ms with no data -> -ETIME");
        expect(t1 - t0 < 500ull * 1000ull * 1000ull, "pump_wait returned in well under 500ms");
        printf("      pump_wait idle %llu us\n", (unsigned long long)((t1 - t0) / 1000ull));
    }

    wp_wl_conn_destroy(&a);
    wp_wl_conn_destroy(&b);
    printf("%s  %d failure(s)\n", g_fail ? "RESULT FAIL" : "RESULT PASS", g_fail);
    return g_fail ? 1 : 0;
}
