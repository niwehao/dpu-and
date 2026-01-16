#include <linux/mm.h>
#include "dpu_compact.h"
#include <linux/compaction.h>
#include "internal.h"
enum compact_result try_dpu_compact_zone(struct zone *zone,
					 unsigned int order,
					 gfp_t gfp_mask)
{
	enum compact_result ret;

	/* Check if DPU compaction is available */
	if (!dpu_compact_available())
		return COMPACT_SKIPPED;

	/* DPU compaction is most effective for higher order allocations */
	if (order < pageblock_order)
		return COMPACT_SKIPPED;

	/* Skip for atomic allocations - DPU compaction may take time */
	if (gfp_mask & __GFP_ATOMIC)
		return COMPACT_SKIPPED;
        //不可休眠，不可阻塞。 使用这个标志的程序要求内核：“立刻给我内存，行就行，不行就报错，千万别让我等。
        //DPU迁移会消耗大量时间，不合适

	trace_printk("DPU: Attempting compaction for order %u in zone %s\n",
		     order, zone->name);

	ret = dpu_compact_memory(zone, order);

	switch (ret) {
	case COMPACT_SUCCESS:
		trace_printk("DPU: Compaction succeeded\n");
		break;
	case COMPACT_PARTIAL:
		trace_printk("DPU: Compaction partially succeeded\n");
		break;
	case COMPACT_COMPLETE:
		trace_printk("DPU: Compaction completed but no suitable block\n");
		break;
	case COMPACT_SKIPPED:
		trace_printk("DPU: Compaction skipped\n");
		break;
	case COMPACT_FAILED:
		trace_printk("DPU: Compaction failed\n");
		break;
	default:
		break;
	}

	return ret;
}