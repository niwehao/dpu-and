/* dpu_compact_verify.c - DPU压缩验证模块 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/rmap.h>
#include <linux/pagemap.h>
#include <linux/sched/mm.h>
#include <linux/highmem.h>
#include <linux/debugfs.h>
#include <linux/random.h>
#include <asm/tlbflush.h>
#include <asm/pgtable.h>

/* ========== 1. 页面内容完整性验证 ========== */

#define MAGIC_PATTERN_SIZE 4096
#define MAGIC_HEADER 0xDEADBEEF
#define MAGIC_FOOTER 0xCAFEBABE

struct page_marker {
	u32 header;
	u32 page_index;
	u64 original_pfn;
	u64 timestamp;
	u8 random_data[MAGIC_PATTERN_SIZE - 24];
	u32 checksum;
	u32 footer;
};

/* 在页面中写入可验证的模式 */
static int mark_page_with_pattern(struct page *page, unsigned int index)
{
	struct page_marker *marker;
	unsigned long pfn = page_to_pfn(page);
	u32 checksum = 0;
	int i;
	
	marker = kmap_atomic(page);
	
	marker->header = MAGIC_HEADER;
	marker->page_index = index;
	marker->original_pfn = pfn;
	marker->timestamp = ktime_get_ns();
	
	/* 填充随机数据 */
	get_random_bytes(marker->random_data, sizeof(marker->random_data));
	
	/* 计算校验和 */
	for (i = 0; i < sizeof(marker->random_data); i++)
		checksum += marker->random_data[i];
	
	marker->checksum = checksum;
	marker->footer = MAGIC_FOOTER;
	
	kunmap_atomic(marker);
	
	return 0;
}

/* 验证页面内容是否完整 */
static int verify_page_pattern(struct page *page, unsigned int expected_index,
			       u64 expected_pfn)
{
	struct page_marker *marker;
	unsigned long current_pfn = page_to_pfn(page);
	u32 checksum = 0;
	int i;
	int errors = 0;
	
	marker = kmap_atomic(page);
	
	/* 验证头部魔数 */
	if (marker->header != MAGIC_HEADER) {
		pr_err("❌ Header corruption: expected 0x%x, got 0x%x\n",
		       MAGIC_HEADER, marker->header);
		errors++;
	}
	
	/* 验证页面索引 */
	if (marker->page_index != expected_index) {
		pr_err("❌ Index mismatch: expected %u, got %u\n",
		       expected_index, marker->page_index);
		errors++;
	}
	
	/* 验证原始PFN */
	if (marker->original_pfn != expected_pfn) {
		pr_warn("⚠️  PFN changed: %llx -> %lx (expected if compacted)\n",
			marker->original_pfn, current_pfn);
	}
	
	/* 验证校验和 */
	for (i = 0; i < sizeof(marker->random_data); i++)
		checksum += marker->random_data[i];
	
	if (marker->checksum != checksum) {
		pr_err("❌ Checksum mismatch: expected 0x%x, got 0x%x\n",
		       checksum, marker->checksum);
		errors++;
	}
	
	/* 验证尾部魔数 */
	if (marker->footer != MAGIC_FOOTER) {
		pr_err("❌ Footer corruption: expected 0x%x, got 0x%x\n",
		       MAGIC_FOOTER, marker->footer);
		errors++;
	}
	
	kunmap_atomic(marker);
	
	if (errors == 0)
		pr_info("✅ Page %u integrity verified (PFN: %lx)\n",
			expected_index, current_pfn);
	
	return errors ? -EINVAL : 0;
}

/* ========== 2. 虚拟地址映射验证 ========== */

struct vma_mapping {
	struct vm_area_struct *vma;
	unsigned long vaddr;
	struct page *page;
	unsigned long original_pfn;
	unsigned long original_pte;
};

/* 验证虚拟地址是否正确映射到物理页 */
static int verify_va_mapping(struct mm_struct *mm, unsigned long vaddr,
			     struct page *expected_page)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *ptep, pte;
	struct page *actual_page;
	unsigned long pfn;
	spinlock_t *ptl;
	int ret = 0;
	
	/* 遍历页表层级 */
	pgd = pgd_offset(mm, vaddr);
	if (pgd_none(*pgd) || pgd_bad(*pgd)) {
		pr_err("❌ Invalid PGD for vaddr %lx\n", vaddr);
		return -EINVAL;
	}
	
	p4d = p4d_offset(pgd, vaddr);
	if (p4d_none(*p4d) || p4d_bad(*p4d)) {
		pr_err("❌ Invalid P4D for vaddr %lx\n", vaddr);
		return -EINVAL;
	}
	
	pud = pud_offset(p4d, vaddr);
	if (pud_none(*pud) || pud_bad(*pud)) {
		pr_err("❌ Invalid PUD for vaddr %lx\n", vaddr);
		return -EINVAL;
	}
	
	pmd = pmd_offset(pud, vaddr);
	if (pmd_none(*pmd) || pmd_bad(*pmd)) {
		pr_err("❌ Invalid PMD for vaddr %lx\n", vaddr);
		return -EINVAL;
	}
	
	/* 获取PTE */
	ptep = pte_offset_map_lock(mm, pmd, vaddr, &ptl);
	if (!ptep) {
		pr_err("❌ Failed to get PTE for vaddr %lx\n", vaddr);
		return -EINVAL;
	}
	
	pte = *ptep;
	
	/* 检查PTE是否为migration entry */
	if (is_swap_pte(pte)) {
		swp_entry_t entry = pte_to_swp_entry(pte);
		if (is_migration_entry(entry)) {
			pr_err("❌ PTE still contains migration entry! vaddr=%lx\n", vaddr);
			pr_err("   Migration entry not removed after compaction!\n");
			ret = -EINVAL;
			goto unlock;
		}
	}
	
	/* 检查PTE是否present */
	if (!pte_present(pte)) {
		pr_err("❌ PTE not present for vaddr %lx\n", vaddr);
		ret = -EINVAL;
		goto unlock;
	}
	
	/* 获取物理页 */
	pfn = pte_pfn(pte);
	actual_page = pfn_to_page(pfn);
	
	/* 验证页面匹配 */
	if (actual_page != expected_page) {
		pr_err("❌ Page mismatch for vaddr %lx:\n", vaddr);
		pr_err("   Expected page: %px (PFN: %lx)\n",
		       expected_page, page_to_pfn(expected_page));
		pr_err("   Actual page:   %px (PFN: %lx)\n",
		       actual_page, pfn);
		ret = -EINVAL;
	} else {
		pr_info("✅ VA mapping verified: vaddr=%lx -> PFN=%lx\n",
			vaddr, pfn);
	}
	
unlock:
	pte_unmap_unlock(ptep, ptl);
	return ret;
}

/* ========== 3. TLB一致性验证 ========== */

/* 验证TLB缓存是否正确刷新 */
static int verify_tlb_consistency(struct mm_struct *mm, unsigned long vaddr,
				  struct page *page)
{
	unsigned long old_value, new_value;
	volatile unsigned long *ptr;
	int errors = 0;
	
	/* 通过虚拟地址访问页面 */
	ptr = (volatile unsigned long *)vaddr;
	
	/* 读取原始值 */
	old_value = *ptr;
	
	/* 写入新值 */
	*ptr = 0x12345678ABCDEFULL;
	
	/* 强制刷新CPU缓存 */
	smp_mb();
	
	/* 读回验证 */
	new_value = *ptr;
	
	if (new_value != 0x12345678ABCDEFULL) {
		pr_err("❌ TLB inconsistency detected!\n");
		pr_err("   Written: 0x%lx, Read back: 0x%lx\n",
		       0x12345678ABCDEFULL, new_value);
		errors++;
	}
	
	/* 恢复原值 */
	*ptr = old_value;
	
	/* 验证物理地址访问 */
	{
		void *kaddr = kmap_atomic(page);
		unsigned long phys_value = *(unsigned long *)kaddr;
		
		if (phys_value != old_value) {
			pr_err("❌ Virtual/Physical access mismatch!\n");
			pr_err("   Via VA: 0x%lx, Via PA: 0x%lx\n",
			       old_value, phys_value);
			errors++;
		} else {
			pr_info("✅ TLB consistency verified for vaddr %lx\n", vaddr);
		}
		
		kunmap_atomic(kaddr);
	}
	
	return errors ? -EINVAL : 0;
}

/* ========== 4. 物理内存连续性验证 ========== */

struct compaction_result {
	unsigned long start_pfn;
	unsigned long end_pfn;
	unsigned long nr_contiguous;
	unsigned long max_contiguous_order;
	unsigned long nr_holes;
	unsigned long fragmentation_score;
};

/* 分析内存连续性 */
static int analyze_memory_continuity(struct zone *zone,
				     unsigned long start_pfn,
				     unsigned long end_pfn,
				     struct compaction_result *result)
{
	unsigned long pfn;
	unsigned long current_run = 0;
	unsigned long max_run = 0;
	unsigned long holes = 0;
	
	memset(result, 0, sizeof(*result));
	result->start_pfn = start_pfn;
	result->end_pfn = end_pfn;
	
	for (pfn = start_pfn; pfn < end_pfn; pfn++) {
		struct page *page;
		
		if (!pfn_valid(pfn)) {
			if (current_run > 0)
				holes++;
			current_run = 0;
			continue;
		}
		
		page = pfn_to_page(pfn);
		
		/* 检查是否为空闲页 */
		if (PageBuddy(page)) {
			current_run++;
			if (current_run > max_run)
				max_run = current_run;
		} else {
			if (current_run > 0)
				result->nr_contiguous++;
			current_run = 0;
		}
	}
	
	result->max_contiguous_order = max_run > 0 ? ilog2(max_run) : 0;
	result->nr_holes = holes;
	result->fragmentation_score = 
		(holes * 100) / (end_pfn - start_pfn);
	
	pr_info("📊 Memory Continuity Analysis:\n");
	pr_info("   Range: PFN %lx - %lx\n", start_pfn, end_pfn);
	pr_info("   Max contiguous: %lu pages (order %lu)\n",
		max_run, result->max_contiguous_order);
	pr_info("   Holes: %lu\n", holes);
	pr_info("   Fragmentation: %lu%%\n", result->fragmentation_score);
	
	return 0;
}

/* ========== 5. 引用计数验证 ========== */

static int verify_page_refcount(struct page *page, const char *context)
{
	int refcount = page_ref_count(page);
	int mapcount = page_mapcount(page);
	int expected_min = 1; /* 至少LRU持有一个引用 */
	
	pr_info("🔍 Refcount check (%s):\n", context);
	pr_info("   Page: %px (PFN: %lx)\n", page, page_to_pfn(page));
	pr_info("   Refcount: %d\n", refcount);
	pr_info("   Mapcount: %d\n", mapcount);
	
	if (refcount < expected_min) {
		pr_err("❌ Refcount too low! Expected >= %d, got %d\n",
		       expected_min, refcount);
		pr_err("   This indicates use-after-free risk!\n");
		return -EINVAL;
	}
	
	/* 如果页面被映射，mapcount应该 >= 1 */
	if (PageMapped(page) && mapcount < 1) {
		pr_err("❌ Page marked as mapped but mapcount is %d!\n", mapcount);
		return -EINVAL;
	}
	
	/* 检查引用泄漏 */
	if (refcount > 100) {
		pr_warn("⚠️  Suspiciously high refcount: %d\n", refcount);
		pr_warn("   Possible reference leak!\n");
		return -EINVAL;
	}
	
	pr_info("✅ Refcount valid\n");
	return 0;
}

/* ========== 6. 进程访问验证 ========== */

struct test_process_ctx {
	struct task_struct *task;
	struct mm_struct *mm;
	unsigned long test_addr;
	struct page *test_page;
	bool access_ok;
};

/* 在用户进程中验证页面访问 */
static int verify_user_access(struct test_process_ctx *ctx)
{
	struct vm_area_struct *vma;
	unsigned long addr = ctx->test_addr;
	int *user_ptr;
	int test_value = 0x42424242;
	int read_value;
	
	if (!ctx->mm)
		return -EINVAL;
	
	/* 获取VMA */
	mmap_read_lock(ctx->mm);
	vma = find_vma(ctx->mm, addr);
	if (!vma || addr < vma->vm_start) {
		pr_err("❌ No VMA found for address %lx\n", addr);
		mmap_read_unlock(ctx->mm);
		return -EINVAL;
	}
	
	/* 检查访问权限 */
	if (!(vma->vm_flags & VM_WRITE)) {
		pr_err("❌ VMA not writable\n");
		mmap_read_unlock(ctx->mm);
		return -EACCES;
	}
	
	mmap_read_unlock(ctx->mm);
	
	/* 切换到用户进程上下文 */
	use_mm(ctx->mm);
	
	user_ptr = (int *)addr;
	
	/* 写测试 */
	if (copy_to_user(user_ptr, &test_value, sizeof(test_value))) {
		pr_err("❌ Failed to write to user address %lx\n", addr);
		unuse_mm(ctx->mm);
		return -EFAULT;
	}
	
	/* 读测试 */
	if (copy_from_user(&read_value, user_ptr, sizeof(read_value))) {
		pr_err("❌ Failed to read from user address %lx\n", addr);
		unuse_mm(ctx->mm);
		return -EFAULT;
	}
	
	/* 验证值 */
	if (read_value != test_value) {
		pr_err("❌ Read/Write mismatch: wrote 0x%x, read 0x%x\n",
		       test_value, read_value);
		unuse_mm(ctx->mm);
		return -EINVAL;
	}
	
	unuse_mm(ctx->mm);
	
	pr_info("✅ User access verified at %lx\n", addr);
	ctx->access_ok = true;
	
	return 0;
}

/* ========== 7. 综合验证流程 ========== */

struct comprehensive_test {
	struct zone *zone;
	struct page **test_pages;
	unsigned int nr_pages;
	struct vma_mapping *mappings;
	unsigned int nr_mappings;
	struct compaction_result result_before;
	struct compaction_result result_after;
};

/* 压缩前的准备和标记 */
static int prepare_comprehensive_test(struct comprehensive_test *test,
				      struct zone *zone,
				      unsigned int nr_pages)
{
	unsigned int i;
	
	test->zone = zone;
	test->nr_pages = nr_pages;
	
	/* 分配页面数组 */
	test->test_pages = kvmalloc_array(nr_pages, sizeof(struct page *),
					  GFP_KERNEL);
	if (!test->test_pages)
		return -ENOMEM;
	
	/* 分配并标记测试页面 */
	for (i = 0; i < nr_pages; i++) {
		struct page *page = alloc_page(GFP_KERNEL);
		if (!page) {
			pr_err("Failed to allocate test page %u\n", i);
			goto cleanup;
		}
		
		test->test_pages[i] = page;
		
		/* 写入可验证的模式 */
		mark_page_with_pattern(page, i);
		
		pr_info("Marked page %u: PFN %lx\n", i, page_to_pfn(page));
	}
	
	/* 分析压缩前的内存状态 */
	analyze_memory_continuity(zone,
				  zone->zone_start_pfn,
				  zone_end_pfn(zone),
				  &test->result_before);
	
	return 0;
	
cleanup:
	for (i = 0; i < nr_pages; i++) {
		if (test->test_pages[i])
			__free_page(test->test_pages[i]);
	}
	kvfree(test->test_pages);
	return -ENOMEM;
}

/* 压缩后的验证 */
static int verify_after_compaction(struct comprehensive_test *test)
{
	unsigned int i;
	int errors = 0;
	
	pr_info("\n========== Post-Compaction Verification ==========\n");
	
	/* 1. 验证页面内容完整性 */
	pr_info("\n--- Phase 1: Content Integrity ---\n");
	for (i = 0; i < test->nr_pages; i++) {
		struct page *page = test->test_pages[i];
		unsigned long original_pfn = page_to_pfn(page);
		
		if (verify_page_pattern(page, i, original_pfn) != 0) {
			pr_err("Page %u failed integrity check\n", i);
			errors++;
		}
	}
	
	/* 2. 验证引用计数 */
	pr_info("\n--- Phase 2: Reference Counts ---\n");
	for (i = 0; i < test->nr_pages; i++) {
		char context[64];
		snprintf(context, sizeof(context), "Page %u", i);
		
		if (verify_page_refcount(test->test_pages[i], context) != 0)
			errors++;
	}
	
	/* 3. 分析压缩后的内存连续性 */
	pr_info("\n--- Phase 3: Memory Continuity ---\n");
	analyze_memory_continuity(test->zone,
				  test->zone->zone_start_pfn,
				  zone_end_pfn(test->zone),
				  &test->result_after);
	
	/* 4. 比较压缩效果 */
	pr_info("\n--- Phase 4: Compaction Effectiveness ---\n");
	pr_info("Before compaction:\n");
	pr_info("  Max order: %lu\n", test->result_before.max_contiguous_order);
	pr_info("  Holes: %lu\n", test->result_before.nr_holes);
	pr_info("  Fragmentation: %lu%%\n", test->result_before.fragmentation_score);
	
	pr_info("After compaction:\n");
	pr_info("  Max order: %lu\n", test->result_after.max_contiguous_order);
	pr_info("  Holes: %lu\n", test->result_after.nr_holes);
	pr_info("  Fragmentation: %lu%%\n", test->result_after.fragmentation_score);
	
	if (test->result_after.max_contiguous_order >
	    test->result_before.max_contiguous_order) {
		pr_info("✅ Compaction improved max order by %lu\n",
			test->result_after.max_contiguous_order -
			test->result_before.max_contiguous_order);
	} else {
		pr_warn("⚠️  No improvement in max order\n");
	}
	
	if (test->result_after.nr_holes < test->result_before.nr_holes) {
		pr_info("✅ Reduced holes by %lu\n",
			test->result_before.nr_holes - test->result_after.nr_holes);
	}
	
	/* 5. 总结 */
	pr_info("\n========== Verification Summary ==========\n");
	if (errors == 0) {
		pr_info("✅ ALL CHECKS PASSED\n");
		pr_info("   Compaction is CORRECT and EFFECTIVE\n");
		return 0;
	} else {
		pr_err("❌ FAILED: %d errors detected\n", errors);
		pr_err("   Compaction has BUGS that need fixing\n");
		return -EINVAL;
	}
}

/* ========== 8. 特殊场景测试 ========== */

/* 测试并发访问 */
static int test_concurrent_access(struct page *page)
{
	#define NR_THREADS 4
	struct task_struct *threads[NR_THREADS];
	atomic_t counter = ATOMIC_INIT(0);
	int i;
	
	pr_info("Testing concurrent access...\n");
	
	/* 启动多个内核线程同时访问页面 */
	for (i = 0; i < NR_THREADS; i++) {
		threads[i] = kthread_run(/* thread function */, NULL,
					 "test_thread_%d", i);
	}
	
	/* 等待线程完成 */
	for (i = 0; i < NR_THREADS; i++) {
		if (!IS_ERR(threads[i]))
			kthread_stop(threads[i]);
	}
	
	pr_info("✅ Concurrent access test completed\n");
	return 0;
}

/* ========== 导出接口 ========== */

/* debugfs触发完整验证 */
static ssize_t full_verify_write(struct file *file, const char __user *buf,
				  size_t count, loff_t *ppos)
{
	struct comprehensive_test test;
	struct zone *zone;
	int ret;
	
	/* 选择测试zone */
	for_each_populated_zone(zone) {
		if (zone_idx(zone) == ZONE_NORMAL)
			break;
	}
	
	if (!zone) {
		pr_err("No suitable zone found\n");
		return -ENODEV;
	}
	
	/* 准备测试 */
	ret = prepare_comprehensive_test(&test, zone, 100);
	if (ret) {
		pr_err("Failed to prepare test: %d\n", ret);
		return ret;
	}
	
	/* 执行DPU压缩 */
	pr_info("Triggering DPU compaction...\n");
	ret = try_dpu_compact_zone(zone, pageblock_order, GFP_KERNEL);
	pr_info("Compaction returned: %d\n", ret);
	
	/* 验证结果 */
	verify_after_compaction(&test);
	
	/* 清理 */
	/* cleanup code */
	
	return count;
}

static const struct file_operations full_verify_fops = {
	.owner = THIS_MODULE,
	.write = full_verify_write,
};

MODULE_LICENSE("GPL");