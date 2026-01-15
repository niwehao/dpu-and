/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_DPU_COMPACT_H
#define _LINUX_DPU_COMPACT_H

#include <linux/mm.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/rmap.h>
#define DPU_COMPACT_REGION_SHIFT	21  /* 2MB regions */
#define DPU_COMPACT_REGION_SIZE		(1UL << DPU_COMPACT_REGION_SHIFT)
#define DPU_COMPACT_REGION_MASK		(~(DPU_COMPACT_REGION_SIZE - 1))//通过 & 掩码运算，低 21 位会被强制清零，结果就是该块的首地址

/* Maximum fragments per DPU operation */
#define DPU_MAX_FRAGMENTS		1024
enum dpu_compact_state {
	DPU_COMPACT_IDLE = 0,
	DPU_COMPACT_COLLECTING,	/* Collecting fragment info */
	DPU_COMPACT_MOVING,	/* DPU is moving pages */
	DPU_COMPACT_UPDATING,	/* Updating page tables */
	DPU_COMPACT_COMPLETE,
	DPU_COMPACT_FAILED,
};
struct dpu_fragment {
	struct list_head list;
	struct page *page;		/* The page being moved */
	unsigned long old_pfn;		/* Original PFN */
	unsigned long new_pfn;		/* New PFN after compaction */
	unsigned long vaddr;		/* Virtual address (if mapped) */
	struct mm_struct *mm;		/* Owner mm (for mapped pages) */
	pte_t *ptep;			/* Page table entry pointer */
	spinlock_t *ptl;		/* Page table lock */
	bool is_mapped;			/* Whether page is mapped */
	bool is_anon;			/* Anonymous page */
	bool is_dirty;			/* Dirty page */
	bool is_frag;         /* Is fragment from buddy allocator */
};
/* DPU compaction region control structure */
struct dpu_compact_region {
	unsigned long base_pfn;		/* Region base PFN */
	unsigned long region_size;	/* Region size in pages */

	/* Fragment tracking */
	struct list_head fragments;	/* List of dpu_fragment */
	unsigned int nr_fragments;	/* Number of fragments */
    unsigned int nr_buddy;

	/* DPU communication */
	uint64_t *dpu_addr_list;	/* Physical addresses for DPU */ //对应dpu内存，保存碎片的物理地址集合
	void *dpu_buffer;		/* DMA buffer for DPU *///对应DPU上的内存，此处只做模拟

	/* State management */
	enum dpu_compact_state state;
	spinlock_t lock;

	/* Statistics */
	unsigned long total_moved;
	unsigned long time_start;
	unsigned long time_end;
};
struct dpu_compact_region *dpu_compact_region_create(unsigned long base_pfn,
						     unsigned long size);
int dpu_compact_execute(struct dpu_compact_region *region);
int dpu_compact_memory(struct zone *zone, unsigned int order);
int dpu_compact_isolate_pages(struct zone *zone,
			      struct dpu_compact_region *region,
			      unsigned long start_pfn,
			      unsigned long end_pfn);
int dpu_hw_compact_execute(struct dpu_compact_region *region);
int dpu_compact_update_mappings(struct dpu_compact_region *region);
#ifdef CONFIG_DPU_COMPACTION
static inline bool dpu_compact_available(void)
{
	return sysctl_dpu_compact_enabled;
}
#else
static inline bool dpu_compact_available(void)
{
	return false;
}
#endif