#include "test_framework.h"
#include "khoros/core/timeseries.h"
#include <string.h>
#include <math.h>

[[nodiscard]]
bool test_timeseries_csv_emission_and_energy(void) {
    khr_physics_world_t world = {};
    khr_physics_world_init(&world);

    /* Sphere at y = 5.0, mass = 2.0 kg */
    khr_rigid_body_t b = {};
    float pos[3] = { 0.0f, 5.0f, 0.0f };
    khr_rigid_body_init_sphere(&b, pos, 0.5f, 2.0f, 0.75f, 0.4f);
    b.user_id = 0;
    (void)khr_physics_world_add_body(&world, &b);

    /* Initial theoretical energy: PE = m * g * (y - floor_y) = 2.0 * 9.80665 * (5.0 - (-2.5)) = 2.0 * 9.80665 * 7.5 = 147.09975 J */
    float e0 = khr_physics_world_compute_total_energy(&world);
    TEST_ASSERT(fabsf(e0 - 147.09975f) < 0.05f, "initial potential energy calculation");

    const char* tmp_path = "experiments/test_timeseries_tmp.csv";
    khr_timeseries_t ts = {};
    float grav[3] = { 0.0f, -9.80665f, 0.0f };
    TEST_ASSERT(khr_timeseries_open(&ts, tmp_path, 1.0f / 60.0f, grav, "test_host"), "open timeseries");

    /* Log tick 0 */
    khr_timeseries_write_tick(&ts, 0, 0.0f, &world);

    /* Step physics and log tick 1 */
    khr_physics_world_step(&world, 1.0f / 60.0f);
    khr_timeseries_write_tick(&ts, 1, 1.0f / 60.0f, &world);

    khr_timeseries_close(&ts);
    TEST_ASSERT_EQ(ts.ticks_written, 2U, "two ticks written");

    /* Read and verify CSV file content */
    FILE* f = fopen(tmp_path, "r");
    TEST_ASSERT_NOT_NULL(f, "open written csv file");

    char line[512] = {};
    /* Line 1: Header */
    TEST_ASSERT_NOT_NULL(fgets(line, sizeof(line), f), "read header line");
    TEST_ASSERT_NOT_NULL(strstr(line, "# Khoros Time Series"), "header identifier");
    TEST_ASSERT_NOT_NULL(strstr(line, "__STDC_VERSION__=202311L"), "C23 version tag in header");

    /* Line 2: Columns */
    TEST_ASSERT_NOT_NULL(fgets(line, sizeof(line), f), "read columns line");
    TEST_ASSERT_EQ(strcmp(line, "tick,t,body_id,x,y,z,qx,qy,qz,qw,vx,vy,vz,wx,wy,wz,E,ncontacts\n"), 0, "column schema");

    /* Line 3: Tick 0 data */
    TEST_ASSERT_NOT_NULL(fgets(line, sizeof(line), f), "read tick 0 data");
    TEST_ASSERT_NOT_NULL(strstr(line, "147.099"), "tick 0 energy value present");

    fclose(f);
    remove(tmp_path);

    return true;
}

[[nodiscard]]
bool test_frame_record_telemetry_csv(void) {
    const char* tmp_path = "experiments/test_frame_record_tmp.csv";
    khr_frame_record_t fr = {};
    TEST_ASSERT(khr_frame_record_open(&fr, tmp_path), "open frame record");

    khr_frame_record_write(&fr, 1, 10, 1000500000ULL, 1000450000ULL, 4, 0, 1048576, "presented");
    khr_frame_record_write(&fr, 2, 20, 0ULL, 1017120000ULL, 4, 1, 1048576, "discarded");

    khr_frame_record_close(&fr);
    TEST_ASSERT_EQ(fr.frames_written, 2U, "two frames recorded");

    FILE* f = fopen(tmp_path, "r");
    TEST_ASSERT_NOT_NULL(f, "read frame record");

    char line[512] = {};
    TEST_ASSERT_NOT_NULL(fgets(line, sizeof(line), f), "read line 1");
    TEST_ASSERT_NOT_NULL(strstr(line, "wp_presentation_time"), "header contains protocol");

    TEST_ASSERT_NOT_NULL(fgets(line, sizeof(line), f), "read columns");
    TEST_ASSERT_EQ(strcmp(line, "frame,tick,presented_ns,monotonic_ns,draw_count,contacts,arena_used,status\n"), 0, "columns");

    TEST_ASSERT_NOT_NULL(fgets(line, sizeof(line), f), "read frame 1");
    TEST_ASSERT_NOT_NULL(strstr(line, "presented"), "frame 1 presented status");

    TEST_ASSERT_NOT_NULL(fgets(line, sizeof(line), f), "read frame 2");
    TEST_ASSERT_NOT_NULL(strstr(line, "discarded"), "frame 2 discarded status");

    fclose(f);
    remove(tmp_path);

    return true;
}
