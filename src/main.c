#include "dpu_defrag.h"
#include <stdio.h>
#include <stdlib.h>

/**
 * Demo program showing DPU defragmentation in action
 */
int main(int argc, char *argv[])
{
    struct dpu_region region;

    (void)argc;  /* Unused */
    (void)argv;  /* Unused */

    printf("========================================\n");
    printf("DPU Memory Defragmentation Demo\n");
    printf("========================================\n\n");

    /* Initialize a memory region */
    dpu_region_init(&region, 0x1000, 0x2000);

    printf("Creating a fragmented memory layout...\n");
    printf("Pattern: Free, Fragment, Free, Fragment, Fragment, Free, Fragment\n\n");

    /* Add fragments and free pages in a fragmented pattern */
    dpu_fragment_add(&region, 0x1000, false);  /* Free page */
    dpu_fragment_add(&region, 0x1001, true);   /* Fragment */
    dpu_fragment_add(&region, 0x1002, false);  /* Free page */
    dpu_fragment_add(&region, 0x1003, true);   /* Fragment */
    dpu_fragment_add(&region, 0x1004, true);   /* Fragment */
    dpu_fragment_add(&region, 0x1005, false);  /* Free page */
    dpu_fragment_add(&region, 0x1006, true);   /* Fragment */

    /* Show initial state */
    dpu_region_stats(&region);

    printf("BEFORE defragmentation:\n");
    dpu_print_fragment_mapping(&region);

    /* Perform defragmentation */
    printf("Running defragmentation algorithm...\n");
    int result = dpu_defragment_region(&region);

    if (result != 0) {
        fprintf(stderr, "Error: Defragmentation failed\n");
        dpu_region_clear(&region);
        return 1;
    }

    printf("\nAFTER defragmentation:\n");
    dpu_print_fragment_mapping(&region);

    printf("========================================\n");
    printf("Demo completed successfully!\n");
    printf("========================================\n");

    /* Clean up */
    dpu_region_clear(&region);

    return 0;
}
