#include "test_framework.h"

/* Define global test context */
test_run_context_t g_khr_test_ctx = {};

#ifdef __SANITIZE_ADDRESS__
const char* __lsan_default_suppressions(void) {
    return "leak:libvulkan\nleak:libvulkan_intel\nleak:libdrm\nleak:<unknown module>\n";
}
#endif

/* Suite 1: ISO C23 Core Standard Features */
extern bool test_c23_keywords(void);
extern bool test_c23_constexpr_and_literals(void);
extern bool test_gnu_source_and_engine_banner(void);

/* Suite 2: Wayland Wire Protocol & Raw io_uring Ring A */
extern bool test_wayland_wire_encoding_decoding(void);
extern bool test_wayland_wire_fuzz_and_edge_cases(void);
extern bool test_wayland_wire_deep_fuzz_and_security(void);
extern bool test_wayland_client_mock_roundtrip(void);
extern bool test_wayland_direct_socket_connect_and_roundtrip(void);
extern bool test_wayland_pure_ring_native_socket_and_connect(void);
extern bool test_wayland_raw_uring_roundtrip(void);
extern bool test_xdg_shell_lifecycle(void);
extern bool test_window_hit_chrome_regions(void);
extern bool test_window_hit_list_first_match(void);
extern bool test_window_buffer_size_from_xdg(void);
extern bool test_plot_demo_samples_in_range(void);
extern bool test_blob_self_relative_box(void);
extern bool test_blob_ingest_leaves_ui_reserve(void);
extern bool test_blob_fit_centers_offset_mesh(void);
extern bool test_blob_box_outward_winding(void);
extern bool test_cam_orbit_snap_and_pick(void);
extern bool test_cam_aspect_ratio_scaling(void);
extern bool test_blob_gizmo_arm(void);
extern bool test_presentation_time_binding_and_lifecycle(void);
extern bool test_presentation_time_feedback_request_and_wire(void);
extern bool test_presentation_time_presented_parsing_and_metrics(void);
extern bool test_presentation_time_discarded_event(void);
extern bool test_presentation_predict_next_vsync_deadline(void);

/* Suite 3: Dual-Ring Topology & PBUF Architecture */
extern bool test_ring_a_clock_and_no_iowait(void);
extern bool test_msg_ring_64bit_payload(void);
extern bool test_pbuf_multishot_recvmsg(void);
extern bool test_pbuf_multishot_cycle_and_exhaustion(void);
extern bool test_pbuf_tier1_multishot_cycle_and_recycling(void);
extern bool test_sendmsg_skip_success(void);
extern bool test_ring_sq_saturation_and_edge_cases(void);
extern bool test_uring_timeout_precision_and_no_double_wait(void);
extern bool test_topology_bringup_and_ipc(void);
extern bool test_topology_ingest_read_fixed(void);
extern bool test_topology_ingest_edge_cases(void);
extern bool test_topology_ingest_multiblock_large_file(void);
extern bool test_ingest_atomic_hardlink_failure_resilience(void);
extern bool test_topology_cqe_staging_and_no_swallow(void);
extern bool test_topology_cqe_deep_staging_saturation(void);
extern bool test_topology_stale_path_and_cmdq(void);
extern bool test_topology_bda_stress_and_cmdq_saturation(void);
extern bool test_eventfd_starvation_watch(void);
extern bool test_fixed_fd_install(void);
extern bool test_sparse_files_registration_and_update(void);
extern bool test_direct_descriptor_ingest_pipeline(void);
extern bool test_wayland_pbuf_inbound_outbound(void);
extern bool test_cpu_pins_avoid_kernel_cores(void);
extern bool test_pump_classifies_no_swallow(void);
extern bool test_pump_burst_pairs_wake(void);
extern bool test_wayland_pbuf_pump_drives_parse(void);
extern bool test_topology_fixed_tick_sim(void);

/* Suite 4: Mock Wayland Compositor Subsystem */
extern bool test_mock_compositor_init_destroy(void);
extern bool test_mock_compositor_globals_advertisement(void);
extern bool test_mock_compositor_client_bind_tracking(void);
extern bool test_mock_compositor_surface_creation_and_commit(void);
extern bool test_mock_compositor_xdg_lifecycle_and_configure(void);
extern bool test_mock_compositor_ping_pong_keepalive(void);
extern bool test_mock_compositor_dmabuf_fd_import(void);
extern bool test_mock_compositor_drm_syncobj_timeline_import_and_points(void);
extern bool test_mock_compositor_timeline_destroy_lifecycle(void);
extern bool test_mock_compositor_multi_plane_dmabuf(void);
extern bool test_mock_compositor_buffer_release_event(void);
extern bool test_mock_compositor_malformed_request_resilience(void);

/* Suite 5: Vulkan 1.4 BDA & Dynamic Rendering Pipeline */
extern bool test_vulkan_instance_1_4_support(void);
extern bool test_vulkan_physical_device_enumeration(void);
extern bool test_vulkan_physical_device_scoring(void);
extern bool test_vulkan_bda_features_query(void);
extern bool test_vulkan_dynamic_rendering_and_sync2_features(void);
extern bool test_vulkan_push_constant_layout_128b_contract(void);
extern bool test_vulkan_uma_memory_types_query(void);
extern bool test_vulkan_queue_family_selection(void);
extern bool test_vulkan_device_init_and_scoring(void);
extern bool test_vulkan_bda_arena_alloc_and_coherence(void);
extern bool test_vulkan_slang_bytecode_and_modules(void);
extern bool test_vulkan_dynamic_rendering_pipeline_create(void);
extern bool test_bda_arena_uring_registration(void);
extern bool test_gfx_real_submit_and_readback(void);
extern bool test_vulkan_depth_reversed_z_occlusion(void);
extern bool test_bda_virtual_arena_pool_and_commit(void);
extern bool test_bda_arena_static_region_and_freeze(void);
extern bool test_bda_arena_slab_cache(void);
extern bool test_bda_arena_frame_scratch(void);
extern bool test_descriptor_buffer_device_support(void);
extern bool test_descriptor_heap_lifecycle_and_alloc(void);
extern bool test_descriptor_heap_register_sampler_direct_memory(void);
extern bool test_descriptor_heap_register_texture_direct_memory(void);
extern bool test_descriptor_heap_command_binding(void);

/* Suite 6: Linux DMA-BUF & Explicit DRM Syncobj Presentation */
extern bool test_drm_syncobj_create_and_export_fd(void);
extern bool test_drm_syncobj_fd_to_handle_roundtrip(void);
extern bool test_drm_syncobj_timeline_point_signaling(void);
extern bool test_drm_syncobj_timeline_wait_timeout(void);
extern bool test_drm_syncobj_per_image_release_isolation(void);
extern bool test_dmabuf_plane_parameters_layout(void);
extern bool test_dmabuf_export_and_wire_import_integration(void);
extern bool test_dmabuf_syncobj_full_surface_commit(void);
extern bool test_dmabuf_syncobj_timeline_destroy_on_swapchain_retire(void);
extern bool test_drm_syncobj_boundary_points(void);
extern bool test_dmabuf_wire_import_fd_passing(void);
extern bool test_dmabuf_vulkan_export_and_import(void);
extern bool test_dmabuf_modifier_intersection(void);
extern bool test_syncobj_wire_full_lifecycle(void);
extern bool test_syncobj_drm_fd_import(void);
extern bool test_dmabuf_present_created_failed_consume(void);
extern bool test_dmabuf_present_loop_mock(void);
extern bool test_dmabuf_present_acquire_bridge(void);
extern bool test_dmabuf_present_resize_rejects_unbound(void);
extern bool test_dmabuf_present_resize_keeps_sync_surface(void);
extern bool test_shm_wire_pool_and_buffer(void);
extern bool test_present_loop_release_recycling(void);
extern bool test_send_await_no_debt_no_steal(void);
extern bool test_await_tag_match_and_timeout(void);

/* Suite 7: Cross-Feature Interactions & Real-World Scenarios (Tiers 3 & 4) */
extern bool test_ipc_bda_stream_cross_core(void);
extern bool test_ipc_bda_high_throughput_burst(void);
extern bool test_ipc_ingestion_to_bda_pipeline(void);
extern bool test_tier3_bda_dmabuf_syncobj_pairwise(void);
extern bool test_tier4_mock_wayland_frame_loop(void);
extern bool test_tier4_144hz_presentation_pacing(void);
extern bool test_tier4_compositor_ping_keepalive_under_load(void);
extern bool test_tier4_window_resize_reconfiguration(void);
extern bool test_tier4_full_dual_thread_e2e_pipeline(void);

/* Suite 8: GPU-Driven Math & Compute Culling Pipeline (Pillar A) */
extern bool test_gpu_math_struct_contracts(void);
extern bool test_gpu_cull_pipeline_lifecycle(void);
extern bool test_gpu_mesh_instanced_pipeline_lifecycle(void);
extern bool test_gpu_compute_math_and_frustum_culling(void);
extern bool test_gpu_silicon_timestamp_queries(void);

/* Suite 9: Decoupled 3D Camera & PBR Material Pipeline (Pillars B & C) */
extern bool test_camera_lifecycle_and_defaults(void);
extern bool test_camera_arcball_virtual_sphere(void);
extern bool test_camera_pan_zoom_and_aabb(void);
extern bool test_camera_feed_cull_push_contract(void);
extern bool test_pbr_material_and_brdf_properties(void);
extern bool test_gpu_instanced_pbr_pipeline_execution(void);

/* Suite 10: Multi-Mesh Scene Graph & Physical Lights (Pillar D) */
extern bool test_light_struct_contracts(void);
extern bool test_physically_based_light_falloff(void);
extern bool test_spotlight_cone_penumbra(void);
extern bool test_scene_lifecycle_and_bda_allocation(void);
extern bool test_multi_mesh_compute_culling_and_indirect_dispatch(void);
extern bool test_multi_light_gpu_pbr_shading(void);

/* Suite 11: Two-Pass Hierarchical-Z (Hi-Z) Occlusion Culling (Pillar 5) */
extern bool test_hiz_struct_contracts(void);
extern bool test_hiz_pyramid_lifecycle_and_mips(void);
extern bool test_hiz_depth_downsample_and_build(void);
extern bool test_hiz_two_pass_occlusion_culling_dispatch(void);
extern bool test_hiz_multi_draw_indirect_count_execution(void);

/* Suite 12: Streaming Audio Engine & SIMD 3D Audio Mixer (Pillar 6) */
extern bool test_audio_clip_and_pool_contracts(void);
extern bool test_audio_spatial_attenuation_and_panning(void);
extern bool test_audio_doppler_frequency_shift(void);
extern bool test_audio_mixer_polyphony_and_simd_vectorization(void);
extern bool test_audio_engine_io_uring_direct_streaming(void);
extern bool test_audio_sample_rate_conversion(void);
extern bool test_audio_unaligned_clip_loop_avx2(void);
extern bool test_audio_zero_sample_rate_fallback(void);

/* Suite 13: Decoupled Action-Mapping Input System (Pillar 7) */
extern bool test_input_ring_spsc_lockfree_stress(void);
extern bool test_action_map_digital_edge_transitions(void);
extern bool test_action_map_analog_deadzone_and_curves(void);
extern bool test_action_map_temporal_smoothing(void);
extern bool test_action_map_fixed_tick_subtick_interpolation(void);
extern bool test_wayland_seat_input_ring_forwarding(void);
extern bool test_action_map_digital_axis_raw_keys(void);
extern bool test_input_ring_burst_saturation_and_overflow(void);
extern bool test_action_map_unbounded_mouse_clamp(void);

/* Suite 14: Zero-CPU Compressed Textures & GPU Synthesizer (Pillar 1) */
extern bool test_texture_header_parser_dds_dx10(void);
extern bool test_texture_header_parser_ktex(void);
extern bool test_texture_optimal_image_allocation(void);
extern bool test_texture_descriptor_buffer_registration(void);
extern bool test_texture_gpu_compute_synthesizer(void);
extern bool test_texture_gpu_copy2_staging_upload(void);

/* Suite 15: Dynamic Spatial Partitioning & Physics on Core 3 (Pillar 2) */
extern bool test_morton3d_encoding_and_ordering(void);
extern bool test_radix_sort_morton_codes(void);
extern bool test_lbvh_tree_construction_and_bounds(void);
extern bool test_lbvh_broadphase_query(void);
extern bool test_narrowphase_sphere_sphere_collision(void);
extern bool test_narrowphase_sphere_plane_collision(void);
extern bool test_narrowphase_sphere_aabb_collision(void);
extern bool test_narrowphase_sphere_capsule_collision(void);
extern bool test_rigid_body_impulse_restitution_and_friction(void);
extern bool test_physics_world_fixed_tick_simulation_stability(void);
extern bool test_physics_double_buffer_bda_integration(void);

/* Suite 16: Ground Grid, Procedural Meshes & Physical Sound Synthesis (Pillars 3 & 4) */
extern bool test_grid_pipeline_layout_and_push_contract(void);
extern bool test_procedural_sphere_generation(void);
extern bool test_procedural_cylinder_generation(void);
extern bool test_audio_synth_impact_modal_resonance(void);
extern bool test_audio_synth_thud_decay(void);
extern bool test_audio_synth_click_transient(void);
extern bool test_physics_collision_event_queue(void);

/* Suite 17: Version 0.2 Verification & Physics Honesty (Contract Fixtures) */
extern bool test_ballistic_symplectic_convergence(void);
extern bool test_restitution_normal_impact_map(void);
extern bool test_lbvh_vs_naive_broadphase(void);
extern bool test_hiz_occupancy_ratio(void);

/* Suite 18: Version 0.3 Instrumentation & Determinism (Named Contract) */
extern bool test_deck_parser_syntax_and_directives(void);
extern bool test_deck_file_loading(void);
extern bool test_timeseries_csv_emission_and_energy(void);
extern bool test_frame_record_telemetry_csv(void);
extern bool test_simulation_determinism_hash(void);

int main(void) {
    printf("\n" KHR_CLR_BOLD KHR_CLR_CYAN);
    printf("┌──────────────────────────────────────────────────────────────────────┐\n");
    printf("│                Khoros Engine Test Suite (ISO C23)                    │\n");
    printf("└──────────────────────────────────────────────────────────────────────┘\n");
    printf(KHR_CLR_RESET "\n");

    test_suite_stats_t stats = {};
    khr_test_init_suite(&stats);

    printf(KHR_CLR_BOLD "Suite 1: ISO C23 Core Standard Features\n" KHR_CLR_RESET);
    RUN_TEST(&stats, test_c23_keywords);
    RUN_TEST(&stats, test_c23_constexpr_and_literals);
    RUN_TEST(&stats, test_gnu_source_and_engine_banner);

    printf("\n" KHR_CLR_BOLD "Suite 2: Wayland Wire Protocol & Raw io_uring Ring A\n" KHR_CLR_RESET);
    RUN_TEST(&stats, test_wayland_wire_encoding_decoding);
    RUN_TEST(&stats, test_wayland_wire_fuzz_and_edge_cases);
    RUN_TEST(&stats, test_wayland_wire_deep_fuzz_and_security);
    RUN_TEST(&stats, test_wayland_client_mock_roundtrip);
    RUN_TEST(&stats, test_wayland_direct_socket_connect_and_roundtrip);
    RUN_TEST(&stats, test_wayland_pure_ring_native_socket_and_connect);
    RUN_TEST(&stats, test_wayland_raw_uring_roundtrip);
    RUN_TEST(&stats, test_xdg_shell_lifecycle);
    RUN_TEST(&stats, test_window_hit_chrome_regions);
    RUN_TEST(&stats, test_window_hit_list_first_match);
    RUN_TEST(&stats, test_window_buffer_size_from_xdg);
    RUN_TEST(&stats, test_plot_demo_samples_in_range);
    RUN_TEST(&stats, test_blob_self_relative_box);
    RUN_TEST(&stats, test_blob_ingest_leaves_ui_reserve);
    RUN_TEST(&stats, test_blob_fit_centers_offset_mesh);
    RUN_TEST(&stats, test_blob_box_outward_winding);
    RUN_TEST(&stats, test_cam_orbit_snap_and_pick);
    RUN_TEST(&stats, test_cam_aspect_ratio_scaling);
    RUN_TEST(&stats, test_blob_gizmo_arm);
    RUN_TEST(&stats, test_shm_wire_pool_and_buffer);
    RUN_TEST(&stats, test_presentation_time_binding_and_lifecycle);
    RUN_TEST(&stats, test_presentation_time_feedback_request_and_wire);
    RUN_TEST(&stats, test_presentation_time_presented_parsing_and_metrics);
    RUN_TEST(&stats, test_presentation_time_discarded_event);
    RUN_TEST(&stats, test_presentation_predict_next_vsync_deadline);

    printf("\n" KHR_CLR_BOLD "Suite 3: Dual-Ring Topology & PBUF Architecture\n" KHR_CLR_RESET);
    RUN_TEST(&stats, test_ring_a_clock_and_no_iowait);
    RUN_TEST(&stats, test_msg_ring_64bit_payload);
    RUN_TEST(&stats, test_pbuf_multishot_recvmsg);
    RUN_TEST(&stats, test_pbuf_multishot_cycle_and_exhaustion);
    RUN_TEST(&stats, test_pbuf_tier1_multishot_cycle_and_recycling);
    RUN_TEST(&stats, test_sendmsg_skip_success);
    RUN_TEST(&stats, test_ring_sq_saturation_and_edge_cases);
    RUN_TEST(&stats, test_uring_timeout_precision_and_no_double_wait);
    RUN_TEST(&stats, test_topology_bringup_and_ipc);
    RUN_TEST(&stats, test_topology_ingest_read_fixed);
    RUN_TEST(&stats, test_topology_ingest_edge_cases);
    RUN_TEST(&stats, test_topology_ingest_multiblock_large_file);
    RUN_TEST(&stats, test_ingest_atomic_hardlink_failure_resilience);
    RUN_TEST(&stats, test_topology_cqe_staging_and_no_swallow);
    RUN_TEST(&stats, test_topology_cqe_deep_staging_saturation);
    RUN_TEST(&stats, test_topology_stale_path_and_cmdq);
    RUN_TEST(&stats, test_topology_bda_stress_and_cmdq_saturation);
    RUN_TEST(&stats, test_eventfd_starvation_watch);
    RUN_TEST(&stats, test_fixed_fd_install);
    RUN_TEST(&stats, test_sparse_files_registration_and_update);
    RUN_TEST(&stats, test_direct_descriptor_ingest_pipeline);
    RUN_TEST(&stats, test_wayland_pbuf_inbound_outbound);
    RUN_TEST(&stats, test_cpu_pins_avoid_kernel_cores);
    RUN_TEST(&stats, test_pump_classifies_no_swallow);
    RUN_TEST(&stats, test_pump_burst_pairs_wake);
    RUN_TEST(&stats, test_wayland_pbuf_pump_drives_parse);
    RUN_TEST(&stats, test_send_await_no_debt_no_steal);
    RUN_TEST(&stats, test_await_tag_match_and_timeout);
    RUN_TEST(&stats, test_topology_fixed_tick_sim);

    printf("\n" KHR_CLR_BOLD "Suite 4: Mock Wayland Compositor Subsystem\n" KHR_CLR_RESET);
    RUN_TEST(&stats, test_mock_compositor_init_destroy);
    RUN_TEST(&stats, test_mock_compositor_globals_advertisement);
    RUN_TEST(&stats, test_mock_compositor_client_bind_tracking);
    RUN_TEST(&stats, test_mock_compositor_surface_creation_and_commit);
    RUN_TEST(&stats, test_mock_compositor_xdg_lifecycle_and_configure);
    RUN_TEST(&stats, test_mock_compositor_ping_pong_keepalive);
    RUN_TEST(&stats, test_mock_compositor_dmabuf_fd_import);
    RUN_TEST(&stats, test_mock_compositor_drm_syncobj_timeline_import_and_points);
    RUN_TEST(&stats, test_mock_compositor_timeline_destroy_lifecycle);
    RUN_TEST(&stats, test_mock_compositor_multi_plane_dmabuf);
    RUN_TEST(&stats, test_mock_compositor_buffer_release_event);
    RUN_TEST(&stats, test_mock_compositor_malformed_request_resilience);

    printf("\n" KHR_CLR_BOLD "Suite 5: Vulkan 1.4 BDA & Dynamic Rendering Pipeline\n" KHR_CLR_RESET);
    RUN_TEST(&stats, test_vulkan_instance_1_4_support);
    RUN_TEST(&stats, test_vulkan_physical_device_enumeration);
    RUN_TEST(&stats, test_vulkan_physical_device_scoring);
    RUN_TEST(&stats, test_vulkan_bda_features_query);
    RUN_TEST(&stats, test_vulkan_dynamic_rendering_and_sync2_features);
    RUN_TEST(&stats, test_vulkan_push_constant_layout_128b_contract);
    RUN_TEST(&stats, test_vulkan_uma_memory_types_query);
    RUN_TEST(&stats, test_vulkan_queue_family_selection);
    RUN_TEST(&stats, test_vulkan_device_init_and_scoring);
    RUN_TEST(&stats, test_vulkan_bda_arena_alloc_and_coherence);
    RUN_TEST(&stats, test_vulkan_slang_bytecode_and_modules);
    RUN_TEST(&stats, test_vulkan_dynamic_rendering_pipeline_create);
    RUN_TEST(&stats, test_bda_arena_uring_registration);
    RUN_TEST(&stats, test_gfx_real_submit_and_readback);
    RUN_TEST(&stats, test_vulkan_depth_reversed_z_occlusion);
    RUN_TEST(&stats, test_bda_virtual_arena_pool_and_commit);
    RUN_TEST(&stats, test_bda_arena_static_region_and_freeze);
    RUN_TEST(&stats, test_bda_arena_slab_cache);
    RUN_TEST(&stats, test_bda_arena_frame_scratch);
    RUN_TEST(&stats, test_descriptor_buffer_device_support);
    RUN_TEST(&stats, test_descriptor_heap_lifecycle_and_alloc);
    RUN_TEST(&stats, test_descriptor_heap_register_sampler_direct_memory);
    RUN_TEST(&stats, test_descriptor_heap_register_texture_direct_memory);
    RUN_TEST(&stats, test_descriptor_heap_command_binding);

    printf("\n" KHR_CLR_BOLD "Suite 6: Linux DMA-BUF & Explicit DRM Syncobj Presentation\n" KHR_CLR_RESET);
    RUN_TEST(&stats, test_drm_syncobj_create_and_export_fd);
    RUN_TEST(&stats, test_drm_syncobj_fd_to_handle_roundtrip);
    RUN_TEST(&stats, test_drm_syncobj_timeline_point_signaling);
    RUN_TEST(&stats, test_drm_syncobj_timeline_wait_timeout);
    RUN_TEST(&stats, test_drm_syncobj_per_image_release_isolation);
    RUN_TEST(&stats, test_dmabuf_plane_parameters_layout);
    RUN_TEST(&stats, test_dmabuf_export_and_wire_import_integration);
    RUN_TEST(&stats, test_dmabuf_syncobj_full_surface_commit);
    RUN_TEST(&stats, test_dmabuf_syncobj_timeline_destroy_on_swapchain_retire);
    RUN_TEST(&stats, test_drm_syncobj_boundary_points);
    RUN_TEST(&stats, test_dmabuf_wire_import_fd_passing);
    RUN_TEST(&stats, test_dmabuf_vulkan_export_and_import);
    RUN_TEST(&stats, test_dmabuf_modifier_intersection);
    RUN_TEST(&stats, test_syncobj_wire_full_lifecycle);
    RUN_TEST(&stats, test_syncobj_drm_fd_import);
    RUN_TEST(&stats, test_dmabuf_present_created_failed_consume);
    RUN_TEST(&stats, test_dmabuf_present_loop_mock);
    RUN_TEST(&stats, test_dmabuf_present_acquire_bridge);
    RUN_TEST(&stats, test_dmabuf_present_resize_rejects_unbound);
    RUN_TEST(&stats, test_dmabuf_present_resize_keeps_sync_surface);

    printf("\n" KHR_CLR_BOLD "Suite 7: Cross-Feature Interactions & Real-World Scenarios (Tiers 3 & 4)\n" KHR_CLR_RESET);
    RUN_TEST(&stats, test_ipc_bda_stream_cross_core);
    RUN_TEST(&stats, test_ipc_bda_high_throughput_burst);
    RUN_TEST(&stats, test_ipc_ingestion_to_bda_pipeline);
    RUN_TEST(&stats, test_tier3_bda_dmabuf_syncobj_pairwise);
    RUN_TEST(&stats, test_tier4_mock_wayland_frame_loop);
    RUN_TEST(&stats, test_tier4_144hz_presentation_pacing);
    RUN_TEST(&stats, test_tier4_compositor_ping_keepalive_under_load);
    RUN_TEST(&stats, test_tier4_window_resize_reconfiguration);
    RUN_TEST(&stats, test_tier4_full_dual_thread_e2e_pipeline);
    RUN_TEST(&stats, test_present_loop_release_recycling);

    printf("\n" KHR_CLR_BOLD "Suite 8: GPU-Driven Math & Compute Culling Pipeline (Pillar A)\n" KHR_CLR_RESET);
    RUN_TEST(&stats, test_gpu_math_struct_contracts);
    RUN_TEST(&stats, test_gpu_cull_pipeline_lifecycle);
    RUN_TEST(&stats, test_gpu_mesh_instanced_pipeline_lifecycle);
    RUN_TEST(&stats, test_gpu_compute_math_and_frustum_culling);
    RUN_TEST(&stats, test_gpu_silicon_timestamp_queries);

    printf("\n" KHR_CLR_BOLD "Suite 9: Decoupled 3D Camera & PBR Material Pipeline (Pillars B & C)\n" KHR_CLR_RESET);
    RUN_TEST(&stats, test_camera_lifecycle_and_defaults);
    RUN_TEST(&stats, test_camera_arcball_virtual_sphere);
    RUN_TEST(&stats, test_camera_pan_zoom_and_aabb);
    RUN_TEST(&stats, test_camera_feed_cull_push_contract);
    RUN_TEST(&stats, test_pbr_material_and_brdf_properties);
    RUN_TEST(&stats, test_gpu_instanced_pbr_pipeline_execution);

    printf("\n" KHR_CLR_BOLD "Suite 10: Multi-Mesh Scene Graph & Physical Lights (Pillar D)\n" KHR_CLR_RESET);
    RUN_TEST(&stats, test_light_struct_contracts);
    RUN_TEST(&stats, test_physically_based_light_falloff);
    RUN_TEST(&stats, test_spotlight_cone_penumbra);
    RUN_TEST(&stats, test_scene_lifecycle_and_bda_allocation);
    RUN_TEST(&stats, test_multi_mesh_compute_culling_and_indirect_dispatch);
    RUN_TEST(&stats, test_multi_light_gpu_pbr_shading);

    printf("\n" KHR_CLR_BOLD "Suite 11: Two-Pass Hierarchical-Z (Hi-Z) Occlusion Culling (Pillar 5)\n" KHR_CLR_RESET);
    RUN_TEST(&stats, test_hiz_struct_contracts);
    RUN_TEST(&stats, test_hiz_pyramid_lifecycle_and_mips);
    RUN_TEST(&stats, test_hiz_depth_downsample_and_build);
    RUN_TEST(&stats, test_hiz_two_pass_occlusion_culling_dispatch);
    RUN_TEST(&stats, test_hiz_multi_draw_indirect_count_execution);

    printf("\n" KHR_CLR_BOLD "Suite 12: Streaming Audio Engine & SIMD 3D Audio Mixer (Pillar 6)\n" KHR_CLR_RESET);
    RUN_TEST(&stats, test_audio_clip_and_pool_contracts);
    RUN_TEST(&stats, test_audio_spatial_attenuation_and_panning);
    RUN_TEST(&stats, test_audio_doppler_frequency_shift);
    RUN_TEST(&stats, test_audio_mixer_polyphony_and_simd_vectorization);
    RUN_TEST(&stats, test_audio_engine_io_uring_direct_streaming);
    RUN_TEST(&stats, test_audio_sample_rate_conversion);
    RUN_TEST(&stats, test_audio_unaligned_clip_loop_avx2);
    RUN_TEST(&stats, test_audio_zero_sample_rate_fallback);

    printf("\n" KHR_CLR_BOLD "Suite 13: Decoupled Action-Mapping Input System (Pillar 7)\n" KHR_CLR_RESET);
    RUN_TEST(&stats, test_input_ring_spsc_lockfree_stress);
    RUN_TEST(&stats, test_action_map_digital_edge_transitions);
    RUN_TEST(&stats, test_action_map_analog_deadzone_and_curves);
    RUN_TEST(&stats, test_action_map_temporal_smoothing);
    RUN_TEST(&stats, test_action_map_fixed_tick_subtick_interpolation);
    RUN_TEST(&stats, test_wayland_seat_input_ring_forwarding);
    RUN_TEST(&stats, test_action_map_digital_axis_raw_keys);
    RUN_TEST(&stats, test_input_ring_burst_saturation_and_overflow);
    RUN_TEST(&stats, test_action_map_unbounded_mouse_clamp);

    printf("\n" KHR_CLR_BOLD "Suite 14: Zero-CPU Compressed Textures & GPU Synthesizer (Pillar 1)\n" KHR_CLR_RESET);
    RUN_TEST(&stats, test_texture_header_parser_dds_dx10);
    RUN_TEST(&stats, test_texture_header_parser_ktex);
    RUN_TEST(&stats, test_texture_optimal_image_allocation);
    RUN_TEST(&stats, test_texture_descriptor_buffer_registration);
    RUN_TEST(&stats, test_texture_gpu_compute_synthesizer);
    RUN_TEST(&stats, test_texture_gpu_copy2_staging_upload);

    printf("\n" KHR_CLR_BOLD "Suite 15: Dynamic Spatial Partitioning & Physics on Core 3 (Pillar 2)\n" KHR_CLR_RESET);
    RUN_TEST(&stats, test_morton3d_encoding_and_ordering);
    RUN_TEST(&stats, test_radix_sort_morton_codes);
    RUN_TEST(&stats, test_lbvh_tree_construction_and_bounds);
    RUN_TEST(&stats, test_lbvh_broadphase_query);
    RUN_TEST(&stats, test_narrowphase_sphere_sphere_collision);
    RUN_TEST(&stats, test_narrowphase_sphere_plane_collision);
    RUN_TEST(&stats, test_narrowphase_sphere_aabb_collision);
    RUN_TEST(&stats, test_narrowphase_sphere_capsule_collision);
    RUN_TEST(&stats, test_rigid_body_impulse_restitution_and_friction);
    RUN_TEST(&stats, test_physics_world_fixed_tick_simulation_stability);
    RUN_TEST(&stats, test_physics_double_buffer_bda_integration);

    printf("\n" KHR_CLR_BOLD "Suite 16: Ground Grid, Procedural Meshes & Physical Sound Synthesis (Pillars 3 & 4)\n" KHR_CLR_RESET);
    RUN_TEST(&stats, test_grid_pipeline_layout_and_push_contract);
    RUN_TEST(&stats, test_procedural_sphere_generation);
    RUN_TEST(&stats, test_procedural_cylinder_generation);
    RUN_TEST(&stats, test_audio_synth_impact_modal_resonance);
    RUN_TEST(&stats, test_audio_synth_thud_decay);
    RUN_TEST(&stats, test_audio_synth_click_transient);
    RUN_TEST(&stats, test_physics_collision_event_queue);

    printf("\n" KHR_CLR_BOLD "Suite 17: Version 0.2 Verification & Physics Honesty (Contract Fixtures)\n" KHR_CLR_RESET);
    RUN_TEST(&stats, test_ballistic_symplectic_convergence);
    RUN_TEST(&stats, test_restitution_normal_impact_map);
    RUN_TEST(&stats, test_lbvh_vs_naive_broadphase);
    RUN_TEST(&stats, test_hiz_occupancy_ratio);

    printf("\n" KHR_CLR_BOLD "Suite 18: Version 0.3 Instrumentation & Determinism (Named Contract)\n" KHR_CLR_RESET);
    RUN_TEST(&stats, test_deck_parser_syntax_and_directives);
    RUN_TEST(&stats, test_deck_file_loading);
    RUN_TEST(&stats, test_timeseries_csv_emission_and_energy);
    RUN_TEST(&stats, test_frame_record_telemetry_csv);
    RUN_TEST(&stats, test_simulation_determinism_hash);

    return khr_test_finish_suite(&stats);
}
