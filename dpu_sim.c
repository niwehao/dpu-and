#include <linux/dpu_compact.h>
static int dpu_hw_compact_execute(struct dpu_compact_region *region)
{
	struct dpu_fragment *frag;
	int migrated = 0;
	int ret;
	unsigned long *src_pfn_list;
	unsigned long *dst_pfn_list;
	int nr_migrations = 0;
	int i = 0;

	/* 统计需要迁移的页面数 */
	list_for_each_entry(frag, &region->fragments, list) {
		if (frag->is_frag && frag->old_pfn != frag->new_pfn) {
			nr_migrations++;
		}
	}

	if (nr_migrations == 0) {
		pr_info("DPU compact: No pages need migration\n");
		return 0;
	}

	/* 分配源和目标PFN数组 */
	src_pfn_list = kmalloc_array(nr_migrations, sizeof(unsigned long), GFP_KERNEL);
	dst_pfn_list = kmalloc_array(nr_migrations, sizeof(unsigned long), GFP_KERNEL);
	
	if (!src_pfn_list || !dst_pfn_list) {
		kfree(src_pfn_list);
		kfree(dst_pfn_list);
		return -ENOMEM;
	}

	/* 构建迁移列表 */
	list_for_each_entry(frag, &region->fragments, list) {
		/* 只处理需要迁移的真碎片 */
		if (frag->is_frag && frag->old_pfn != frag->new_pfn) {
			src_pfn_list[i] = frag->old_pfn;
			dst_pfn_list[i] = frag->new_pfn;
			i++;
			
			pr_debug("DPU compact: Plan to migrate PFN %lu -> %lu\n",
				 frag->old_pfn, frag->new_pfn);
		}
	}

	/* 调用DPU硬件执行迁移 */
	ret = dpu_hw_memory_move(src_pfn_list, dst_pfn_list, nr_migrations);
	
	if (ret < 0) {
		pr_err("DPU compact: Hardware migration failed with error %d\n", ret);
		kfree(src_pfn_list);
		kfree(dst_pfn_list);
		return ret;
	}

	migrated = ret;
	pr_info("DPU compact: Successfully migrated %d pages\n", migrated);

	kfree(src_pfn_list);
	kfree(dst_pfn_list);

	return migrated;
}


static int dpu_hw_memory_move(unsigned long *src_pfn_list,
			      unsigned long *dst_pfn_list,
			      int count)
{
	int i, migrated = 0;

	for (i = 0; i < count; i++) {
		struct page *src_page, *dst_page;
		void *src, *dst;

		/* 验证并获取page */
		if (!pfn_valid(src_pfn_list[i]) || !pfn_valid(dst_pfn_list[i]))
			continue;

		src_page = pfn_to_page(src_pfn_list[i]);
		dst_page = pfn_to_page(dst_pfn_list[i]);

		/* 原子映射并拷贝 */
		src = kmap_atomic(src_page);
		dst = kmap_atomic(dst_page);
		
		copy_page(dst, src);  /* 或使用 memcpy(dst, src, PAGE_SIZE) */
		
		kunmap_atomic(dst);
		kunmap_atomic(src);

		migrated++;

		/* 每64页让出CPU */
		if ((i + 1) % 64 == 0)
			cond_resched();
	}

	return migrated;
}