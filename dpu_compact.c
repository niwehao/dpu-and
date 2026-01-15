#include <linux/mm.h>
#include <linux/dpu_compact.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/rmap.h>
#include <linux/swap.h>
#include <linux/migrate.h>
#include <linux/pagevec.h>
#include <linux/mm_inline.h>
#include <linux/hugetlb.h>
#include <linux/page-isolation.h>
#include <linux/ktime.h>
#include <linux/dma-mapping.h>
#include <asm/tlbflush.h>
#include "internal.h"
struct dpu_compact_region *dpu_compact_region_create(unsigned long base_pfn,//base_pfn：区域的起始页帧号
						     unsigned long size)
{
	struct dpu_compact_region *region;

	region = kzalloc(sizeof(*region), GFP_KERNEL);//kzalloc：分配内存后，自动调用 memset 将这块内存全部填充为 0。
	if (!region)//GFP_KERNEL这是一次常规的内核内存申请，如果不立即够用，可以等待（休眠）
		return NULL;

	region->base_pfn = base_pfn;
	region->region_size = size;
	region->state = DPU_COMPACT_IDLE;

	INIT_LIST_HEAD(&region->fragments);//INIT_LIST_HEAD 会将 fragments 的 next 指针和 prev 指针都指向它自己
	spin_lock_init(&region->lock);//初始化一个自旋锁（Spinlock）

	/* Allocate DPU address list buffer */
	region->dpu_addr_list = kmalloc(DPU_MAX_FRAGMENTS * sizeof(uint64_t),
					GFP_KERNEL);
	if (!region->dpu_addr_list) {
		kfree(region);
		return NULL;
	}
    //TODO 实际不需要分配内存，直接把对应的物理地址发射给DPU即可
	/* Allocate DMA-capable buffer for DPU communication */
	region->dpu_buffer = kmalloc(DPU_COMPACT_REGION_SIZE, GFP_KERNEL | GFP_DMA);
	if (!region->dpu_buffer) {
		kfree(region->dpu_addr_list);
		kfree(region);
		return NULL;
	}

	return region;
}
bool dpu_compact_page_suitable(struct page *page)
{
	/* Skip huge pages - handle them separately */
	if (PageHuge(page) || PageTransHuge(page))//排除大页，dpu针对的是细小碎片
		return false;

	/* Skip reserved and kernel pages */
	if (PageReserved(page) || PageKsm(page))//排除保留页和内核页
		return false;

	/* Skip pages in Buddy allocator (already free) */
	

	/* Skip pages being written back (async mode) */
	if (PageWriteback(page))//排除正在写回的页
		return false;

	/* Skip unevictable pages */
	if (PageUnevictable(page))//排除不可回收的页
		return false;

	/* Must be either LRU or movable */
	if (!PageLRU(page) && !__PageMovable(page))//排除不可移动页
		return false;

	return true;
}
int dpu_compact_add_fragment(struct dpu_compact_region *region,
			     struct page *page,
			     struct vm_area_struct *vma,
			     unsigned long vaddr, bool is_frag)
{
	struct dpu_fragment *frag;
	unsigned long flags;

	if (region->nr_fragments >= DPU_MAX_FRAGMENTS)
		return -ENOSPC;

	frag = kzalloc(sizeof(*frag), GFP_ATOMIC);
	if (!frag)
		return -ENOMEM;

	frag->page = page;
	frag->old_pfn = page_to_pfn(page);//page对应页号
	frag->is_anon = PageAnon(page);
	frag->is_dirty = PageDirty(page);
    frag->is_frag = is_frag;

	/* Check if page is mapped */
	if (vma && vaddr) {//vm默认为空
		frag->is_mapped = true;
		frag->vaddr = vaddr;
		frag->mm = vma->vm_mm;

		/* Get PTE and lock - will be used during update phase */
		frag->ptep = NULL; /* Will be resolved during update */
		frag->ptl = NULL;
	} else {
		frag->is_mapped = false;
		frag->mm = NULL;
	}

	spin_lock_irqsave(&region->lock, flags);
	list_add_tail(&frag->list, &region->fragments);//把碎片的链表节点添加到controller的碎片链表的尾部
	region->nr_fragments++;//增加碎片数量

	/* Store physical address for DPU */
	region->dpu_addr_list[region->nr_fragments - 1] =//储存碎片的位置
		(uint64_t)(page_to_pfn(page) << PAGE_SHIFT);

	spin_unlock_irqrestore(&region->lock, flags);

	/* Pin the page to prevent it from being freed */
	get_page(page);

	return 0;
}
static unsigned long dpu_compact_isolate_buddy_page(
    struct dpu_compact_region *region,
    struct page *page)
{
    struct list_head free_list;
    struct page *split_page, *tmp;
    unsigned long nr_isolated;
    unsigned long remaining_space;
    unsigned long isolated = 0;
    unsigned long taken = 0;
    unsigned int order;


    /* 计算剩余空间 */
    remaining_space = DPU_MAX_FRAGMENTS - region->nr_fragments;
    if (remaining_space == 0)
        return 0;

    /* 获取order */
    order = buddy_order(page);

    /* 隔离Buddy页面 */
    isolated = __isolate_free_page(page, order);
    if (!isolated)
        return 0;

    /* 初始化链表并添加页面 */
    INIT_LIST_HEAD(&free_list);
    set_page_private(page, order);
    list_add(&page->lru, &free_list);

    /* 拆分成单页 */
    split_map_pages(&free_list);

    /* 只取需要的数量，释放多余的 */
    list_for_each_entry_safe(split_page, tmp, &free_list, lru) {
        list_del(&split_page->lru);

        if (taken < remaining_space && 
            dpu_compact_add_fragment(region, split_page, NULL, 0, false) == 0) {
            taken++;
        } else {
            /* 超出需要或添加失败，释放页面 */
            __free_page(split_page);
        }
    }

    return taken;
}
int dpu_compact_isolate_pages(struct zone *zone,
			      struct dpu_compact_region *region,
			      unsigned long start_pfn,
			      unsigned long end_pfn)
{
	unsigned long pfn;
	struct page *page;
	int isolated = 0;

	for (pfn = start_pfn; pfn < end_pfn; pfn++) {
		/* Stop if we have enough fragments */
		if (region->nr_fragments >= DPU_MAX_FRAGMENTS)//从0开始慢慢添加
			break;

		/* Check if PFN is valid */
		if (!pfn_valid(pfn))
			continue;

		page = pfn_to_page(pfn);//获得该pfn对应的Page的结构体指针

		/* Check if zone matches */
		if (page_zone(page) != zone)
			continue;
        if (PageBuddy(page)){
            unsigned long taken;
            taken = dpu_compact_isolate_buddy_page(region, page);
            if(taken>0){
                isolated += taken-1;
            }
           
            continue;     
            
        }

		/* Check if page is suitable */
		if (!dpu_compact_page_suitable(page))
			continue;

		/* Try to isolate LRU page */
		if (PageLRU(page)) {//隔离（Isolation）与提取
			if (isolate_lru_page(page) != 0)
				continue;

			/* Add to region */
			if (dpu_compact_add_fragment(region, page, NULL, 0, true) == 0) {
				isolated++;
			} else {
				/* Failed to add, put back to LRU */
				putback_lru_page(page);
			}
		}
		/* Handle movable non-LRU pages */
        /**
         不考虑迁移movable页面，因为不同的页面类型可能需要不同的处理方式。
         */
		// else if (__PageMovable(page)) {
		// 	if (isolate_movable_page(page, ISOLATE_ASYNC_MIGRATE) != 0)
		// 		continue;

		// 	if (dpu_compact_add_fragment(region, page, NULL, 0) == 0) {
		// 		isolated++;
		// 	} else {
		// 		putback_movable_page(page);
		// 	}
		// }
	}

	return isolated;
}

int dpu_compact_execute(struct dpu_compact_region *region)
{

	int ret;
	ktime_t start_time, end_time;

	if (region->state != DPU_COMPACT_COLLECTING)
		return -EINVAL;

	if (region->nr_fragments == 0)
		return -EINVAL;

	region->state = DPU_COMPACT_MOVING;
	region->time_start = jiffies;
	start_time = ktime_get();

	/* Base physical address for the region */
	base_addr = region->base_pfn << PAGE_SHIFT;

	/*
	 * Calculate new PFNs for each fragment
	 * After DPU compaction, fragments will be laid out sequentially
	 * starting from base_pfn, maintaining their relative order
	 */
    struct dpu_fragment *front_frag, *back_frag;
	struct list_head *front_pos, *back_pos;

	front_pos = region->fragments.next;  /* 从头开始 */
	back_pos = region->fragments.prev;   /* 从尾开始 */

	while (front_pos != &region->fragments && 
	       back_pos != &region->fragments &&
	       front_pos != back_pos) {
		
		front_frag = list_entry(front_pos, struct dpu_fragment, list);
		back_frag = list_entry(back_pos, struct dpu_fragment, list);

		/* 前指针指向真碎片，它保持原位不动 */
		if (front_frag->is_frag) {  /* is_frag=true 是真碎片 */
			front_frag->new_pfn = front_frag->old_pfn;  /* 保持原PFN */
			front_pos = front_pos->next;
			continue;
		}

		/* 前指针指向空闲页，找后面的真碎片来填充这个位置 */
		if (!front_frag->is_frag) {  /* is_frag=false 是空闲页 */
			/* 后指针往前找真碎片 */
			while (back_pos != &region->fragments && 
			       back_pos != front_pos) {
				back_frag = list_entry(back_pos, struct dpu_fragment, list);
				
				if (back_frag->is_frag) {  /* is_frag=true 是真碎片 */
					/* 找到真碎片，让它迁移到当前空闲页的PFN */
					back_frag->new_pfn = front_frag->old_pfn;
					back_pos = back_pos->prev;
					break;
				}
				
				/* 也是空闲页，继续找 */
				back_pos = back_pos->prev;
			}
			
			front_pos = front_pos->next;
		}
	}

	/* 处理剩余的真碎片（保持原位） */
	while (front_pos != &region->fragments && front_pos != back_pos->next) {
		front_frag = list_entry(front_pos, struct dpu_fragment, list);
		
		if (front_frag->is_frag) {  /* is_frag=true 是真碎片 */
			front_frag->new_pfn = front_frag->old_pfn;
		}
		
		front_pos = front_pos->next;
	}

	/* Execute DPU hardware compaction */
	ret = dpu_hw_compact_execute(region);

	end_time = ktime_get();

	if (ret) {
		pr_err("DPU compaction failed: %d\n", ret);
		region->state = DPU_COMPACT_FAILED;
		return ret;
	}

	region->time_end = jiffies;
	region->total_moved = region->nr_fragments;

	/* Update statistics */
	spin_lock(&stats_lock);
	global_stats.pages_moved += region->nr_fragments;
	global_stats.time_in_dpu_ns += ktime_to_ns(ktime_sub(end_time, start_time));
	spin_unlock(&stats_lock);

	pr_info("DPU compaction completed: %u pages moved in %lld ns\n",
		region->nr_fragments, ktime_to_ns(ktime_sub(end_time, start_time)));

	return 0;
}
int dpu_compact_memory(struct zone *zone, unsigned int order)
{
	struct dpu_compact_region *region;
	unsigned long start_pfn, end_pfn;
	unsigned long region_pfn;
	int ret = COMPACT_COMPLETE;
	int isolated;

	if (!dpu_compact_available())
		return COMPACT_SKIPPED;

	/* Only compact for higher order allocations */
	if (order < pageblock_order)//如果申请的内存块不够大，跳过 DPU 压缩
		return COMPACT_SKIPPED;

	start_pfn = zone->zone_start_pfn;
	end_pfn = zone_end_pfn(zone);//获取内存管理区（Zone）的结束物理页帧号（PFN）

	/* Align to DPU region size */
	region_pfn = ALIGN(start_pfn, DPU_COMPACT_REGION_SIZE >> PAGE_SHIFT);//计算区域对应的页面数
    //找到当前内存管理区（Zone）中，第一个符合 DPU 2MB 区域对齐条件的物理页帧号（PFN）。

	if (region_pfn >= end_pfn)//无需要压缩，即区域小于 2MB
		return COMPACT_SKIPPED;
    //TODO,目前只支持2MB大小压缩，后期要支持跨多个block的压缩

	/* Create compaction region */
	region = dpu_compact_region_create(region_pfn,//创建和DPU沟通的桥梁
					   DPU_COMPACT_REGION_SIZE >> PAGE_SHIFT);
	if (!region) {
		ret = COMPACT_FAILED;
		goto out;
	}

	region->state = DPU_COMPACT_COLLECTING;

	/* Isolate pages in the region */
	isolated = dpu_compact_isolate_pages(zone, region, region_pfn,
					     region_pfn + (DPU_COMPACT_REGION_SIZE >> PAGE_SHIFT));

	pr_info("DPU compact: isolated %d pages in zone %s\n",
		isolated, zone->name);

	/* Check if we have enough fragments */
    //碎片数量不足的情况下跳过
	// if (region->nr_fragments < sysctl_dpu_compact_min_fragments) {
	// 	pr_info("DPU compact: not enough fragments (%u < %u)\n",
	// 		region->nr_fragments, sysctl_dpu_compact_min_fragments);
	// 	ret = COMPACT_SKIPPED;
	// 	goto out_free;
	// }

	/* Execute DPU compaction */
	ret = dpu_compact_execute(region);
	if (ret) {
		pr_err("DPU compact execution failed: %d\n", ret);
		ret = COMPACT_FAILED;
		goto out_free;
	}

	/* Update page tables and metadata */
	ret = dpu_compact_update_mappings(region);
	if (ret) {
		pr_err("DPU mapping update failed: %d\n", ret);
		ret = COMPACT_FAILED;
		goto out_free;
	}

	ret = COMPACT_SUCCESS;

// out_free:
// 	dpu_compact_region_free(region);
out:
	return ret;
}
/*
 * Phase 3: 释放空闲页面
 */
list_for_each_entry(frag, &region->fragments, list) {
    /* 
     * 情况1: 原本的空闲页 (is_frag=false)
     * 这些页面在隔离时从Buddy取出
     * 无论是否被填充，都需要检查
     */
    if (!frag->is_frag) {
        struct page *free_page = frag->page;
        
        /* 
         * 检查这个空闲页是否被用作迁移目标
         * 如果 old_pfn 等于某个真碎片的 new_pfn，说明被填充了
         */
        bool is_filled = false;
        struct dpu_fragment *check_frag;
        
        list_for_each_entry(check_frag, &region->fragments, list) {
            if (check_frag->is_frag && 
                check_frag->new_pfn == frag->old_pfn) {
                is_filled = true;
                break;
            }
        }
        
        if (!is_filled) {
            /* 这个空闲页没有被填充，释放它 */
            __free_page(free_page);
            freed++;
            pr_debug("DPU compact: Freed unused free page at pfn %lu\n",
                     frag->old_pfn);
        } else {
            pr_debug("DPU compact: Free page at pfn %lu was filled, not freeing\n",
                     frag->old_pfn);
        }
        continue;
    }

    /*
     * 情况2: 真碎片迁移后空出的位置
     * old_pfn != new_pfn 的页面，其old_pfn位置现在空了
     */
    if (frag->is_frag && frag->old_pfn != frag->new_pfn) {
        struct page *old_page = pfn_to_page(frag->old_pfn);
        
        /*
         * 检查是否有其他页面填充到这个位置
         * （理论上不会，因为算法设计）
         */
        bool is_filled = false;
        struct dpu_fragment *check_frag;
        
        list_for_each_entry(check_frag, &region->fragments, list) {
            if (check_frag->is_frag && 
                check_frag != frag &&
                check_frag->new_pfn == frag->old_pfn) {
                is_filled = true;
                break;
            }
        }
        
        if (!is_filled) {
            /* 这个位置确实空了，释放它 */
            if (page_count(old_page) == 0) {
                __free_page(old_page);
                freed++;
                pr_debug("DPU compact: Freed vacated page at pfn %lu (was migrated to %lu)\n",
                         frag->old_pfn, frag->new_pfn);
            }
        } else {
            pr_warn("DPU compact: Unexpected - pfn %lu was filled after migration\n",
                    frag->old_pfn);
        }
    }
}