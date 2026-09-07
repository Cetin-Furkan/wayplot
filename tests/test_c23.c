#include "test_framework.h"
#include "engine.h"

[[nodiscard]]
bool test_c23_keywords(void) {
    bool flag = true;
    TEST_ASSERT(flag == true, "bool keyword must evaluate correctly");
    flag = false;
    TEST_ASSERT(flag == false, "bool false must evaluate correctly");

    struct cacheline_aligned_t {
        alignas(64) uint8_t data[64];
    };
    TEST_ASSERT(alignof(struct cacheline_aligned_t) == 64, "alignof(struct cacheline_aligned_t) must evaluate to 64");

    alignas(64) uint8_t cacheline_buffer[64];
    TEST_ASSERT(((uintptr_t)cacheline_buffer % 64) == 0, "alignas(64) must align memory to 64 bytes");

    void* ptr = nullptr;
    TEST_ASSERT(ptr == nullptr, "nullptr keyword must evaluate correctly");

    return true;
}

[[nodiscard]]
bool test_c23_constexpr_and_literals(void) {
    constexpr uint32_t bin_val = 0b1011'0001;
    TEST_ASSERT(bin_val == 177, "Binary literal and digit separator must evaluate to 177");

    constexpr size_t page_sz = 4'096;
    TEST_ASSERT(page_sz == 4096, "Digit separator in 4'096 must equal 4096");
    TEST_ASSERT(ENGINE_PAGE_SIZE == 4096, "ENGINE_PAGE_SIZE constexpr must equal 4096");
    TEST_ASSERT(ENGINE_CACHE_LINE_SIZE == 64, "ENGINE_CACHE_LINE_SIZE constexpr must equal 64");

    return true;
}

[[nodiscard]]
bool test_gnu_source_and_engine_banner(void) {
#ifndef _GNU_SOURCE
    TEST_ASSERT(false, "_GNU_SOURCE must be defined by build flags");
#endif

    const char* banner = engine_get_banner();
    TEST_ASSERT(banner != nullptr, "Engine banner must not be nullptr");
    return true;
}
