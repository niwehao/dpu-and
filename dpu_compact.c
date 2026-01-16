#include <linux/mm.h>
#include "dpu_compact.h"
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
#include <linux/swapops.h>
#include <asm/tlbflush.h>
#include "internal.h"

/* --- 1. 创建管理区域 --- */
struct dpu_compact_region *dpu_compact_region_create(unsigned long base_pfn, unsigned long size)
{
    struct dpu_compact_region *region;

    region = kzalloc(sizeof(*region), GFP_KERNEL);
    if (!region)
        return NULL;

    region->base_pfn = base_pfn;
    region->region_size = size;
    region->state = DPU_COMPACT_IDLE;

    INIT_LIST_HEAD(&region->fragments);
    spin_lock_init(&region->lock);

    region->dpu_addr_list = kmalloc(DPU_MAX_FRAGMENTS * sizeof(uint64_t), GFP_KERNEL);
    if (!region->dpu_addr_list) {
        kfree(region);
        return NULL;
    }

    region->dpu_buffer = kmalloc(DPU_COMPACT_REGION_SIZE, GFP_KERNEL );
    if (!region->dpu_buffer) {
        kfree(region->dpu_addr_list);
        kfree(region);
        return NULL;
    }

    return region;
}

/* --- 2. 页面适用性检查 --- */
bool dpu_compact_page_suitable(struct page *page)
{
    if (PageHuge(page) || PageTransHuge(page))
        return false;

    if (PageReserved(page) || PageKsm(page))
        return false;

    if (PageWriteback(page))
        return false;

    if (PageUnevictable(page))
        return false;

    if (!PageLRU(page) && !__PageMovable(page))
        return false;

    return true;
}

/* --- 3. 添加碎片记录 --- */
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
    frag->old_pfn = page_to_pfn(page);
    frag->is_anon = PageAnon(page);
    frag->is_dirty = PageDirty(page);
    frag->is_frag = is_frag;
    frag->anon_vma = NULL;

    /* 不需要记录单个 VMA，migration entry 会处理所有映射 */
    frag->is_mapped = false;
    frag->vaddr = 0;
    frag->mm = NULL;

    spin_lock_irqsave(&region->lock, flags);
    list_add_tail(&frag->list, &region->fragments);
    region->dpu_addr_list[region->nr_fragments] = (uint64_t)(frag->old_pfn << PAGE_SHIFT);
    region->nr_fragments++;
    spin_unlock_irqrestore(&region->lock, flags);

    return 0;
}

/* --- 4. 隔离 Buddy 空闲页 --- */
static unsigned long dpu_compact_isolate_buddy_page(struct dpu_compact_region *region, 
                                                   struct page *page)
{
    struct list_head free_list;
    struct page *split_page, *tmp;
    unsigned long remaining_space;
    unsigned long taken = 0;
    unsigned int order;
    unsigned long flags;
    struct zone *zone = page_zone(page);

    remaining_space = DPU_MAX_FRAGMENTS - region->nr_fragments;
    if (remaining_space == 0)
        return 0;

    order = buddy_order(page);

    spin_lock_irqsave(&zone->lock, flags);
    if (!__isolate_free_page(page, order)) {
        spin_unlock_irqrestore(&zone->lock, flags);
        return 0;
    }
    spin_unlock_irqrestore(&zone->lock, flags);

    INIT_LIST_HEAD(&free_list);
    list_add(&page->lru, &free_list);

    split_map_pages(&free_list);

    list_for_each_entry_safe(split_page, tmp, &free_list, lru) {
        list_del(&split_page->lru);
        if (taken < remaining_space && 
            dpu_compact_add_fragment(region, split_page, NULL, 0, false) == 0) {
            taken++;
        } else {
            __free_page(split_page);
        }
    }
    return taken;
}

/* --- 5. 页面扫描与隔离 --- */
int dpu_compact_isolate_pages(struct zone *zone, struct dpu_compact_region *region,
                  unsigned long start_pfn, unsigned long end_pfn)
{
    unsigned long pfn;
    struct page *page;
    int isolated = 0;

    for (pfn = start_pfn; pfn < end_pfn; pfn++) {
        if (region->nr_fragments >= DPU_MAX_FRAGMENTS)
            break;

        if (!pfn_valid(pfn))
            continue;

        page = pfn_to_page(pfn);
        if (page_zone(page) != zone)
            continue;

        if (PageBuddy(page)) {
            isolated += dpu_compact_isolate_buddy_page(region, page);
            continue;     
        }

        if (!dpu_compact_page_suitable(page))
            continue;

        if (PageLRU(page)) {
            if (isolate_lru_page(page) != 0)
                continue;

            if (!trylock_page(page)) {
                putback_lru_page(page);
                continue;
            }

            if (dpu_compact_add_fragment(region, page, NULL, 0, true) == 0) {
                isolated++;
            } else {
                unlock_page(page);
                putback_lru_page(page);
            }
        }
    }
    return isolated;
}

/* --- 6. 建立 migration entries (关键修复) --- */
static int dpu_compact_unmap_pages(struct dpu_compact_region *region)
{
    struct dpu_fragment *frag;
    int page_was_mapped;
    struct folio *src_folio;

    list_for_each_entry(frag, &region->fragments, list) {
        if (!frag->is_frag)
            continue;

        src_folio = page_folio(frag->page);
        
        /* 
         * 获取 anon_vma 引用（如果是匿名页）
         * 这防止在迁移过程中 anon_vma 被释放
         */
        if (folio_test_anon(src_folio) && !folio_test_ksm(src_folio))
            frag->anon_vma = folio_get_anon_vma(src_folio);

        /* 检查页面是否有映射 */
        if (!src_folio->mapping) {
            /* 无映射的页面，尝试释放 buffer */
            if (folio_test_private(src_folio)) {
                try_to_free_buffers(src_folio);
            }
            continue;
        }

        if (!folio_mapped(src_folio))
            continue;

        /* 
         * 核心修复：使用 try_to_migrate() 将所有 PTE 替换为 migration entry
         * 这会自动处理：
         * - 所有进程的所有映射
         * - 文件页的共享映射
         * - fork 后的父子进程
         * - rmap 更新
         */
        try_to_migrate(src_folio, 0);
        page_was_mapped = 1;
        frag->was_mapped = page_was_mapped;
    }

    return 0;
}

/* --- 7. 计算迁移目标并触发 DPU --- */
int dpu_compact_execute(struct dpu_compact_region *region)
{
    int ret;
    int last_pfn;
    ktime_t start_time, end_time;
    struct dpu_fragment *slow_frag, *fast_frag;
    struct list_head *slow_pos, *fast_pos;

    if (region->state != DPU_COMPACT_COLLECTING || region->nr_fragments == 0)
        return -EINVAL;

    region->state = DPU_COMPACT_MOVING;
    
    /* 第一步：建立 migration entries */
    ret = dpu_compact_unmap_pages(region);
    if (ret)
        return ret;

    start_time = ktime_get();

    /* 第二步：计算 PFN 映射（双指针算法） */
    slow_pos = region->fragments.next;
    fast_pos = region->fragments.next;
    slow_frag = list_entry(slow_pos, struct dpu_fragment, list);//空闲位置指针


    while (fast_pos != &region->fragments) {
        fast_frag = list_entry(fast_pos, struct dpu_fragment, list);
        if (fast_frag->is_frag) {
            fast_frag->new_pfn = slow_frag->old_pfn;
            slow_pos = slow_pos->next;
            last_pfn = slow_frag->old_pfn;
        }
        fast_pos = fast_pos->next;
    }
       


    /* 第三步：DPU 硬件搬运数据 */
    ret = dpu_hw_compact_execute(region);
    
    /* 必须添加内存屏障，确保 DPU 写入完成 */
    if (!ret) {
        smp_wmb();
        dma_sync_single_for_cpu(NULL, region->dpu_buffer_dma, 
                               DPU_COMPACT_REGION_SIZE, DMA_FROM_DEVICE);
    }
    
    end_time = ktime_get();

    if (ret) {
        region->state = DPU_COMPACT_FAILED;
        return 0;
    }

    return last_pfn;
}

/* --- 8. 更新映射与元数据 (完全重写) --- */
int dpu_compact_update_mappings(struct dpu_compact_region *region,int last_pfn)
{
    struct dpu_fragment *frag;
    int rc;

    if (region->state != DPU_COMPACT_MOVING)
        return -EINVAL;

    region->state = DPU_COMPACT_UPDATING;

    list_for_each_entry(frag, &region->fragments, list) {
        struct folio *src_folio = page_folio(frag->page);
        struct folio *dst_folio;
        struct page *newpage;
        
        /* 空闲页直接放回 buddy 系统 */
        if (frag->old_pfn > last_pfn) {
            __free_page(frag->page);
            unlock_page(frag->page);
            continue;
        }

        /* 原地不动的页面 */
        if (frag->old_pfn == frag->new_pfn) {
            unlock_page(frag->page);
            putback_lru_page(frag->page);
            if (frag->anon_vma)
                put_anon_vma(frag->anon_vma);
            continue;
        }

        /* 获取新页面 */
        newpage = pfn_to_page(frag->new_pfn);
        dst_folio = page_folio(newpage);

        /* 锁定新页面 */
        if (!trylock_page(newpage)) {
            pr_err("DPU compact: failed to lock new page\n");
            unlock_page(frag->page);
            putback_lru_page(frag->page);
            if (frag->anon_vma)
                put_anon_vma(frag->anon_vma);
            continue;
        }

        /*
         * 核心修复1: 更新 page cache 映射
         * folio_migrate_mapping() 会：
         * - 更新 radix tree/xarray
         * - 处理引用计数
         * - 更新统计信息
         */
        if (src_folio->mapping) {
            rc = folio_migrate_mapping(src_folio->mapping, dst_folio, 
                                      src_folio, 0);
            if (rc != MIGRATEPAGE_SUCCESS) {
                pr_err("DPU compact: mapping migration failed\n");
                unlock_page(newpage);
                unlock_page(frag->page);
                putback_lru_page(frag->page);
                if (frag->anon_vma)
                    put_anon_vma(frag->anon_vma);
                continue;
            }
        } else {
            /* 无 mapping 的匿名页 */
            dst_folio->index = src_folio->index;
            dst_folio->mapping = src_folio->mapping;
            if (folio_test_swapbacked(src_folio))
                __folio_set_swapbacked(dst_folio);
        }

        /*
         * 核心修复2: 复制页面标志和元数据
         * 这会正确复制所有软件状态
         */
        folio_migrate_flags(dst_folio, src_folio);

        /*
         * 核心修复3: 恢复所有映射
         * remove_migration_ptes() 会：
         * - 遍历所有 migration entries
         * - 将它们转换回正常 PTE，指向新页
         * - 正确更新 rmap
         * - 处理所有进程的所有映射
         */
        if (frag->was_mapped) {
            /* 先添加到 LRU，remove_migration_ptes 需要它 */
            folio_add_lru(dst_folio);
            
            /* 这会自动处理所有映射和 rmap */
            remove_migration_ptes(src_folio, dst_folio, false);
        } else {
            /* 无映射的页面也要加入 LRU */
            folio_add_lru(dst_folio);
        }

        /* 释放锁 */
        unlock_page(newpage);
        unlock_page(frag->page);

        /* 释放 anon_vma 引用 */
        if (frag->anon_vma)
            put_anon_vma(frag->anon_vma);

        /* 
         * 核心修复4: 正确管理引用计数
         * 旧页面：
         * - isolate_lru_page() 增加了 1 次引用
         * - 现在释放这个引用
         */
        put_page(frag->page);
        
        /*
         * 新页面：
         * - folio_migrate_mapping() 已经正确设置了引用计数
         * - 不需要额外操作
         */
    }

    /* 全局 TLB 刷新 */
    flush_tlb_all();

    region->state = DPU_COMPACT_COMPLETE;
    return 0;
}

/* --- 9. 清理函数 --- */
static void dpu_compact_cleanup(struct dpu_compact_region *region, bool success)
{
    struct dpu_fragment *frag, *tmp;

    if (!success) {
        /* 失败情况：恢复所有页面 */
        list_for_each_entry_safe(frag, tmp, &region->fragments, list) {
            struct folio *folio = page_folio(frag->page);
            
            /* 恢复映射（如果有 migration entries） */
            if (frag->was_mapped && folio_mapped(folio))
                remove_migration_ptes(folio, folio, false);
            
            if (frag->is_frag) {
                unlock_page(frag->page);
                putback_lru_page(frag->page);
            } else {
                unlock_page(frag->page);
                __free_page(frag->page);
            }
            
            if (frag->anon_vma)
                put_anon_vma(frag->anon_vma);
            
            list_del(&frag->list);
            kfree(frag);
        }
    } else {
        /* 成功情况：只需释放 fragment 结构 */
        list_for_each_entry_safe(frag, tmp, &region->fragments, list) {
            list_del(&frag->list);
            kfree(frag);
        }
    }

    region->nr_fragments = 0;
}

/* --- 10. 入口函数 --- */
int dpu_compact_memory(struct zone *zone, unsigned int order)
{
    struct dpu_compact_region *region;
    unsigned long start_pfn, region_pfn;
    int last_pfn;
    int ret = COMPACT_COMPLETE;

    if (!dpu_compact_available() || order < pageblock_order)
        return COMPACT_SKIPPED;

    start_pfn = zone->zone_start_pfn;
    region_pfn = ALIGN(start_pfn, DPU_COMPACT_REGION_SIZE >> PAGE_SHIFT);

    if (region_pfn >= zone_end_pfn(zone))
        return COMPACT_SKIPPED;

    region = dpu_compact_region_create(region_pfn, DPU_COMPACT_REGION_SIZE >> PAGE_SHIFT);
    if (!region) 
        return COMPACT_FAILED;

    region->state = DPU_COMPACT_COLLECTING;

    /* 隔离页面 */
    dpu_compact_isolate_pages(zone, region, region_pfn, 
                             region_pfn + (DPU_COMPACT_REGION_SIZE >> PAGE_SHIFT));

    if (region->nr_fragments == 0) {
        ret = COMPACT_SKIPPED;
        goto out_free;
    }

    /* 执行迁移 */
    last_pfn = dpu_compact_execute(region);
    if (last_pfn == 0) {
        ret = COMPACT_FAILED;
        dpu_compact_cleanup(region, false);
        goto out_free;
    }


    /* 更新映射 */
    if (dpu_compact_update_mappings(region, last_pfn) != 0) {
        ret = COMPACT_FAILED;
        dpu_compact_cleanup(region, false);
        goto out_free;
    }

    ret = COMPACT_SUCCESS;
    dpu_compact_cleanup(region, true);

out_free:
    if (region->dpu_buffer)
        kfree(region->dpu_buffer);
    if (region->dpu_addr_list)
        kfree(region->dpu_addr_list);
    kfree(region);
    
    return ret;
}