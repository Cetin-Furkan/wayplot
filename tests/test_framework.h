#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#if __STDC_VERSION__ < 202311L
#error "Tests require pure ISO C23 (compile with -std=c23)"
#endif

#ifdef _STDBOOL_H
#error "<stdbool.h> is strictly banned. In C23, bool, true, and false are native language keywords."
#endif

#ifdef _STDALIGN_H
#error "<stdalign.h> is strictly banned. In C23, alignas and alignof are native language keywords."
#endif

#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <string.h>

/*
 * ANSI Styling and Color Codes
 */
#define KHR_CLR_RESET       "\033[0m"
#define KHR_CLR_BOLD        "\033[1m"
#define KHR_CLR_DIM         "\033[2m"
#define KHR_CLR_RED         "\033[91m"
#define KHR_CLR_GREEN       "\033[92m"
#define KHR_CLR_YELLOW      "\033[93m"
#define KHR_CLR_BLUE        "\033[94m"
#define KHR_CLR_MAGENTA     "\033[95m"
#define KHR_CLR_CYAN        "\033[96m"
#define KHR_CLR_WHITE       "\033[97m"
#define KHR_BG_RED_BOLD     "\033[41m\033[97m\033[1m"
#define KHR_BG_GREEN_BOLD   "\033[42m\033[97m\033[1m"

constexpr size_t MAX_TEST_FAILURES = 256;
static const char FAILURES_LOG_PATH[] = "logs/test_logs/failures.log";

typedef struct {
    const char* test_name;
    const char* file;
    int         line;
    const char* expr;
    const char* message;
} test_failure_record_t;

typedef struct {
    uint32_t total;
    uint32_t passed;
    uint32_t failed;
    uint32_t assertions_checked;
    uint64_t start_time_ns;
    uint64_t end_time_ns;
    uint32_t failure_count;
    test_failure_record_t failures[MAX_TEST_FAILURES];
    FILE* fail_log_file;
} test_suite_stats_t;

/* Context variable for the currently executing test */
typedef struct {
    const char* current_test_name;
    test_suite_stats_t* stats;
    bool failed;
} test_run_context_t;

extern test_run_context_t g_khr_test_ctx;

static inline uint64_t khr_test_now_ns(void) {
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1'000'000'000ULL) + (uint64_t)ts.tv_nsec;
}

static inline void khr_test_record_failure(test_run_context_t* ctx,
                                           const char* file,
                                           int line,
                                           const char* expr,
                                           const char* message) {
    ctx->failed = true;
    test_suite_stats_t* stats = ctx->stats;

    if (stats != nullptr && stats->failure_count < MAX_TEST_FAILURES) {
        stats->failures[stats->failure_count] = (test_failure_record_t){
            .test_name = ctx->current_test_name,
            .file = file,
            .line = line,
            .expr = expr,
            .message = message
        };
        stats->failure_count++;
    }

    /* Print immediate detailed diagnostics in terminal */
    printf("\n  " KHR_CLR_RED "┌─ Failure Diagnostics ──────────────────────────────────────────────────" KHR_CLR_RESET "\n");
    printf("  " KHR_CLR_RED "│" KHR_CLR_RESET " " KHR_CLR_BOLD "Test:" KHR_CLR_RESET "     %s\n", ctx->current_test_name);
    printf("  " KHR_CLR_RED "│" KHR_CLR_RESET " " KHR_CLR_BOLD "Location:" KHR_CLR_RESET " %s:%d\n", file, line);
    printf("  " KHR_CLR_RED "│" KHR_CLR_RESET " " KHR_CLR_BOLD "Assert:" KHR_CLR_RESET "   " KHR_CLR_YELLOW "%s" KHR_CLR_RESET "\n", expr);
    printf("  " KHR_CLR_RED "│" KHR_CLR_RESET " " KHR_CLR_BOLD "Reason:" KHR_CLR_RESET "   " KHR_CLR_RED "%s" KHR_CLR_RESET "\n", message);
    printf("  " KHR_CLR_RED "└────────────────────────────────────────────────────────────────────────" KHR_CLR_RESET "\n");

    /* Lazy open and log failure into logs/test_logs/failures.log */
    if (stats != nullptr) {
        if (stats->fail_log_file == nullptr) {
            stats->fail_log_file = fopen(FAILURES_LOG_PATH, "w");
            if (stats->fail_log_file != nullptr) {
                fprintf(stats->fail_log_file, "=== Khoros Engine Test Failures Log ===\n\n");
            }
        }
        if (stats->fail_log_file != nullptr) {
            time_t raw_time = time(nullptr);
            char time_buf[64] = {};
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%S", localtime(&raw_time));

            fprintf(stats->fail_log_file, "[%s] FAILED: %s\n", time_buf, ctx->current_test_name);
            fprintf(stats->fail_log_file, "  File:      %s:%d\n", file, line);
            fprintf(stats->fail_log_file, "  Assert:    %s\n", expr);
            fprintf(stats->fail_log_file, "  Reason:    %s\n\n", message);
            fflush(stats->fail_log_file);
        }
    }
}

#define TEST_ASSERT(expr, msg) do { \
    if (g_khr_test_ctx.stats != nullptr) { \
        g_khr_test_ctx.stats->assertions_checked++; \
    } \
    if (!(expr)) { \
        khr_test_record_failure(&g_khr_test_ctx, __FILE__, __LINE__, #expr, (msg)); \
        return false; \
    } \
} while (0)

#define TEST_ASSERT_EQ(a, b, msg) do { \
    if (g_khr_test_ctx.stats != nullptr) { \
        g_khr_test_ctx.stats->assertions_checked++; \
    } \
    auto _a = (a); \
    auto _b = (b); \
    if (_a != _b) { \
        khr_test_record_failure(&g_khr_test_ctx, __FILE__, __LINE__, #a " == " #b, (msg)); \
        return false; \
    } \
} while (0)

#define TEST_ASSERT_NE(a, b, msg) do { \
    if (g_khr_test_ctx.stats != nullptr) { \
        g_khr_test_ctx.stats->assertions_checked++; \
    } \
    auto _a = (a); \
    auto _b = (b); \
    if (_a == _b) { \
        khr_test_record_failure(&g_khr_test_ctx, __FILE__, __LINE__, #a " != " #b, (msg)); \
        return false; \
    } \
} while (0)

#define TEST_ASSERT_NOT_NULL(ptr, msg) do { \
    if (g_khr_test_ctx.stats != nullptr) { \
        g_khr_test_ctx.stats->assertions_checked++; \
    } \
    if ((ptr) == nullptr) { \
        khr_test_record_failure(&g_khr_test_ctx, __FILE__, __LINE__, #ptr " != nullptr", (msg)); \
        return false; \
    } \
} while (0)

#define TEST_ASSERT_NULL(ptr, msg) do { \
    if (g_khr_test_ctx.stats != nullptr) { \
        g_khr_test_ctx.stats->assertions_checked++; \
    } \
    if ((ptr) != nullptr) { \
        khr_test_record_failure(&g_khr_test_ctx, __FILE__, __LINE__, #ptr " == nullptr", (msg)); \
        return false; \
    } \
} while (0)

#define TEST_ASSERT_LT(a, b, msg) do { \
    if (g_khr_test_ctx.stats != nullptr) { \
        g_khr_test_ctx.stats->assertions_checked++; \
    } \
    auto _a = (a); \
    auto _b = (b); \
    if (!(_a < _b)) { \
        khr_test_record_failure(&g_khr_test_ctx, __FILE__, __LINE__, #a " < " #b, (msg)); \
        return false; \
    } \
} while (0)

#define TEST_ASSERT_LE(a, b, msg) do { \
    if (g_khr_test_ctx.stats != nullptr) { \
        g_khr_test_ctx.stats->assertions_checked++; \
    } \
    auto _a = (a); \
    auto _b = (b); \
    if (!(_a <= _b)) { \
        khr_test_record_failure(&g_khr_test_ctx, __FILE__, __LINE__, #a " <= " #b, (msg)); \
        return false; \
    } \
} while (0)

#define TEST_ASSERT_GT(a, b, msg) do { \
    if (g_khr_test_ctx.stats != nullptr) { \
        g_khr_test_ctx.stats->assertions_checked++; \
    } \
    auto _a = (a); \
    auto _b = (b); \
    if (!(_a > _b)) { \
        khr_test_record_failure(&g_khr_test_ctx, __FILE__, __LINE__, #a " > " #b, (msg)); \
        return false; \
    } \
} while (0)

#define TEST_ASSERT_GE(a, b, msg) do { \
    if (g_khr_test_ctx.stats != nullptr) { \
        g_khr_test_ctx.stats->assertions_checked++; \
    } \
    auto _a = (a); \
    auto _b = (b); \
    if (!(_a >= _b)) { \
        khr_test_record_failure(&g_khr_test_ctx, __FILE__, __LINE__, #a " >= " #b, (msg)); \
        return false; \
    } \
} while (0)

#define TEST_ASSERT_MEM_EQ(p1, p2, len, msg) do { \
    if (g_khr_test_ctx.stats != nullptr) { \
        g_khr_test_ctx.stats->assertions_checked++; \
    } \
    if (memcmp((p1), (p2), (len)) != 0) { \
        khr_test_record_failure(&g_khr_test_ctx, __FILE__, __LINE__, "memcmp(" #p1 ", " #p2 ", " #len ") == 0", (msg)); \
        return false; \
    } \
} while (0)

static inline void khr_test_init_suite(test_suite_stats_t* stats) {
    *stats = (test_suite_stats_t){};
    stats->start_time_ns = khr_test_now_ns();
    stats->fail_log_file = nullptr;
    remove(FAILURES_LOG_PATH);
}

static inline void khr_test_run_internal(test_suite_stats_t* stats,
                                         const char* test_name,
                                         bool (*test_fn)(void)) {
    stats->total++;
    g_khr_test_ctx = (test_run_context_t){
        .current_test_name = test_name,
        .stats = stats,
        .failed = false
    };

    printf("  " KHR_CLR_CYAN "▶" KHR_CLR_RESET " %-42s ", test_name);
    fflush(stdout);

    uint64_t t0 = khr_test_now_ns();
    bool ok = test_fn();
    uint64_t t1 = khr_test_now_ns();
    double duration_ms = (double)(t1 - t0) / 1'000'000.0;

    if (ok && !g_khr_test_ctx.failed) {
        printf(KHR_CLR_GREEN "✔ [ PASS ]" KHR_CLR_RESET "  " KHR_CLR_DIM "(%6.3f ms)" KHR_CLR_RESET "\n", duration_ms);
        stats->passed++;
    } else {
        printf(KHR_CLR_RED KHR_CLR_BOLD "✖ [ FAIL ]" KHR_CLR_RESET "  " KHR_CLR_DIM "(%6.3f ms)" KHR_CLR_RESET "\n", duration_ms);
        stats->failed++;
    }
}

#define RUN_TEST(stats, test_fn) \
    khr_test_run_internal((stats), #test_fn, (test_fn))

static inline void khr_test_print_progress_bar(uint32_t passed, uint32_t total, uint32_t bar_width) {
    if (total == 0) return;
    float ratio = (float)passed / (float)total;
    uint32_t filled = (uint32_t)(ratio * (float)bar_width);

    printf("  " KHR_CLR_BOLD "Pass Rate:" KHR_CLR_RESET " [");
    for (uint32_t i = 0; i < bar_width; i++) {
        if (i < filled) {
            printf(KHR_CLR_GREEN "█" KHR_CLR_RESET);
        } else {
            printf(KHR_CLR_DIM "░" KHR_CLR_RESET);
        }
    }
    printf("] " KHR_CLR_BOLD "%5.1f%%" KHR_CLR_RESET "\n", ratio * 100.0f);
}

static inline int khr_test_finish_suite(test_suite_stats_t* stats) {
    stats->end_time_ns = khr_test_now_ns();
    double total_ms = (double)(stats->end_time_ns - stats->start_time_ns) / 1'000'000.0;

    printf("\n");
    printf(KHR_CLR_BOLD "────────────────────────────────────────────────────────────────────────" KHR_CLR_RESET "\n");

    if (stats->failed > 0) {
        printf("\n" KHR_BG_RED_BOLD " ✖ TEST SUITE FAILED (%u/%u failed) " KHR_CLR_RESET "\n\n",
               stats->failed, stats->total);

        /* Dedicated FAILURE SUMMARY BLOCK */
        printf(KHR_CLR_RED KHR_CLR_BOLD "╔════════════════════════════════════════════════════════════════════════╗\n" KHR_CLR_RESET);
        printf(KHR_CLR_RED KHR_CLR_BOLD "║                        FAILURE SUMMARY BREAKDOWN                       ║\n" KHR_CLR_RESET);
        printf(KHR_CLR_RED KHR_CLR_BOLD "╚════════════════════════════════════════════════════════════════════════╝\n" KHR_CLR_RESET);

        for (uint32_t i = 0; i < stats->failure_count; i++) {
            auto fail = &stats->failures[i];
            printf("  " KHR_CLR_RED KHR_CLR_BOLD "#%u." KHR_CLR_RESET " " KHR_CLR_BOLD "%s" KHR_CLR_RESET "\n",
                   i + 1, fail->test_name);
            printf("     " KHR_CLR_DIM "File:" KHR_CLR_RESET "   %s:%d\n", fail->file, fail->line);
            printf("     " KHR_CLR_DIM "Assert:" KHR_CLR_RESET " " KHR_CLR_YELLOW "%s" KHR_CLR_RESET "\n", fail->expr);
            printf("     " KHR_CLR_DIM "Error:" KHR_CLR_RESET "  " KHR_CLR_RED "%s" KHR_CLR_RESET "\n\n", fail->message);
        }

        printf("  " KHR_CLR_YELLOW "⚠ Complete failure log saved to: " KHR_CLR_RESET "%s\n\n", FAILURES_LOG_PATH);
    } else {
        printf("\n" KHR_BG_GREEN_BOLD " ✔ ALL TESTS PASSED SUCCESSFULLY " KHR_CLR_RESET "\n\n");
    }

    khr_test_print_progress_bar(stats->passed, stats->total, 36);

    printf("  " KHR_CLR_BOLD "Summary:" KHR_CLR_RESET "   "
           KHR_CLR_GREEN "%u Passed" KHR_CLR_RESET " | "
           "%s%u Failed" KHR_CLR_RESET " | "
           "%u Total" KHR_CLR_DIM " (%u assertions checked)" KHR_CLR_RESET "\n",
           stats->passed,
           (stats->failed > 0) ? KHR_CLR_RED : KHR_CLR_DIM,
           stats->failed,
           stats->total,
           stats->assertions_checked);
    printf("  " KHR_CLR_BOLD "Duration:" KHR_CLR_RESET "  %.3f ms\n", total_ms);
    printf(KHR_CLR_BOLD "────────────────────────────────────────────────────────────────────────" KHR_CLR_RESET "\n\n");

    if (stats->fail_log_file != nullptr) {
        if (stats->failed == 0) {
            fprintf(stats->fail_log_file, "All %u tests passed with 0 errors in %.3f ms.\n",
                    stats->total, total_ms);
        } else {
            fprintf(stats->fail_log_file, "Total Failures: %u / %u (Duration: %.3f ms)\n",
                    stats->failed, stats->total, total_ms);
        }
        fclose(stats->fail_log_file);
        stats->fail_log_file = nullptr;
    }

    return (stats->failed == 0) ? 0 : 1;
}

#endif /* TEST_FRAMEWORK_H */
