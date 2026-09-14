#include "khoros/core/deck.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void khr_deck_init_defaults(khr_deck_t* deck) {
    if (deck == nullptr) {
        return;
    }
    memset(deck, 0, sizeof(*deck));
    deck->dt_s = KHR_PHYSICS_DEFAULT_DT_S;
    deck->gravity[0] = 0.0f;
    deck->gravity[1] = -KHR_PHYSICS_GRAVITY_M_S2;
    deck->gravity[2] = 0.0f;
    deck->floor_y = KHR_PHYSICS_FLOOR_Y;
    deck->seed = 1;
    deck->ticks = 600;
    strncpy(deck->integrator, "symplectic_euler", sizeof(deck->integrator) - 1);
    deck->body_count = 0;
}

static const char* skip_ws(const char* p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r')) {
        p++;
    }
    return p;
}

static bool parse_float(const char* str, float* out_val) {
    char* end = nullptr;
    float v = strtof(str, &end);
    if (end == str) {
        return false;
    }
    *out_val = v;
    return true;
}

static bool parse_uint(const char* str, uint32_t* out_val) {
    char* end = nullptr;
    unsigned long v = strtoul(str, &end, 10);
    if (end == str) {
        return false;
    }
    *out_val = (uint32_t)v;
    return true;
}

static void parse_body_key_value(const char* token,
                                 uint32_t* id,
                                 khr_shape_type_t* shape,
                                 float* radius,
                                 float* mass,
                                 float* restitution,
                                 float* friction,
                                 float pos[3],
                                 float vel[3],
                                 float hx[3]) {
    const char* eq = strchr(token, '=');
    if (eq == nullptr) {
        return;
    }
    size_t klen = (size_t)(eq - token);
    const char* val = eq + 1;

    if (strncmp(token, "id", klen) == 0) {
        (void)parse_uint(val, id);
    } else if (strncmp(token, "shape", klen) == 0) {
        if (strcmp(val, "sphere") == 0) {
            *shape = KHR_SHAPE_SPHERE;
        } else if (strcmp(val, "plane") == 0) {
            *shape = KHR_SHAPE_PLANE;
        } else if (strcmp(val, "aabb") == 0) {
            *shape = KHR_SHAPE_AABB;
        } else if (strcmp(val, "capsule") == 0) {
            *shape = KHR_SHAPE_CAPSULE;
        }
    } else if (strncmp(token, "r", klen) == 0 || strncmp(token, "radius", klen) == 0) {
        (void)parse_float(val, radius);
    } else if (strncmp(token, "mass", klen) == 0 || strncmp(token, "m", klen) == 0) {
        (void)parse_float(val, mass);
    } else if (strncmp(token, "e", klen) == 0 || strncmp(token, "restitution", klen) == 0) {
        (void)parse_float(val, restitution);
    } else if (strncmp(token, "mu", klen) == 0 || strncmp(token, "friction", klen) == 0) {
        (void)parse_float(val, friction);
    } else if (strncmp(token, "x", klen) == 0) {
        (void)parse_float(val, &pos[0]);
    } else if (strncmp(token, "y", klen) == 0) {
        (void)parse_float(val, &pos[1]);
    } else if (strncmp(token, "z", klen) == 0) {
        (void)parse_float(val, &pos[2]);
    } else if (strncmp(token, "vx", klen) == 0) {
        (void)parse_float(val, &vel[0]);
    } else if (strncmp(token, "vy", klen) == 0) {
        (void)parse_float(val, &vel[1]);
    } else if (strncmp(token, "vz", klen) == 0) {
        (void)parse_float(val, &vel[2]);
    } else if (strncmp(token, "hx", klen) == 0) {
        (void)parse_float(val, &hx[0]);
    } else if (strncmp(token, "hy", klen) == 0) {
        (void)parse_float(val, &hx[1]);
    } else if (strncmp(token, "hz", klen) == 0) {
        (void)parse_float(val, &hx[2]);
    }
}

bool khr_deck_parse_string(khr_deck_t* deck, const char* text, size_t len) {
    if (deck == nullptr || text == nullptr || len == 0) {
        return false;
    }
    khr_deck_init_defaults(deck);

    bool header_found = false;
    const char* cur = text;
    const char* end = text + len;

    while (cur < end) {
        /* Extract single line */
        const char* line_start = cur;
        while (cur < end && *cur != '\n') {
            cur++;
        }
        size_t line_len = (size_t)(cur - line_start);
        if (cur < end && *cur == '\n') {
            cur++;
        }

        if (line_len == 0) {
            continue;
        }

        char line_buf[512];
        size_t copy_sz = (line_len < sizeof(line_buf) - 1) ? line_len : (sizeof(line_buf) - 1);
        memcpy(line_buf, line_start, copy_sz);
        line_buf[copy_sz] = '\0';

        const char* p = skip_ws(line_buf);
        if (*p == '\0') {
            continue;
        }

        /* Check for format header */
        if (!header_found) {
            if (strncmp(p, "# khoros-run v1", 15) == 0) {
                header_found = true;
                continue;
            }
            if (*p == '#') {
                continue; /* Allow preceding comments */
            }
            /* If no explicit magic header, still accept if first non-comment has directives */
            header_found = true;
        }

        if (*p == '#') {
            continue;
        }

        char verb[64] = {};
        int n_read = 0;
        if (sscanf(p, "%63s%n", verb, &n_read) != 1) {
            continue;
        }
        const char* rest = skip_ws(p + n_read);

        if (strcmp(verb, "dt_s") == 0 || strcmp(verb, "dt") == 0) {
            (void)parse_float(rest, &deck->dt_s);
        } else if (strcmp(verb, "gravity") == 0) {
            float gx = 0.0f, gy = 0.0f, gz = 0.0f;
            if (sscanf(rest, "%f %f %f", &gx, &gy, &gz) == 3) {
                deck->gravity[0] = gx;
                deck->gravity[1] = gy;
                deck->gravity[2] = gz;
            }
        } else if (strcmp(verb, "floor_y") == 0) {
            (void)parse_float(rest, &deck->floor_y);
        } else if (strcmp(verb, "seed") == 0) {
            (void)parse_uint(rest, &deck->seed);
        } else if (strcmp(verb, "ticks") == 0 || strcmp(verb, "steps") == 0) {
            (void)parse_uint(rest, &deck->ticks);
        } else if (strcmp(verb, "integrator") == 0) {
            sscanf(rest, "%31s", deck->integrator);
        } else if (strcmp(verb, "log_csv") == 0) {
            sscanf(rest, "%255s", deck->log_csv);
        } else if (strcmp(verb, "body") == 0) {
            if (deck->body_count >= KHR_PHYSICS_MAX_BODIES) {
                continue;
            }

            uint32_t id = deck->body_count;
            khr_shape_type_t shape = KHR_SHAPE_SPHERE;
            float radius = 0.5f;
            float mass = 1.0f;
            float restitution = 0.7f;
            float friction = 0.4f;
            float pos[3] = { 0.0f, 0.0f, 0.0f };
            float vel[3] = { 0.0f, 0.0f, 0.0f };
            float hx[3] = { 0.5f, 0.5f, 0.5f };

            /* Tokenize remaining key=value pairs */
            char body_args[440];
            strncpy(body_args, rest, sizeof(body_args) - 1);
            body_args[sizeof(body_args) - 1] = '\0';

            char* saveptr = nullptr;
            char* tok = strtok_r(body_args, " \t\r\n", &saveptr);
            while (tok != nullptr) {
                parse_body_key_value(tok, &id, &shape, &radius, &mass, &restitution,
                                     &friction, pos, vel, hx);
                tok = strtok_r(nullptr, " \t\r\n", &saveptr);
            }

            khr_rigid_body_t* b = &deck->bodies[deck->body_count++];
            memset(b, 0, sizeof(*b));

            if (shape == KHR_SHAPE_SPHERE) {
                khr_rigid_body_init_sphere(b, pos, radius, mass, restitution, friction);
            } else if (shape == KHR_SHAPE_AABB) {
                khr_rigid_body_init_aabb(b, pos, hx, mass, restitution, friction);
            } else if (shape == KHR_SHAPE_CAPSULE) {
                float p0[3] = { 0.0f, -0.45f, 0.0f };
                float p1[3] = { 0.0f,  0.45f, 0.0f };
                khr_rigid_body_init_capsule(b, p0, p1, radius, mass, restitution, friction);
                b->position[0] = pos[0];
                b->position[1] = pos[1];
                b->position[2] = pos[2];
            } else if (shape == KHR_SHAPE_PLANE) {
                float norm[3] = { 0.0f, 1.0f, 0.0f };
                khr_rigid_body_init_plane(b, norm, pos[1], restitution, friction);
            } else {
                khr_rigid_body_init_sphere(b, pos, radius, mass, restitution, friction);
            }

            b->velocity[0] = vel[0];
            b->velocity[1] = vel[1];
            b->velocity[2] = vel[2];
            b->user_id = id;
        }
    }

    return true;
}

bool khr_deck_load_file(khr_deck_t* deck, const char* filepath) {
    if (deck == nullptr || filepath == nullptr) {
        return false;
    }
    FILE* f = fopen(filepath, "rb");
    if (f == nullptr) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0 || sz > 1024 * 1024) {
        fclose(f);
        return false;
    }
    fseek(f, 0, SEEK_SET);

    char* buf = (char*)malloc((size_t)sz + 1);
    if (buf == nullptr) {
        fclose(f);
        return false;
    }
    size_t read_bytes = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[read_bytes] = '\0';

    bool ok = khr_deck_parse_string(deck, buf, read_bytes);
    free(buf);
    return ok;
}

bool khr_deck_apply_to_world(const khr_deck_t* deck, khr_physics_world_t* world) {
    if (deck == nullptr || world == nullptr) {
        return false;
    }
    khr_physics_world_init(world);
    world->gravity[0] = deck->gravity[0];
    world->gravity[1] = deck->gravity[1];
    world->gravity[2] = deck->gravity[2];

    /* Always ensure static floor plane is present at deck->floor_y */
    bool have_floor = false;
    for (uint32_t i = 0; i < deck->body_count; i++) {
        if (deck->bodies[i].shape.type == KHR_SHAPE_PLANE) {
            have_floor = true;
            break;
        }
    }
    if (!have_floor) {
        khr_rigid_body_t floor_plane = {};
        float floor_n[3] = { 0.0f, 1.0f, 0.0f };
        khr_rigid_body_init_plane(&floor_plane, floor_n, deck->floor_y, 0.7f, 0.4f);
        floor_plane.user_id = UINT32_MAX;
        (void)khr_physics_world_add_body(world, &floor_plane);
    }

    for (uint32_t i = 0; i < deck->body_count; i++) {
        (void)khr_physics_world_add_body(world, &deck->bodies[i]);
    }
    return true;
}

uint32_t khr_deck_populate_instances(const khr_deck_t* deck,
                                     khr_gpu_instance_t* out_instances,
                                     uint32_t max_instances) {
    if (deck == nullptr || out_instances == nullptr || max_instances == 0) {
        return 0;
    }
    uint32_t count = 0;
    for (uint32_t i = 0; i < deck->body_count && count < max_instances; i++) {
        const khr_rigid_body_t* b = &deck->bodies[i];
        if (b->shape.type == KHR_SHAPE_PLANE) {
            continue; /* Plane rendered via ground grid shader */
        }
        float r = 0.5f;
        if (b->shape.type == KHR_SHAPE_SPHERE) {
            r = b->shape.sphere.radius;
        } else if (b->shape.type == KHR_SHAPE_AABB) {
            r = b->shape.aabb.half_extents[0];
        }

        out_instances[count] = (khr_gpu_instance_t){
            .position = { b->position[0], b->position[1], b->position[2] },
            .radius = r,
            .rotation = { b->rotation[0], b->rotation[1], b->rotation[2], b->rotation[3] },
            .scale = { r, r, r },
            .mesh_id = 0,
            .albedo = { 0.9f, 0.8f, 0.6f },
            .roughness = 0.3f,
            .metallic = 0.8f,
            .ao = 1.0f,
            .albedo_tex_id = UINT32_MAX,
            .normal_tex_id = UINT32_MAX,
        };
        count++;
    }
    return count;
}
