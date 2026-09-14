#include "engine.h"
#include "khoros/gfx/blob.h"
#include "khoros/core/deck.h"
#include "khoros/core/timeseries.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char* prog) {
    printf("Usage: %s [OPTIONS] [blob_path | deck_path]\n\n", prog);
    printf("Options:\n");
    printf("  --deck=<path>       Load an experiment deck (# khoros-run v1) into live 3D window\n");
    printf("  --headless          Run simulation headlessly in terminal without opening 3D window\n");
    printf("  --ticks=<N>         Run exactly N simulation steps (default: 1000)\n");
    printf("  --fixed-dt          Enforce fixed timestep dt\n");
    printf("  --seed=<N>          Set random seed for simulation perturbations\n");
    printf("  --no-wall-clock     Headless simulation fast-forward without display pacing\n");
    printf("  --csv=<path>        Output per-tick mechanical state to CSV (implies headless)\n");
    printf("  --frame-csv=<path>  Output per-present presentation time telemetry to CSV\n");
    printf("  --hash-only         Output only 64-bit pose state hash for determinism tests\n");
    printf("  --write-box <path>      Write procedural box mesh blob to file and exit\n");
    printf("  --write-sphere <path>   Write procedural sphere mesh blob to file and exit\n");
    printf("  --write-cylinder <path> Write procedural cylinder mesh blob to file and exit\n");
    printf("  --write-torus <path>    Write procedural torus mesh blob to file and exit\n");
    printf("  --audio=<backend>       Select audio backend: auto (default), pipewire, pulse, alsa, null\n");
    printf("  --no-audio              Disable audio engine (null sink)\n");
    printf("  --audio-card=<N>        Select ALSA sound card index (default: 0)\n");
    printf("  --audio-device=<N>      Select ALSA playback device index (default: 0)\n");
    printf("  --target-fps=<N>        Cap presentation rate to N FPS (e.g., 60, 120, 144, 240)\n");
    printf("  --stress-n=<N>          Spawn N rigid bodies in 3D scene to stress GPU & physics\n");
    printf("  --stress-gpu            Saturate GPU with 1024 rigid bodies\n");
    printf("  --unlocked              Unlock presentation rate (disable 60 Hz cap to maximize GPU throughput)\n");
    printf("  -h, --help              Display this help message\n");
}

static int run_headless_simulation(const char* deck_path,
                                   uint32_t ticks,
                                   uint32_t seed,
                                   float dt,
                                   const char* csv_path,
                                   bool hash_only) {
    khr_deck_t deck = {};
    if (deck_path != nullptr) {
        if (!khr_deck_load_file(&deck, deck_path)) {
            fprintf(stderr, "Error: Failed to load deck '%s'\n", deck_path);
            return 1;
        }
    } else {
        khr_deck_init_defaults(&deck);
        /* Default ballistic falling sphere */
        khr_rigid_body_t b = {};
        float pos[3] = { 0.0f, 5.0f, 0.0f };
        khr_rigid_body_init_sphere(&b, pos, 0.5f, 1.0f, 0.75f, 0.4f);
        b.user_id = 0;
        deck.bodies[deck.body_count++] = b;
    }

    if (seed != 0) deck.seed = seed;
    if (dt > 0.0f) deck.dt_s = dt;
    if (ticks == 0) ticks = (deck.ticks > 0) ? deck.ticks : 1000;

    khr_physics_world_t* world = (khr_physics_world_t*)malloc(sizeof(khr_physics_world_t));
    if (world == nullptr) {
        fprintf(stderr, "Error: Out of memory for physics world\n");
        return 1;
    }
    (void)khr_deck_apply_to_world(&deck, world);

    khr_timeseries_t ts = {};
    bool logging = false;
    const char* out_csv = (csv_path != nullptr) ? csv_path : (deck.log_csv[0] != '\0' ? deck.log_csv : nullptr);
    if (out_csv != nullptr && !hash_only) {
        logging = khr_timeseries_open(&ts, out_csv, deck.dt_s, deck.gravity, "arch-cetin");
    }

    float sim_time = 0.0f;
    for (uint32_t t = 0; t < ticks; t++) {
        if (logging) {
            khr_timeseries_write_tick(&ts, t, sim_time, world);
        }
        khr_physics_world_step(world, deck.dt_s);
        sim_time += deck.dt_s;
    }
    if (logging) {
        khr_timeseries_write_tick(&ts, ticks, sim_time, world);
        khr_timeseries_close(&ts);
    }

    uint64_t hash = khr_physics_world_hash_state(world);
    if (hash_only) {
        printf("0x%016llx\n", (unsigned long long)hash);
    } else {
        printf("=== Khoros Headless Experiment Complete ===\n");
        printf("  Deck:       %s\n", deck_path ? deck_path : "(procedural)");
        printf("  Ticks:      %u (dt = %.6f s, total = %.3f s)\n", ticks, (double)deck.dt_s, (double)sim_time);
        printf("  Bodies:     %u\n", world->body_count);
        printf("  Contacts:   %u\n", world->contact_count);
        printf("  Energy:     %.4f J\n", (double)khr_physics_world_compute_total_energy(world));
        if (out_csv) printf("  Time Series: %s (%llu ticks logged)\n", out_csv, (unsigned long long)ts.ticks_written);
        printf("  State Hash: 0x%016llx\n", (unsigned long long)hash);
    }

    free(world);
    return 0;
}

int main(int argc, char** argv) {
    const char* deck_path = nullptr;
    const char* blob_path = nullptr;
    const char* csv_path = nullptr;
    uint32_t ticks = 0;
    uint32_t seed = 0;
    float dt = 0.0f;
    bool no_wall_clock = false;
    bool hash_only = false;
    bool headless = false;
    bool no_audio = false;
    const char* audio_backend = nullptr;
    uint32_t audio_card = 0;
    uint32_t audio_device = 0;
    uint32_t stress_n = 0;
    bool unlocked = false;
    bool stress_gpu = false;
    uint32_t target_fps = 0;
    uint32_t max_frames = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--write-box") == 0 && i + 1 < argc) {
            uint8_t buf[512];
            size_t n = khr_blob_write_box(buf, sizeof(buf));
            if (n == 0) return 1;
            FILE* f = fopen(argv[i + 1], "wb");
            if (f == nullptr || fwrite(buf, 1, n, f) != n) {
                if (f != nullptr) fclose(f);
                return 1;
            }
            fclose(f);
            return 0;
        } else if (strcmp(argv[i], "--write-sphere") == 0 && i + 1 < argc) {
            uint8_t buf[65536];
            size_t n = khr_blob_write_sphere(buf, sizeof(buf), 0.75f);
            if (n == 0) return 1;
            FILE* f = fopen(argv[i + 1], "wb");
            if (f == nullptr || fwrite(buf, 1, n, f) != n) {
                if (f != nullptr) fclose(f);
                return 1;
            }
            fclose(f);
            return 0;
        } else if (strcmp(argv[i], "--write-cylinder") == 0 && i + 1 < argc) {
            uint8_t buf[65536];
            size_t n = khr_blob_write_cylinder(buf, sizeof(buf), 0.5f, 1.2f);
            if (n == 0) return 1;
            FILE* f = fopen(argv[i + 1], "wb");
            if (f == nullptr || fwrite(buf, 1, n, f) != n) {
                if (f != nullptr) fclose(f);
                return 1;
            }
            fclose(f);
            return 0;
        } else if (strcmp(argv[i], "--write-torus") == 0 && i + 1 < argc) {
            uint8_t buf[65536];
            size_t n = khr_blob_write_torus(buf, sizeof(buf), 0.6f, 0.22f);
            if (n == 0) return 1;
            FILE* f = fopen(argv[i + 1], "wb");
            if (f == nullptr || fwrite(buf, 1, n, f) != n) {
                if (f != nullptr) fclose(f);
                return 1;
            }
            fclose(f);
            return 0;
        } else if (strcmp(argv[i], "--no-audio") == 0) {
            no_audio = true;
        } else if (strncmp(argv[i], "--audio=", 8) == 0) {
            audio_backend = argv[i] + 8;
        } else if (strcmp(argv[i], "--audio") == 0 && i + 1 < argc) {
            audio_backend = argv[++i];
        } else if (strncmp(argv[i], "--audio-card=", 13) == 0) {
            audio_card = (uint32_t)strtoul(argv[i] + 13, nullptr, 10);
        } else if (strncmp(argv[i], "--audio-device=", 15) == 0) {
            audio_device = (uint32_t)strtoul(argv[i] + 15, nullptr, 10);
        } else if (strncmp(argv[i], "--target-fps=", 13) == 0) {
            target_fps = (uint32_t)strtoul(argv[i] + 13, nullptr, 10);
        } else if (strcmp(argv[i], "--target-fps") == 0 && i + 1 < argc) {
            target_fps = (uint32_t)strtoul(argv[++i], nullptr, 10);
        } else if (strncmp(argv[i], "--fps=", 6) == 0) {
            target_fps = (uint32_t)strtoul(argv[i] + 6, nullptr, 10);
        } else if (strncmp(argv[i], "--frames=", 9) == 0) {
            max_frames = (uint32_t)strtoul(argv[i] + 9, nullptr, 10);
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = (uint32_t)strtoul(argv[++i], nullptr, 10);
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            max_frames = (uint32_t)strtoul(argv[++i], nullptr, 10);
        } else if (strncmp(argv[i], "--stress-n=", 11) == 0) {
            stress_n = (uint32_t)strtoul(argv[i] + 11, nullptr, 10);
        } else if (strcmp(argv[i], "--stress-gpu") == 0) {
            stress_gpu = true;
            stress_n = 1024;
            unlocked = true;
        } else if (strcmp(argv[i], "--unlocked") == 0) {
            unlocked = true;
        } else if (strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (strncmp(argv[i], "--deck=", 7) == 0) {
            deck_path = argv[i] + 7;
        } else if (strcmp(argv[i], "--deck") == 0 && i + 1 < argc) {
            deck_path = argv[++i];
        } else if (strncmp(argv[i], "--ticks=", 8) == 0) {
            ticks = (uint32_t)strtoul(argv[i] + 8, nullptr, 10);
            headless = true;
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            ticks = (uint32_t)strtoul(argv[++i], nullptr, 10);
            headless = true;
        } else if (strncmp(argv[i], "--seed=", 7) == 0) {
            seed = (uint32_t)strtoul(argv[i] + 7, nullptr, 10);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            seed = (uint32_t)strtoul(argv[++i], nullptr, 10);
        } else if (strncmp(argv[i], "--dt=", 5) == 0) {
            dt = strtof(argv[i] + 5, nullptr);
        } else if (strcmp(argv[i], "--fixed-dt") == 0) {
            /* Handled in simulation step */
        } else if (strcmp(argv[i], "--no-wall-clock") == 0) {
            no_wall_clock = true;
            headless = true;
        } else if (strncmp(argv[i], "--csv=", 6) == 0) {
            csv_path = argv[i] + 6;
            headless = true;
        } else if (strcmp(argv[i], "--hash-only") == 0) {
            hash_only = true;
            headless = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            /* Check if argument is a deck or mesh blob */
            if (strstr(argv[i], ".deck") != nullptr || strstr(argv[i], ".txt") != nullptr) {
                deck_path = argv[i];
            } else {
                blob_path = argv[i];
            }
        }
    }

    if (headless || no_wall_clock || hash_only) {
        return run_headless_simulation(deck_path, ticks, seed, dt, csv_path, hash_only);
    }

    engine_options_t opts = {
        .blob_path = blob_path,
        .deck_path = deck_path,
        .no_audio = no_audio,
        .audio_backend = audio_backend,
        .audio_card = audio_card,
        .audio_device = audio_device,
        .stress_n = stress_n,
        .unlocked = unlocked,
        .stress_gpu = stress_gpu,
        .target_fps = target_fps,
        .max_frames = max_frames,
    };
    auto ok = engine_init_opts(&opts);
    if (!ok) {
        return 1;
    }
    return 0;
}
