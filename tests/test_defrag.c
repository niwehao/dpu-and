#include "dpu_defrag.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>

/* ANSI color codes for better output */
#define COLOR_GREEN   "\x1b[32m"
#define COLOR_RED     "\x1b[31m"
#define COLOR_YELLOW  "\x1b[33m"
#define COLOR_BLUE    "\x1b[34m"
#define COLOR_RESET   "\x1b[0m"

/* Test result tracking */
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(condition, message) do { \
    if (condition) { \
        printf(COLOR_GREEN "  ✓ PASS: %s" COLOR_RESET "\n", message); \
        tests_passed++; \
    } else { \
        printf(COLOR_RED "  ✗ FAIL: %s" COLOR_RESET "\n", message); \
        tests_failed++; \
    } \
} while(0)

#define TEST_START(name) \
    printf(COLOR_BLUE "\n▶ Running test: %s" COLOR_RESET "\n", name)

#define TEST_END() \
    printf("\n")

/**
 * Test 1: Basic defragmentation with alternating fragments and free pages
 *
 * Initial layout: F1 Free1 F2 Free2 F3
 * Expected result: F1 F2 F3 Free1 Free2 (fragments compacted at start)
 */
void test_basic_defragmentation(void)
{
    struct dpu_region region;
    struct dpu_fragment *frag;
    struct list_head *pos;
    int idx;

    TEST_START("Basic Defragmentation");

    dpu_region_init(&region, 1000, 1100);

    /* Create alternating pattern: F Free F Free F */
    dpu_fragment_add(&region, 1000, true);   /* Fragment */
    dpu_fragment_add(&region, 1001, false);  /* Free */
    dpu_fragment_add(&region, 1002, true);   /* Fragment */
    dpu_fragment_add(&region, 1003, false);  /* Free */
    dpu_fragment_add(&region, 1004, true);   /* Fragment */

    printf("  Initial configuration: F Free F Free F\n");
    dpu_region_stats(&region);

    /* Run defragmentation */
    int result = dpu_defragment_region(&region);
    TEST_ASSERT(result == 0, "Defragmentation completed successfully");

    dpu_print_fragment_mapping(&region);

    /* Verify: fragments should be at PFN 1000, 1001, 1002 */
    /* Free pages should be at PFN 1003, 1004 */
    idx = 0;
    list_for_each(pos, &region.fragments) {
        frag = list_entry(pos, struct dpu_fragment, list);

        if (idx == 0) {
            TEST_ASSERT(frag->is_frag && frag->new_pfn == 1000,
                       "First fragment at correct position (1000)");
        } else if (idx == 1) {
            TEST_ASSERT(!frag->is_frag && frag->new_pfn == 1003,
                       "First free page moved to position 1003");
        } else if (idx == 2) {
            TEST_ASSERT(frag->is_frag && frag->new_pfn == 1001,
                       "Second fragment at correct position (1001)");
        } else if (idx == 3) {
            TEST_ASSERT(!frag->is_frag && frag->new_pfn == 1004,
                       "Second free page moved to position 1004");
        } else if (idx == 4) {
            TEST_ASSERT(frag->is_frag && frag->new_pfn == 1002,
                       "Third fragment at correct position (1002)");
        }
        idx++;
    }

    dpu_region_clear(&region);
    TEST_END();
}

/**
 * Test 2: Fragments at the end, free pages at the start
 *
 * Initial layout: Free Free Free F F F
 * Expected result: F F F Free Free Free
 */
void test_reversed_layout(void)
{
    struct dpu_region region;
    struct dpu_fragment *frag;
    struct list_head *pos;
    int frag_count = 0;

    TEST_START("Reversed Layout (Free pages first, then fragments)");

    dpu_region_init(&region, 2000, 2100);

    /* Create reversed pattern: Free Free Free F F F */
    dpu_fragment_add(&region, 2000, false);  /* Free */
    dpu_fragment_add(&region, 2001, false);  /* Free */
    dpu_fragment_add(&region, 2002, false);  /* Free */
    dpu_fragment_add(&region, 2003, true);   /* Fragment */
    dpu_fragment_add(&region, 2004, true);   /* Fragment */
    dpu_fragment_add(&region, 2005, true);   /* Fragment */

    printf("  Initial configuration: Free Free Free F F F\n");
    dpu_region_stats(&region);

    int result = dpu_defragment_region(&region);
    TEST_ASSERT(result == 0, "Defragmentation completed successfully");

    dpu_print_fragment_mapping(&region);

    /* Verify: all fragments should be compacted at the beginning */
    list_for_each(pos, &region.fragments) {
        frag = list_entry(pos, struct dpu_fragment, list);
        if (frag->is_frag) {
            frag_count++;
            TEST_ASSERT(frag->new_pfn >= 2000 && frag->new_pfn <= 2002,
                       "Fragment compacted to beginning positions");
        } else {
            TEST_ASSERT(frag->new_pfn >= 2003 && frag->new_pfn <= 2005,
                       "Free page moved to end positions");
        }
    }

    TEST_ASSERT(frag_count == 3, "All 3 fragments accounted for");

    dpu_region_clear(&region);
    TEST_END();
}

/**
 * Test 3: Already defragmented (no changes needed)
 *
 * Initial layout: F F F Free Free Free
 * Expected result: F F F Free Free Free (no migrations)
 */
void test_already_defragmented(void)
{
    struct dpu_region region;
    struct dpu_fragment *frag;
    struct list_head *pos;
    int migrations = 0;

    TEST_START("Already Defragmented");

    dpu_region_init(&region, 3000, 3100);

    /* Create already optimal pattern: F F F Free Free Free */
    dpu_fragment_add(&region, 3000, true);   /* Fragment */
    dpu_fragment_add(&region, 3001, true);   /* Fragment */
    dpu_fragment_add(&region, 3002, true);   /* Fragment */
    dpu_fragment_add(&region, 3003, false);  /* Free */
    dpu_fragment_add(&region, 3004, false);  /* Free */
    dpu_fragment_add(&region, 3005, false);  /* Free */

    printf("  Initial configuration: F F F Free Free Free (already optimal)\n");
    dpu_region_stats(&region);

    int result = dpu_defragment_region(&region);
    TEST_ASSERT(result == 0, "Defragmentation completed successfully");

    dpu_print_fragment_mapping(&region);

    /* Verify: no migrations should be needed */
    list_for_each(pos, &region.fragments) {
        frag = list_entry(pos, struct dpu_fragment, list);
        if (frag->old_pfn != frag->new_pfn) {
            migrations++;
        }
    }

    TEST_ASSERT(migrations == 0, "No migrations needed (already optimal)");

    dpu_region_clear(&region);
    TEST_END();
}

/**
 * Test 4: Complex fragmentation pattern
 *
 * Initial layout: Free F Free F Free F F Free F Free
 * Tests the algorithm's ability to handle complex patterns
 */
void test_complex_fragmentation(void)
{
    struct dpu_region region;
    struct dpu_fragment *frag;
    struct list_head *pos;
    int frag_in_correct_range = 0;
    int free_in_correct_range = 0;

    TEST_START("Complex Fragmentation Pattern");

    dpu_region_init(&region, 4000, 4100);

    /* Create complex pattern: Free F Free F Free F F Free F Free */
    dpu_fragment_add(&region, 4000, false);  /* Free */
    dpu_fragment_add(&region, 4001, true);   /* Fragment */
    dpu_fragment_add(&region, 4002, false);  /* Free */
    dpu_fragment_add(&region, 4003, true);   /* Fragment */
    dpu_fragment_add(&region, 4004, false);  /* Free */
    dpu_fragment_add(&region, 4005, true);   /* Fragment */
    dpu_fragment_add(&region, 4006, true);   /* Fragment */
    dpu_fragment_add(&region, 4007, false);  /* Free */
    dpu_fragment_add(&region, 4008, true);   /* Fragment */
    dpu_fragment_add(&region, 4009, false);  /* Free */

    printf("  Initial configuration: Free F Free F Free F F Free F Free\n");
    dpu_region_stats(&region);

    int result = dpu_defragment_region(&region);
    TEST_ASSERT(result == 0, "Defragmentation completed successfully");

    dpu_print_fragment_mapping(&region);

    /* Verify: 5 fragments should be at PFNs 4000-4004, 5 free pages at 4005-4009 */
    list_for_each(pos, &region.fragments) {
        frag = list_entry(pos, struct dpu_fragment, list);
        if (frag->is_frag) {
            if (frag->new_pfn >= 4000 && frag->new_pfn <= 4004) {
                frag_in_correct_range++;
            }
        } else {
            if (frag->new_pfn >= 4005 && frag->new_pfn <= 4009) {
                free_in_correct_range++;
            }
        }
    }

    TEST_ASSERT(frag_in_correct_range == 5,
               "All 5 fragments compacted to positions 4000-4004");
    TEST_ASSERT(free_in_correct_range == 5,
               "All 5 free pages moved to positions 4005-4009");

    dpu_region_clear(&region);
    TEST_END();
}

/**
 * Test 5: Single fragment
 */
void test_single_fragment(void)
{
    struct dpu_region region;

    TEST_START("Single Fragment");

    dpu_region_init(&region, 5000, 5100);
    dpu_fragment_add(&region, 5000, true);

    printf("  Initial configuration: Single fragment\n");
    dpu_region_stats(&region);

    int result = dpu_defragment_region(&region);
    TEST_ASSERT(result == 0, "Defragmentation completed successfully");

    dpu_print_fragment_mapping(&region);

    dpu_region_clear(&region);
    TEST_END();
}

/**
 * Test 6: Large-scale test with many fragments
 */
void test_large_scale(void)
{
    struct dpu_region region;
    struct dpu_fragment *frag;
    struct list_head *pos;
    int i;
    int frag_in_correct_range = 0;
    int free_in_correct_range = 0;
    int total_frags = 0;
    int total_free = 0;

    TEST_START("Large Scale (100 pages, alternating pattern)");

    dpu_region_init(&region, 10000, 10200);

    /* Create alternating pattern for 100 pages */
    for (i = 0; i < 100; i++) {
        dpu_fragment_add(&region, 10000 + i, (i % 2 == 0));  /* Even: Fragment, Odd: Free */
    }

    printf("  Initial configuration: 100 pages with alternating F/Free pattern\n");
    dpu_region_stats(&region);

    int result = dpu_defragment_region(&region);
    TEST_ASSERT(result == 0, "Defragmentation completed successfully");

    printf("  Note: Mapping output suppressed for large scale test\n");
    // dpu_print_fragment_mapping(&region);  /* Commented out for brevity */

    /* Verify: 50 fragments at start, 50 free pages at end */
    list_for_each(pos, &region.fragments) {
        frag = list_entry(pos, struct dpu_fragment, list);
        if (frag->is_frag) {
            total_frags++;
            if (frag->new_pfn >= 10000 && frag->new_pfn <= 10049) {
                frag_in_correct_range++;
            }
        } else {
            total_free++;
            if (frag->new_pfn >= 10050 && frag->new_pfn <= 10099) {
                free_in_correct_range++;
            }
        }
    }

    TEST_ASSERT(total_frags == 50 && frag_in_correct_range == 50,
               "All 50 fragments compacted to positions 10000-10049");
    TEST_ASSERT(total_free == 50 && free_in_correct_range == 50,
               "All 50 free pages moved to positions 10050-10099");

    dpu_region_clear(&region);
    TEST_END();
}

/**
 * Main test runner
 */
int main(int argc, char *argv[])
{
    (void)argc;  /* Unused */
    (void)argv;  /* Unused */

    printf(COLOR_YELLOW);
    printf("╔════════════════════════════════════════════════════╗\n");
    printf("║   DPU Memory Defragmentation Test Suite          ║\n");
    printf("║   Optimized Algorithm with O(n) Complexity        ║\n");
    printf("╚════════════════════════════════════════════════════╝\n");
    printf(COLOR_RESET);

    /* Run all tests */
    test_basic_defragmentation();
    test_reversed_layout();
    test_already_defragmented();
    test_complex_fragmentation();
    test_single_fragment();
    test_large_scale();

    /* Print summary */
    printf(COLOR_YELLOW);
    printf("\n╔════════════════════════════════════════════════════╗\n");
    printf("║                  TEST SUMMARY                      ║\n");
    printf("╚════════════════════════════════════════════════════╝\n");
    printf(COLOR_RESET);

    printf("Total tests run: %d\n", tests_passed + tests_failed);
    printf(COLOR_GREEN "Tests passed:    %d" COLOR_RESET "\n", tests_passed);
    printf(COLOR_RED "Tests failed:    %d" COLOR_RESET "\n", tests_failed);

    if (tests_failed == 0) {
        printf(COLOR_GREEN "\n✓ All tests passed!\n" COLOR_RESET);
        return 0;
    } else {
        printf(COLOR_RED "\n✗ Some tests failed.\n" COLOR_RESET);
        return 1;
    }
}
