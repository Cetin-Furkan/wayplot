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
extern bool test_window_buffer_size_from_xdg(void);

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
    RUN_TEST(&stats, test_window_buffer_size_from_xdg);
    RUN_TEST(&stats, test_shm_wire_pool_and_buffer);

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

    return khr_test_finish_suite(&stats);
}
