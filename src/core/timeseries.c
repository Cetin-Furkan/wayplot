#include "khoros/core/timeseries.h"
#include "engine.h"
#include <string.h>
#include <unistd.h>
#include <math.h>

bool khr_timeseries_open(khr_timeseries_t* ts,
                         const char* filepath,
                         float dt,
                         const float gravity[3],
                         const char* host_str) {
    if (ts == nullptr || filepath == nullptr) {
        return false;
    }
    memset(ts, 0, sizeof(*ts));
    ts->file = fopen(filepath, "w");
    if (ts->file == nullptr) {
        return false;
    }
    ts->dt = dt;
    float g = (gravity != nullptr) ? sqrtf(gravity[0]*gravity[0] + gravity[1]*gravity[1] + gravity[2]*gravity[2])
                                   : KHR_PHYSICS_GRAVITY_M_S2;
    ts->gravity = g;

    char hostname_buf[64] = "unknown";
    if (host_str != nullptr && host_str[0] != '\0') {
        strncpy(hostname_buf, host_str, sizeof(hostname_buf) - 1);
    } else {
        (void)gethostname(hostname_buf, sizeof(hostname_buf) - 1);
    }

    /* Contract B Header Line */
    fprintf(ts->file, "# Khoros Time Series v%u.%u.%u | __STDC_VERSION__=%ldL | dt=%.6f | g=%.5f | host=%s\n",
            ENGINE_VERSION_MAJOR, ENGINE_VERSION_MINOR, ENGINE_VERSION_PATCH,
            __STDC_VERSION__, (double)dt, (double)g, hostname_buf);

    /* CSV Column Names */
    fprintf(ts->file, "tick,t,body_id,x,y,z,qx,qy,qz,qw,vx,vy,vz,wx,wy,wz,E,ncontacts\n");
    fflush(ts->file);
    return true;
}

void khr_timeseries_write_tick(khr_timeseries_t* ts,
                               uint64_t tick,
                               float t,
                               const khr_physics_world_t* world) {
    if (ts == nullptr || ts->file == nullptr || world == nullptr) {
        return;
    }

    for (uint32_t b = 0; b < world->body_count; b++) {
        const khr_rigid_body_t* body = &world->bodies[b];
        if (!body->active || body->shape.type == KHR_SHAPE_PLANE) {
            continue; /* Log dynamic bodies */
        }

        /* Count contacts involving this body */
        uint32_t ncontacts = 0;
        for (uint32_t c = 0; c < world->contact_count; c++) {
            if (world->contacts[c].body_a == b || world->contacts[c].body_b == b) {
                ncontacts++;
            }
        }

        float E = khr_rigid_body_compute_energy(body, KHR_PHYSICS_FLOOR_Y, ts->gravity);

        fprintf(ts->file,
                "%llu,%.6f,%u,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%u\n",
                (unsigned long long)tick,
                (double)t,
                body->user_id,
                (double)body->position[0],
                (double)body->position[1],
                (double)body->position[2],
                (double)body->rotation[0],
                (double)body->rotation[1],
                (double)body->rotation[2],
                (double)body->rotation[3],
                (double)body->velocity[0],
                (double)body->velocity[1],
                (double)body->velocity[2],
                (double)body->angular_velocity[0],
                (double)body->angular_velocity[1],
                (double)body->angular_velocity[2],
                (double)E,
                ncontacts);
    }
    ts->ticks_written++;
}

void khr_timeseries_close(khr_timeseries_t* ts) {
    if (ts == nullptr || ts->file == nullptr) {
        return;
    }
    fflush(ts->file);
    fclose(ts->file);
    ts->file = nullptr;
}

/*
 * Frame Record Logger
 */

bool khr_frame_record_open(khr_frame_record_t* fr, const char* filepath) {
    if (fr == nullptr || filepath == nullptr) {
        return false;
    }
    memset(fr, 0, sizeof(*fr));
    fr->file = fopen(filepath, "w");
    if (fr->file == nullptr) {
        return false;
    }

    fprintf(fr->file, "# Khoros Frame Telemetry v%u.%u.%u (wp_presentation_time empirical data)\n",
            ENGINE_VERSION_MAJOR, ENGINE_VERSION_MINOR, ENGINE_VERSION_PATCH);
    fprintf(fr->file, "frame,tick,presented_ns,monotonic_ns,draw_count,contacts,arena_used,status\n");
    fflush(fr->file);
    return true;
}

void khr_frame_record_write(khr_frame_record_t* fr,
                            uint64_t frame,
                            uint64_t tick,
                            uint64_t presented_ns,
                            uint64_t monotonic_ns,
                            uint32_t draw_count,
                            uint32_t contacts,
                            size_t arena_used,
                            const char* status) {
    if (fr == nullptr || fr->file == nullptr) {
        return;
    }
    fprintf(fr->file, "%llu,%llu,%llu,%llu,%u,%u,%zu,%s\n",
            (unsigned long long)frame,
            (unsigned long long)tick,
            (unsigned long long)presented_ns,
            (unsigned long long)monotonic_ns,
            draw_count,
            contacts,
            arena_used,
            (status != nullptr) ? status : "presented");
    fr->frames_written++;
}

void khr_frame_record_close(khr_frame_record_t* fr) {
    if (fr == nullptr || fr->file == nullptr) {
        return;
    }
    fflush(fr->file);
    fclose(fr->file);
    fr->file = nullptr;
}

/*
 * 64-bit FNV-1a Hash of Rigid Body Simulation State
 */
static inline uint64_t fnv1a64_update(uint64_t hash, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint64_t)p[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

uint64_t khr_physics_world_hash_state(const khr_physics_world_t* world) {
    if (world == nullptr) {
        return 0;
    }
    uint64_t hash = 0xcbf29ce484222325ULL; /* FNV-1a 64-bit offset basis */

    for (uint32_t i = 0; i < world->body_count; i++) {
        const khr_rigid_body_t* b = &world->bodies[i];
        if (!b->active || b->shape.type == KHR_SHAPE_PLANE) {
            continue;
        }
        hash = fnv1a64_update(hash, b->position, sizeof(b->position));
        hash = fnv1a64_update(hash, b->rotation, sizeof(b->rotation));
        hash = fnv1a64_update(hash, b->velocity, sizeof(b->velocity));
        hash = fnv1a64_update(hash, b->angular_velocity, sizeof(b->angular_velocity));
    }
    return hash;
}
