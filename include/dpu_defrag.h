#ifndef _DPU_DEFRAG_H
#define _DPU_DEFRAG_H

#include "list.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * DPU Memory Fragment Defragmentation System
 *
 * This system manages memory fragmentation by tracking fragments and
 * computing optimal page remapping to reduce fragmentation.
 */

/* Page frame number type */
typedef uint64_t pfn_t;

/**
 * struct dpu_fragment - Represents a memory fragment or free page
 * @list: List node for linking fragments in a region
 * @old_pfn: Original page frame number
 * @new_pfn: Target page frame number after defragmentation
 * @is_frag: true if this is an actual fragment (in use), false if free page
 * @size: Size in pages (for future extension)
 */
struct dpu_fragment {
    struct list_head list;
    pfn_t old_pfn;
    pfn_t new_pfn;
    bool is_frag;
    uint32_t size;
};

/**
 * struct dpu_region - Memory region containing fragments
 * @fragments: Head of the fragment list
 * @total_count: Total number of fragments + free pages in list
 * @frag_count: Number of actual fragments (is_frag=true)
 * @free_count: Number of free pages (is_frag=false)
 * @start_pfn: Starting PFN of this region
 * @end_pfn: Ending PFN of this region
 */
struct dpu_region {
    struct list_head fragments;
    uint32_t total_count;
    uint32_t frag_count;
    uint32_t free_count;
    pfn_t start_pfn;
    pfn_t end_pfn;
};

/**
 * dpu_region_init - Initialize a memory region
 * @region: Region to initialize
 * @start_pfn: Starting page frame number
 * @end_pfn: Ending page frame number
 */
void dpu_region_init(struct dpu_region *region, pfn_t start_pfn, pfn_t end_pfn);

/**
 * dpu_fragment_add - Add a fragment to a region
 * @region: Target region
 * @pfn: Page frame number
 * @is_frag: true for actual fragment, false for free page
 *
 * Returns: Pointer to created fragment, or NULL on failure
 */
struct dpu_fragment *dpu_fragment_add(struct dpu_region *region,
                                      pfn_t pfn,
                                      bool is_frag);

/**
 * dpu_defragment_region - Compute defragmentation mapping
 * @region: Region to defragment
 *
 * This function computes the optimal remapping of fragments to reduce
 * fragmentation. After calling this function, each fragment's new_pfn
 * field will contain the target PFN.
 *
 * Algorithm:
 * - Fragments (is_frag=true) are compacted to the beginning
 * - Free pages (is_frag=false) are moved to the end
 * - Minimizes the number of page migrations needed
 *
 * Returns: 0 on success, negative error code on failure
 */
int dpu_defragment_region(struct dpu_region *region);

/**
 * dpu_region_clear - Clear all fragments from a region
 * @region: Region to clear
 */
void dpu_region_clear(struct dpu_region *region);

/**
 * dpu_region_stats - Print region statistics
 * @region: Region to print stats for
 */
void dpu_region_stats(struct dpu_region *region);

/**
 * dpu_print_fragment_mapping - Print fragment remapping details
 * @region: Region to print mappings for
 */
void dpu_print_fragment_mapping(struct dpu_region *region);

#endif /* _DPU_DEFRAG_H */
