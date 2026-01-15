#include "dpu_defrag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void dpu_region_init(struct dpu_region *region, pfn_t start_pfn, pfn_t end_pfn)
{
    if (!region) {
        return;
    }

    INIT_LIST_HEAD(&region->fragments);
    region->total_count = 0;
    region->frag_count = 0;
    region->free_count = 0;
    region->start_pfn = start_pfn;
    region->end_pfn = end_pfn;
}

struct dpu_fragment *dpu_fragment_add(struct dpu_region *region,
                                      pfn_t pfn,
                                      bool is_frag)
{
    struct dpu_fragment *frag;

    if (!region) {
        return NULL;
    }

    frag = (struct dpu_fragment *)malloc(sizeof(struct dpu_fragment));
    if (!frag) {
        fprintf(stderr, "Error: Failed to allocate fragment\n");
        return NULL;
    }

    frag->old_pfn = pfn;
    frag->new_pfn = pfn;  /* Initially no remapping */
    frag->is_frag = is_frag;
    frag->size = 1;  /* Single page for now */

    list_add_tail(&frag->list, &region->fragments);
    region->total_count++;

    if (is_frag) {
        region->frag_count++;
    } else {
        region->free_count++;
    }

    return frag;
}

/**
 * dpu_defragment_region - Optimized defragmentation algorithm
 *
 * OPTIMIZED ALGORITHM:
 * Instead of nested loops (O(n²)), we use a single-pass algorithm with
 * temporary arrays for O(n) time complexity.
 *
 * Strategy:
 * 1. Collect all fragments and free pages in separate arrays (one pass)
 * 2. Assign new PFNs: fragments get low PFNs, free pages get high PFNs
 * 3. This ensures fragments are compacted at the beginning of the region
 *
 * Key improvements over original code:
 * - No nested loops: O(n) instead of O(n²)
 * - Correctly handles fragments that move (their old positions become free)
 * - Uses array-based mapping for clarity and efficiency
 * - Properly accounts for all fragment movements
 */
int dpu_defragment_region(struct dpu_region *region)
{
    struct dpu_fragment **frags = NULL;
    struct dpu_fragment **free_pages = NULL;
    struct dpu_fragment *frag;
    struct list_head *pos;
    uint32_t frag_idx = 0, free_idx = 0;
    uint32_t i;
    pfn_t next_pfn;

    if (!region || list_empty(&region->fragments)) {
        return -1;
    }

    /* Allocate temporary arrays to hold fragment and free page pointers */
    if (region->frag_count > 0) {
        frags = (struct dpu_fragment **)malloc(
            sizeof(struct dpu_fragment *) * region->frag_count);
        if (!frags) {
            fprintf(stderr, "Error: Failed to allocate fragment array\n");
            return -1;
        }
    }

    if (region->free_count > 0) {
        free_pages = (struct dpu_fragment **)malloc(
            sizeof(struct dpu_fragment *) * region->free_count);
        if (!free_pages) {
            fprintf(stderr, "Error: Failed to allocate free page array\n");
            free(frags);
            return -1;
        }
    }

    /**
     * STEP 1: Single pass to separate fragments from free pages
     * This replaces the original nested while loops
     */
    list_for_each(pos, &region->fragments) {
        frag = list_entry(pos, struct dpu_fragment, list);

        if (frag->is_frag) {
            frags[frag_idx++] = frag;
        } else {
            free_pages[free_idx++] = frag;
        }
    }

    /**
     * STEP 2: Assign new PFNs for optimal compaction
     *
     * Strategy:
     * - All fragments get consecutive PFNs starting from region start
     * - All free pages get consecutive PFNs after fragments
     *
     * This ensures:
     * - Fragments are compacted at the beginning (no gaps)
     * - Free pages are pushed to the end
     * - Minimal page migrations needed
     */
    next_pfn = region->start_pfn;

    /* Assign new PFNs to fragments (compact them at the beginning) */
    for (i = 0; i < region->frag_count; i++) {
        frags[i]->new_pfn = next_pfn++;
    }

    /* Assign new PFNs to free pages (push them to the end) */
    for (i = 0; i < region->free_count; i++) {
        free_pages[i]->new_pfn = next_pfn++;
    }

    /* Clean up temporary arrays */
    free(frags);
    free(free_pages);

    return 0;
}

void dpu_region_clear(struct dpu_region *region)
{
    struct dpu_fragment *frag, *tmp;

    if (!region) {
        return;
    }

    list_for_each_entry_safe(frag, tmp, &region->fragments, list) {
        list_del(&frag->list);
        free(frag);
    }

    region->total_count = 0;
    region->frag_count = 0;
    region->free_count = 0;
}

void dpu_region_stats(struct dpu_region *region)
{
    if (!region) {
        return;
    }

    printf("\n=== DPU Region Statistics ===\n");
    printf("Region PFN range: %lu - %lu\n", region->start_pfn, region->end_pfn);
    printf("Total entries:    %u\n", region->total_count);
    printf("Fragments:        %u\n", region->frag_count);
    printf("Free pages:       %u\n", region->free_count);
    printf("============================\n\n");
}

void dpu_print_fragment_mapping(struct dpu_region *region)
{
    struct dpu_fragment *frag;
    struct list_head *pos;
    uint32_t migrations = 0;
    uint32_t idx = 0;

    if (!region) {
        return;
    }

    printf("\n=== Fragment Mapping ===\n");
    printf("%-5s %-10s %-10s %-10s %s\n",
           "Index", "Old PFN", "New PFN", "Type", "Status");
    printf("-------------------------------------------------------\n");

    list_for_each(pos, &region->fragments) {
        frag = list_entry(pos, struct dpu_fragment, list);

        printf("%-5u %-10lu %-10lu %-10s %s\n",
               idx++,
               frag->old_pfn,
               frag->new_pfn,
               frag->is_frag ? "Fragment" : "Free",
               (frag->old_pfn != frag->new_pfn) ? "MIGRATE" : "STAY");

        if (frag->old_pfn != frag->new_pfn) {
            migrations++;
        }
    }

    printf("-------------------------------------------------------\n");
    printf("Total migrations needed: %u\n", migrations);
    printf("========================\n\n");
}
