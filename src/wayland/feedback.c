#define _GNU_SOURCE
#include "wayland/feedback.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

void wp_feedback_init(struct wp_feedback *f)
{
    if (!f)
        return;
    memset(f, 0, sizeof(*f));
    f->current = -1;
}

static void free_tranches(struct wp_feedback *f)
{
    uint32_t i;

    for (i = 0; i < f->ntranches; i++)
        free(f->tranches[i].pairs);
    free(f->tranches);
    f->tranches = NULL;
    f->ntranches = 0;
    f->cap = 0;
    f->current = -1;
}

void wp_feedback_reset(struct wp_feedback *f)
{
    if (!f)
        return;
    free(f->table);
    f->table = NULL;
    f->table_count = 0;
    free_tranches(f);
    f->main_device = 0;
    f->done = false;
}

void wp_feedback_free(struct wp_feedback *f)
{
    wp_feedback_reset(f);
    if (f)
        memset(f, 0, sizeof(*f));
}

int wp_feedback_set_table(struct wp_feedback *f, int fd, uint32_t bytes)
{
    void *map;
    struct wp_format_table_entry *copy;

    if (!f || fd < 0)
        return -EBADF;
    if (bytes == 0 || (bytes % sizeof(struct wp_format_table_entry)) != 0) {
        close(fd);
        return -EBADMSG;
    }

    map = mmap(NULL, bytes, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
        return -errno;

    copy = malloc(bytes);
    if (!copy) {
        munmap(map, bytes);
        return -ENOMEM;
    }
    memcpy(copy, map, bytes);
    munmap(map, bytes);

    free(f->table);
    f->table = copy;
    f->table_count = bytes / (uint32_t)sizeof(struct wp_format_table_entry);
    /* A new table starts a new feedback round. */
    free_tranches(f);
    f->done = false;
    return 0;
}

void wp_feedback_set_main_device(struct wp_feedback *f, dev_t dev)
{
    if (f)
        f->main_device = dev;
}

struct wp_tranche *wp_feedback_cur(struct wp_feedback *f)
{
    struct wp_tranche *t;

    if (!f)
        return NULL;
    if (f->current >= 0 && (uint32_t)f->current < f->ntranches)
        return &f->tranches[f->current];
    if (f->ntranches == f->cap) {
        uint32_t cap = f->cap ? f->cap * 2 : 4;
        t = realloc(f->tranches, cap * sizeof(*t));
        if (!t)
            return NULL;
        f->tranches = t;
        f->cap = cap;
    }
    t = &f->tranches[f->ntranches];
    memset(t, 0, sizeof(*t));
    f->current = (int)f->ntranches;
    f->ntranches++;
    return t;
}

int wp_feedback_add_indices(struct wp_feedback *f, const uint16_t *idx, uint32_t n)
{
    struct wp_tranche *tr;
    struct wp_format_pair *p;
    uint32_t i, want;

    if (!f || !idx)
        return -EINVAL;
    if (!f->table || f->table_count == 0)
        return -EPROTO;
    tr = wp_feedback_cur(f);
    if (!tr)
        return -ENOMEM;
    want = tr->pair_count + n;
    p = realloc(tr->pairs, want * sizeof(*p));
    if (!p)
        return -ENOMEM;
    tr->pairs = p;
    for (i = 0; i < n; i++) {
        uint16_t k = idx[i];
        if (k >= f->table_count)
            return -ERANGE;
        tr->pairs[tr->pair_count++] = (struct wp_format_pair){
            .format = f->table[k].format,
            .modifier = f->table[k].modifier,
        };
    }
    return 0;
}

void wp_feedback_tranche_done(struct wp_feedback *f)
{
    if (f)
        f->current = -1;
}

void wp_feedback_print(const struct wp_feedback *f, FILE *out)
{
    uint32_t i;

    if (!f || !out)
        return;
    fprintf(out, "main_device %lu  format_table %u entries  tranches %u  %s\n",
            (unsigned long)f->main_device, f->table_count, f->ntranches,
            f->done ? "done" : "pending");
    for (i = 0; i < f->ntranches; i++) {
        fprintf(out, "  tranche[%u] device %lu flags 0x%x pairs %u%s\n",
                i, (unsigned long)f->tranches[i].target_device,
                f->tranches[i].flags, f->tranches[i].pair_count,
                (f->tranches[i].flags & WP_TRANCHE_SCANOUT) ? " scanout" : "");
    }
}
