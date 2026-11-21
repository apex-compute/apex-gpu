// SPDX-License-Identifier: GPL-2.0-only
/*
 * Render memory, GPU VMs and command-processor queues.
 *
 * The kernel owns LOCAL allocation, VMIDs and page tables, queue slots,
 * status pages, doorbell mappings, fences and the native reset. It never
 * parses user command streams: user queues append packets to their rings and
 * write doorbells directly. The kernel ring (queue 0) carries the kernel's own
 * TLB_INVALIDATE, VM_DRAIN, FILL and KFENCE packets.
 *
 * Lock order: ctx->lock -> cp->hw_lock (read) -> cp->kring_lock ->
 * cp->lock -> cp->fence_lock, cp->reg_lock. dma_fence locks are never taken
 * while holding cp->fence_lock.
 */

#include <linux/bitmap.h>
#include <linux/delay.h>
#include <linux/dma-fence.h>
#include <linux/dma-fence-chain.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/iosys-map.h>
#include <linux/kref.h>
#include <linux/log2.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/rwsem.h>
#include <linux/fdtable.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <drm/drm_auth.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include "apex_compat.h"
#if APEX_HAS_DRM_MODE_SIZE_DUMB
#include <drm/drm_dumb_buffers.h>
#endif
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_mm.h>
#include <drm/drm_prime.h>
#include <drm/drm_syncobj.h>
#include <drm/drm_vma_manager.h>

#include "apex_cp.h"
#include "apex_cp_packet.h"
#include "apex_drm.h"

/* BAR0 command-processor block. */
#define CP_ID 0x0500
#define CP_CAPS 0x0504
#define CP_CONTROL 0x0508
#define CP_STATUS 0x050c
#define CP_CONTEXT_LOW 0x0510
#define CP_CONTEXT_HIGH 0x0514
#define CP_WAVE_WATCHDOG 0x0518 /* cycles a wave may run, nonzero */
#define CP_QUEUE_SELECT 0x0520
#define CP_QUEUE_CONFIG 0x0524
#define CP_RING_LOW 0x0528
#define CP_RING_HIGH 0x052c
#define CP_RING_SIZE 0x0530
#define CP_STATUS_PAGE_LOW 0x0534
#define CP_STATUS_PAGE_HIGH 0x0538
#define CP_QUEUE_RPTR 0x053c
#define CP_QUEUE_WPTR 0x0540
#define CP_QUEUE_STATE 0x0544
#define CP_QUEUE_COMMAND 0x0548
#define CP_FAULT_QUEUE 0x0550 /* queue 5:0, valid 31 */
#define CP_FAULT_CAUSE 0x0554 /* unit 3:0, status 15:8 */
#define CP_FAULT_ADDRESS_LOW 0x0558
#define CP_FAULT_ADDRESS_HIGH 0x055c
#define CP_IRQ_PENDING_LOW 0x0560
#define CP_IRQ_PENDING_HIGH 0x0564
#define CP_IRQ_MASK_LOW 0x0568
#define CP_IRQ_MASK_HIGH 0x056c
#define CP_COUNTER_SELECT 0x0570
#define CP_COUNTER_LOW 0x0574
#define CP_COUNTER_PACKETS 1 /* packets, draws, dispatches, passes: 1-4 */
#define CP_COUNTER_PASSES 4
#define CP_PRIVATE_LOW 0x0580
#define CP_PRIVATE_HIGH 0x0584

#define CP_ID_VALUE 0x50435041
#define CP_CONTROL_ENABLE BIT(0)
#define CP_CONTROL_HALT BIT(1)
#define CP_CONTROL_RESET BIT(2)
#define CP_STATUS_IDLE BIT(1)
#define CP_STATUS_RESET_DONE BIT(3)
#define CP_STATUS_STUCK BIT(4)
#define CP_QUEUE_ENABLED BIT(0)
#define CP_QUEUE_ACTIVE BIT(1)
#define CP_QUEUE_WAITING BIT(2)
#define CP_QUEUE_DRAINED BIT(4)
#define CP_COMMAND_ENABLE BIT(1)
#define CP_COMMAND_DISABLE BIT(2)
#define CP_COMMAND_ABORT BIT(3)
#define CP_COMMAND_CLEAR_FAULT BIT(4)
#define CP_FAULT_VALID BIT(31)
#define CP_FAULT_QUEUE_MASK GENMASK(5, 0)

#define CP_QUEUES 64
#define CP_VMS 64
#define CP_QUEUES_PER_FILE 8
#define CP_PRIVATE_BYTES SZ_128K
#define CP_PRIVATE_ALIGN SZ_64K
#define CP_CONTEXT_BYTES SZ_4K
#define CP_TIMEBASE 0x001c /* BAR0 timestamp clock, Hz */
#define BAR2_FAULT 0x0204 /* first BAR2 backing fault since transport reset */
#define BAR2_FAULT_ADDRESS_LOW 0x0208
#define BAR2_FAULT_ADDRESS_HIGH 0x020c

#define KRING_BYTES SZ_64K
#define KRING_DWORDS (KRING_BYTES / 4)
#define KRING_LOG2 4 /* 2^(12+4) bytes */

#define DRAIN_TIMEOUT msecs_to_jiffies(5000)
#define FENCE_SCAN msecs_to_jiffies(100)

/* A fence deadline missed without progress faults the queue. The default, 0,
 * is 10 s at the U50's 250 MHz GPU clock scaled to the image's clock
 * (CP_TIMEBASE), so the 50 MHz Kintex-7 gets 50 s. While the CP keeps issuing
 * work the deadline restarts, up to FENCE_PROGRESS_LIMIT deadlines. */
static unsigned int apex_fence_timeout_ms;
module_param_named(fence_timeout_ms, apex_fence_timeout_ms, uint, 0444);
MODULE_PARM_DESC(fence_timeout_ms,
		 "deadline of each queue fence in ms (default 0: 10000 at 250 MHz, scaled to the GPU clock; minimum 100)");
#define FENCE_TIMEOUT_MS 10000ULL
#define FENCE_TIMEOUT_HZ 250000000ULL
#define FENCE_PROGRESS_LIMIT 32
#define HALT_TIMEOUT_US 100000
#define QUIESCE_TIMEOUT_US 1000000
#define RESET_CHECK_TIMEOUT msecs_to_jiffies(1000)
#define RESET_TIMEOUT_US 1000000

/* Page tables: 40-bit VA, 8 KiB root indexed by VA[39:30], 4 KiB tables
 * indexed by VA[29:21] and VA[20:12]; P4 PTE format. */
#define PTE_VALID BIT_ULL(0)
#define PTE_LEAF BIT_ULL(1)
#define PTE_READ BIT_ULL(2)
#define PTE_WRITE BIT_ULL(3)
#define PTE_EXEC BIT_ULL(4)
#define PTE_NULL BIT_ULL(5)
#define PTE_SYSTEM BIT_ULL(6)
#define PTE_ADDRESS GENMASK_ULL(51, 12)
#define VA_LIMIT BIT_ULL(40)
#define VA_USER_BASE SZ_2M
#define PHYSICAL_LIMIT BIT_ULL(52)

struct apex_pt {
	struct drm_mm_node node;
	DECLARE_BITMAP(large, 512); /* level 1: entries holding 2 MiB leaves */
	struct apex_pt *child[];    /* root 1024, level 1 512, leaf none */
};

struct apex_mapping {
	struct drm_mm_node va;
	struct list_head link;
	struct drm_gem_object *object; /* NULL for NULL leaves */
	u32 flags;
};

struct apex_cp;
struct apex_ctx;

/* The mmap handle of a status or doorbell page. */
struct apex_page_bo {
	struct drm_gem_object base;
	struct mutex lock;
	struct apex_cp *cp;
	struct drm_file *file;
	void *cpu;         /* status page, or NULL for a doorbell */
	dma_addr_t dma;
	phys_addr_t phys;  /* doorbell page */
	bool revoked;
};

struct apex_queue {
	struct apex_cp *cp;
	struct apex_ctx *ctx; /* NULL for the kernel ring */
	u32 slot, index;
	void *status;
	dma_addr_t status_dma;
	u64 status_va;
	struct apex_page_bo *status_bo, *doorbell_bo;
	/* The kwait word in LOCAL at kwait_va: CP WAITs must not read SYSTEM. */
	struct drm_mm_node kwait_node;
	u64 kwait_va;
	u64 fence_context, fence_seq;
	u64 kwait;
	struct list_head fences; /* cp->fence_lock */
	struct list_head waits;  /* cp->fence_lock */
	bool faulted, lost, timed_out;
};

struct apex_ctx {
	struct apex_cp *cp;
	struct drm_device *drm;
	struct mutex lock;
	u32 vmid;
	struct apex_pt *root;
	struct drm_mm va;
	struct apex_queue *queues[CP_QUEUES_PER_FILE];
	atomic_t error;
	bool gpu_used;
	/* The opening process, named in fault reports. */
	char comm[TASK_COMM_LEN];
	pid_t pid;
};

struct apex_fence {
	struct dma_fence base;
	spinlock_t lock;
	struct list_head link;
	struct apex_queue *queue;
	u64 target;
	u64 order;		/* attach order across the device */
	unsigned long start;	/* the deadline counts from here */
	unsigned long head;	/* became the timed fence: bounds progress restarts */
	int error;
};

/* queue is cleared (under fence_lock) by whoever takes ownership: the
 * callback, or a detach that then removes the callback before freeing. */
struct apex_wait {
	struct dma_fence_cb cb;
	struct dma_fence *fence;
	struct apex_cp *cp;
	struct apex_queue *queue;
	struct list_head link;
	u64 value;
};

struct apex_extent {
	struct drm_mm_node node;
	struct list_head link; /* apex_local_bo.extents, in object order */
};

/* LOCAL backing is one range when a hole fits the object; otherwise pieces
 * halved down to 4 KiB, which the page tables and CPU mappings stitch. */
struct apex_local_bo {
	struct drm_gem_object base;
	struct apex_cp *cp;
	struct list_head extents;
	struct list_head link; /* cp->local_bos, cp->lock */
};

struct apex_cp {
	struct kref ref;
	struct pci_dev *pdev;
	struct device *dev;
	void __iomem *bar0;
	void __iomem *bar2; /* LOCAL [0, usable), write-combined */
	void __iomem *bar4;
	phys_addr_t bar2_phys, bar4_phys;
	const struct apex_cp_ops *ops;
	void *owner;
	u64 usable, visible;
	u32 profile;
	u32 caps;
	u32 timestamp_hz;
	bool present;     /* command processor usable */
	bool master;
	bool hw_gone;     /* no MMIO after remove; hw_lock write, fence_lock */
	bool failed;      /* admission closed */
	bool quarantined; /* device may still access backing: leak host memory */

	struct rw_semaphore hw_lock;
	/* Held for write while user CPU mappings of LOCAL are revoked around a
	 * native reset; the fault handler maps them again under read. */
	struct rw_semaphore map_lock;
	struct mutex lock;
	struct drm_mm mm;
	u64 local_used;             /* cp->lock */
	struct list_head local_bos; /* cp->lock */
	DECLARE_BITMAP(vmids, CP_VMS);
	DECLARE_BITMAP(slot_used, CP_QUEUES); /* cp->lock */
	struct apex_queue *slots[CP_QUEUES];  /* published under fence_lock */
	struct drm_mm_node context_node, private_node;

	struct mutex kring_lock;
	u32 *kring;
	dma_addr_t kring_dma;
	u32 kwptr;
	u64 kseq;
	struct apex_queue kq;

	spinlock_t fence_lock;
	spinlock_t reg_lock;
	wait_queue_head_t wait;
	struct delayed_work fence_work;
	struct work_struct reset_work;
	struct work_struct fail_work;
	unsigned int pending_fences;
	u64 fence_order;
	u32 issued; /* fence_work: last sum of the CP's issue counters */
};

static const struct drm_gem_object_funcs apex_local_funcs;
static const struct drm_gem_object_funcs apex_system_funcs;
static const struct drm_gem_object_funcs apex_page_funcs;

static void cp_free(struct kref *ref);

/* ---------------------------------------------------------------- basics */

static struct apex_cp *cp_get(struct apex_cp *cp)
{
	kref_get(&cp->ref);
	return cp;
}

void apex_cp_put(struct apex_cp *cp)
{
	if (cp)
		kref_put(&cp->ref, cp_free);
}

static bool cp_hw_enter(struct apex_cp *cp)
{
	down_read(&cp->hw_lock);
	if (cp->hw_gone) {
		up_read(&cp->hw_lock);
		return false;
	}
	return true;
}

static void cp_hw_exit(struct apex_cp *cp)
{
	up_read(&cp->hw_lock);
}

static u64 status_u64(struct apex_queue *q, unsigned int offset)
{
	return READ_ONCE(*(u64 *)((u8 *)q->status + offset));
}

static u32 status_u32(struct apex_queue *q, unsigned int offset)
{
	return READ_ONCE(*(u32 *)((u8 *)q->status + offset));
}

static void ctx_fail(struct apex_ctx *ctx, int error)
{
	if (ctx)
		atomic_cmpxchg(&ctx->error, 0, error);
}

static void cp_queue_command(struct apex_cp *cp, u32 slot, u32 command)
{
	unsigned long flags;

	spin_lock_irqsave(&cp->reg_lock, flags);
	writel(slot, cp->bar0 + CP_QUEUE_SELECT);
	writel(command, cp->bar0 + CP_QUEUE_COMMAND);
	spin_unlock_irqrestore(&cp->reg_lock, flags);
}

static u32 cp_queue_state(struct apex_cp *cp, u32 slot)
{
	unsigned long flags;
	u32 state;

	spin_lock_irqsave(&cp->reg_lock, flags);
	writel(slot, cp->bar0 + CP_QUEUE_SELECT);
	state = readl(cp->bar0 + CP_QUEUE_STATE);
	spin_unlock_irqrestore(&cp->reg_lock, flags);
	return state;
}

/* A unit or the abort sequencer reported itself stuck: native reset. */
static bool cp_stuck(struct apex_cp *cp)
{
	u32 status = readl(cp->bar0 + CP_STATUS);

	return status != U32_MAX && (status & CP_STATUS_STUCK);
}

static unsigned long cp_fence_timeout(const struct apex_cp *cp)
{
	u64 ms = apex_fence_timeout_ms;

	if (!ms)
		ms = div64_u64(FENCE_TIMEOUT_MS * FENCE_TIMEOUT_HZ,
			       min_t(u64, max(cp->timestamp_hz, 1U), FENCE_TIMEOUT_HZ));
	return msecs_to_jiffies(max_t(u64, ms, 100));
}

static void cp_queue_program(struct apex_cp *cp, struct apex_queue *q, u32 vmid,
			     u32 priority, u64 ring, u32 ring_log2)
{
	unsigned long flags;

	spin_lock_irqsave(&cp->reg_lock, flags);
	writel(q->slot, cp->bar0 + CP_QUEUE_SELECT);
	writel(vmid | (priority << 8), cp->bar0 + CP_QUEUE_CONFIG);
	writel(lower_32_bits(ring), cp->bar0 + CP_RING_LOW);
	writel(upper_32_bits(ring), cp->bar0 + CP_RING_HIGH);
	writel(ring_log2, cp->bar0 + CP_RING_SIZE);
	writel(lower_32_bits(q->status_dma), cp->bar0 + CP_STATUS_PAGE_LOW);
	writel(upper_32_bits(q->status_dma), cp->bar0 + CP_STATUS_PAGE_HIGH);
	writel(CP_COMMAND_ENABLE, cp->bar0 + CP_QUEUE_COMMAND);
	spin_unlock_irqrestore(&cp->reg_lock, flags);
}

/* Remove unmaps BAR2 while files may still be open, so kernel stores through
 * it run inside cp_hw_enter() or under fence_lock with hw_gone clear. */
static void __iomem *local_map(struct apex_cp *cp, u64 address)
{
	return cp->bar2 + address;
}

static int local_alloc(struct apex_cp *cp, struct drm_mm_node *node, u64 bytes,
		       u64 alignment)
{
	int ret;

	mutex_lock(&cp->lock);
	if (cp->failed)
		ret = -EIO;
	else
		ret = drm_mm_insert_node_generic(&cp->mm, node, bytes, alignment, 0,
						 DRM_MM_INSERT_BEST);
	if (!ret)
		cp->local_used += bytes;
	mutex_unlock(&cp->lock);
	return ret;
}

/* After a failure the allocator stays closed until reload, so removing the
 * bookkeeping never hands LOCAL the device may still address to a user. */
static void local_free(struct apex_cp *cp, struct drm_mm_node *node)
{
	if (!drm_mm_node_allocated(node))
		return;
	mutex_lock(&cp->lock);
	cp->local_used -= node->size;
	drm_mm_remove_node(node);
	mutex_unlock(&cp->lock);
}

static void local_extents_free(struct apex_cp *cp, struct list_head *extents)
{
	struct apex_extent *extent, *next;

	mutex_lock(&cp->lock);
	list_for_each_entry_safe(extent, next, extents, link) {
		cp->local_used -= extent->node.size;
		drm_mm_remove_node(&extent->node);
		list_del(&extent->link);
		kfree(extent);
	}
	mutex_unlock(&cp->lock);
}

/* Back `bytes` with one range when a hole fits; otherwise, unless the object
 * must be contiguous, halve the piece on each miss down to 4 KiB. */
static int local_extents_alloc(struct apex_cp *cp, struct list_head *extents, u64 bytes,
			       bool contiguous)
{
	u64 piece = bytes, left = bytes;
	int ret = 0;

	mutex_lock(&cp->lock);
	if (cp->failed)
		ret = -EIO;
	else if (bytes > cp->usable - cp->local_used)
		ret = -ENOSPC;
	while (!ret && left) {
		u64 size = min(piece, left);
		struct apex_extent *extent = kzalloc(sizeof(*extent), GFP_KERNEL);

		if (!extent) {
			ret = -ENOMEM;
			break;
		}
		ret = drm_mm_insert_node_generic(&cp->mm, &extent->node, size,
						 size >= SZ_2M ? SZ_2M : SZ_4K, 0,
						 DRM_MM_INSERT_BEST);
		if (ret) {
			kfree(extent);
			if (ret != -ENOSPC || contiguous || size == SZ_4K)
				break;
			piece = size >= SZ_4M ? round_down(size / 2, SZ_2M) :
					       max_t(u64, round_down(size / 2, SZ_4K), SZ_4K);
			ret = 0;
			continue;
		}
		cp->local_used += size;
		list_add_tail(&extent->link, extents);
		left -= size;
	}
	mutex_unlock(&cp->lock);
	if (ret)
		local_extents_free(cp, extents);
	return ret;
}

/* Revoke every user CPU mapping of LOCAL: host BAR2 accesses stop once this
 * returns, and the next access faults (cp->map_lock orders it after a reset). */
static void local_unmap_all(struct apex_cp *cp)
{
	struct apex_local_bo *bo;

	mutex_lock(&cp->lock);
	list_for_each_entry(bo, &cp->local_bos, link)
		drm_vma_node_unmap(&bo->base.vma_node, bo->base.dev->anon_inode->i_mapping);
	mutex_unlock(&cp->lock);
}

/* ------------------------------------------------------------- fences */

static const char *apex_fence_driver(struct dma_fence *fence)
{
	return "apex";
}

static const char *apex_fence_timeline(struct dma_fence *fence)
{
	return "apex-queue";
}

static const struct dma_fence_ops apex_fence_ops = {
	.get_driver_name = apex_fence_driver,
	.get_timeline_name = apex_fence_timeline,
};

/* fence_lock held. Only a queue's oldest fence is timed, and its deadline
 * counts from the later of its attach and the end of the last fence attached
 * before it, of its own queue or of another: until then it waits behind that
 * work rather than running. An ended fence restarts the clock of every
 * queue's oldest fence attached after it. */
static void fences_rearm(struct apex_cp *cp, u64 order)
{
	int q;

	for (q = 0; q < CP_QUEUES; q++) {
		struct apex_queue *queue = cp->slots[q];
		struct apex_fence *head;

		if (!queue || list_empty(&queue->fences))
			continue;
		head = list_first_entry(&queue->fences, struct apex_fence, link);
		if (head->order > order)
			head->start = head->head = jiffies;
	}
}

/* fence_lock held. Moves completed (or, with error, all) fences to done. */
static void queue_collect(struct apex_cp *cp, struct apex_queue *q, int error,
			  struct list_head *done)
{
	struct apex_fence *fence, *next;
	u64 kfence = status_u64(q, APEX_CP_STATUS_KFENCE);
	u64 order = 0;

	list_for_each_entry_safe(fence, next, &q->fences, link) {
		if (!error && fence->target > kfence)
			continue;
		fence->error = fence->target > kfence ? error : 0;
		fence->queue = NULL;
		order = max(order, fence->order);
		list_move_tail(&fence->link, done);
		cp->pending_fences--;
	}
	if (order)
		fences_rearm(cp, order);
}

static void fences_signal(struct list_head *done)
{
	struct apex_fence *fence, *next;

	list_for_each_entry_safe(fence, next, done, link) {
		list_del_init(&fence->link);
		if (fence->error)
			dma_fence_set_error(&fence->base, fence->error);
		dma_fence_signal(&fence->base);
		dma_fence_put(&fence->base);
	}
}

/* fence_lock held. Reads a queue's status page after an interrupt or scan. */
static void queue_service(struct apex_cp *cp, struct apex_queue *q,
			  struct list_head *done)
{
	u32 state = status_u32(q, APEX_CP_STATUS_STATE);

	if ((state & APEX_CP_STATE_FAULTED) && !q->faulted) {
		q->faulted = true;
		dev_warn_ratelimited(cp->dev,
			"queue %u (%s[%d]) faulted: status %u unit %u address %#llx packet %u\n",
			q->slot, q->ctx ? q->ctx->comm : "kernel", q->ctx ? q->ctx->pid : 0,
			status_u32(q, APEX_CP_STATUS_FAULT_STATUS),
			status_u32(q, APEX_CP_STATUS_FAULT_UNIT),
			status_u64(q, APEX_CP_STATUS_FAULT_ADDRESS),
			status_u32(q, APEX_CP_STATUS_FAULT_PACKET));
		ctx_fail(q->ctx, -EIO);
		if (!q->ctx)
			schedule_work(&cp->reset_work);
	}
	queue_collect(cp, q, q->faulted || q->lost ? -EIO : 0, done);
}

irqreturn_t apex_cp_irq(int irq, void *data)
{
	struct apex_cp *cp = data;
	unsigned long flags;
	LIST_HEAD(done);
	u64 pending;
	u32 low, high;
	int q;

	if (READ_ONCE(cp->hw_gone) || !READ_ONCE(cp->present))
		return IRQ_NONE;
	low = readl(cp->bar0 + CP_IRQ_PENDING_LOW);
	high = readl(cp->bar0 + CP_IRQ_PENDING_HIGH);
	if (!(low | high) || low == U32_MAX)
		return IRQ_NONE;
	writel(low, cp->bar0 + CP_IRQ_PENDING_LOW);
	writel(high, cp->bar0 + CP_IRQ_PENDING_HIGH);
	pending = low | ((u64)high << 32);
	spin_lock_irqsave(&cp->fence_lock, flags);
	for_each_set_bit(q, (unsigned long *)&pending, CP_QUEUES)
		if (cp->slots[q])
			queue_service(cp, cp->slots[q], &done);
	spin_unlock_irqrestore(&cp->fence_lock, flags);
	fences_signal(&done);
	wake_up_all(&cp->wait);
	return IRQ_HANDLED;
}

/* hw_lock held for read, hardware present. Whether the CP issued a packet,
 * draw, dispatch or pass since the last scan, and the queues then waiting on
 * a WAIT, whose deadlines progress elsewhere does not restart. */
/* reg_lock held: packets, draws, dispatches and passes the CP issued. */
static u32 cp_issued_locked(struct apex_cp *cp)
{
	u32 issued = 0;
	int c;

	for (c = CP_COUNTER_PACKETS; c <= CP_COUNTER_PASSES; c++) {
		writel(c, cp->bar0 + CP_COUNTER_SELECT);
		issued += readl(cp->bar0 + CP_COUNTER_LOW);
	}
	return issued;
}

static u32 cp_issued(struct apex_cp *cp)
{
	unsigned long flags;
	u32 issued;

	spin_lock_irqsave(&cp->reg_lock, flags);
	issued = cp_issued_locked(cp);
	spin_unlock_irqrestore(&cp->reg_lock, flags);
	return issued;
}

static bool cp_progress(struct apex_cp *cp, unsigned long *waiting)
{
	unsigned long flags;
	u32 issued;
	bool progress;
	int q;

	spin_lock_irqsave(&cp->reg_lock, flags);
	issued = cp_issued_locked(cp);
	progress = issued != cp->issued;
	cp->issued = issued;
	for (q = 0; progress && q < CP_QUEUES; q++) {
		if (!READ_ONCE(cp->slots[q]))
			continue;
		writel(q, cp->bar0 + CP_QUEUE_SELECT);
		if (readl(cp->bar0 + CP_QUEUE_STATE) & CP_QUEUE_WAITING)
			set_bit(q, waiting);
	}
	spin_unlock_irqrestore(&cp->reg_lock, flags);
	return progress;
}

/* Missed interrupts, deadlines and escalation: a 100 ms scan while fences
 * are pending. A deadline missed without progress faults the queue and fails
 * the fence. */
static void cp_fence_work(struct work_struct *work)
{
	struct apex_cp *cp = container_of(to_delayed_work(work), struct apex_cp,
					  fence_work);
	struct apex_fence *fence, *next;
	DECLARE_BITMAP(abort, CP_QUEUES);
	DECLARE_BITMAP(waiting, CP_QUEUES);
	unsigned long flags, timeout = cp_fence_timeout(cp);
	bool again, progress = false;
	LIST_HEAD(done);
	u64 order = 0;
	int q;

	bitmap_zero(abort, CP_QUEUES);
	bitmap_zero(waiting, CP_QUEUES);
	if (down_read_trylock(&cp->hw_lock)) {
		if (!cp->hw_gone && cp->present)
			progress = cp_progress(cp, waiting);
		up_read(&cp->hw_lock);
	}
	spin_lock_irqsave(&cp->fence_lock, flags);
	for (q = 0; q < CP_QUEUES; q++) {
		struct apex_queue *queue = cp->slots[q];

		if (!queue)
			continue;
		queue_service(cp, queue, &done);
		if (list_empty(&queue->fences))
			continue;
		fence = list_first_entry(&queue->fences, struct apex_fence, link);
		if (progress && !test_bit(q, waiting) &&
		    time_before(jiffies, fence->head + FENCE_PROGRESS_LIMIT * timeout))
			fence->start = jiffies;
		if (!queue->timed_out && time_before(jiffies, fence->start + timeout))
			continue;
		list_for_each_entry_safe(fence, next, &queue->fences, link) {
			if (!queue->timed_out) {
				queue->timed_out = true;
				set_bit(q, abort);
				ctx_fail(queue->ctx, -ETIMEDOUT);
				dev_warn_ratelimited(cp->dev,
					"queue %u fence %llu missed its deadline at kfence %llu: aborting the queue\n",
					q, fence->target, status_u64(queue, APEX_CP_STATUS_KFENCE));
			}
			fence->error = -ETIMEDOUT;
			fence->queue = NULL;
			order = max(order, fence->order);
			list_move_tail(&fence->link, &done);
			cp->pending_fences--;
		}
	}
	if (order)
		fences_rearm(cp, order);
	again = cp->pending_fences && !cp->hw_gone;
	spin_unlock_irqrestore(&cp->fence_lock, flags);
	fences_signal(&done);
	if (down_read_trylock(&cp->hw_lock)) {
		if (!cp->hw_gone && cp->present) {
			for_each_set_bit(q, abort, CP_QUEUES)
				cp_queue_command(cp, q, CP_COMMAND_ABORT);
			if (cp_stuck(cp))
				schedule_work(&cp->reset_work);
		}
		up_read(&cp->hw_lock);
	}
	if (again)
		schedule_delayed_work(&cp->fence_work, FENCE_SCAN);
}

/* Fail every pending fence of every queue. */
static void cp_fail_fences(struct apex_cp *cp, int error)
{
	unsigned long flags;
	LIST_HEAD(done);
	int q;

	spin_lock_irqsave(&cp->fence_lock, flags);
	for (q = 0; q < CP_QUEUES; q++) {
		struct apex_queue *queue = cp->slots[q];

		if (!queue)
			continue;
		queue->lost = true;
		ctx_fail(queue->ctx, error);
		queue_collect(cp, queue, error, &done);
	}
	spin_unlock_irqrestore(&cp->fence_lock, flags);
	fences_signal(&done);
	wake_up_all(&cp->wait);
}

static void cp_fail_work(struct work_struct *work)
{
	struct apex_cp *cp = container_of(work, struct apex_cp, fail_work);

	local_unmap_all(cp);
	cp_fail_fences(cp, -EIO);
}

void apex_cp_poison(struct apex_cp *cp)
{
	if (!cp)
		return;
	WRITE_ONCE(cp->failed, true);
	WRITE_ONCE(cp->quarantined, true);
	wake_up_all(&cp->wait);
	if (!READ_ONCE(cp->hw_gone))
		schedule_work(&cp->fail_work);
}

/* ----------------------------------------------------------- kernel ring */

static bool kring_done(struct apex_cp *cp, u64 seq)
{
	return status_u64(&cp->kq, APEX_CP_STATUS_KFENCE) >= seq ||
	       READ_ONCE(cp->failed) || READ_ONCE(cp->kq.faulted);
}

static void cp_quarantine(struct apex_cp *cp)
{
	dev_crit(cp->dev,
		 "native reset failed; LOCAL and host backing quarantined until reload\n");
	WRITE_ONCE(cp->failed, true);
	WRITE_ONCE(cp->quarantined, true);
	local_unmap_all(cp);
	cp->ops->failed(cp->owner);
	cp_fail_fences(cp, -EIO);
}

static void kring_enable(struct apex_cp *cp)
{
	memset(cp->kq.status, 0, PAGE_SIZE);
	cp->kwptr = 0;
	cp->kq.faulted = false;
	cp_queue_program(cp, &cp->kq, 0, 0,
			 cp->kring_dma | APEX_CP_SYSTEM_ADDRESS, KRING_LOG2);
}

static void cp_program(struct apex_cp *cp)
{
	writel(lower_32_bits(cp->context_node.start), cp->bar0 + CP_CONTEXT_LOW);
	writel(upper_32_bits(cp->context_node.start), cp->bar0 + CP_CONTEXT_HIGH);
	writel(lower_32_bits(cp->private_node.start), cp->bar0 + CP_PRIVATE_LOW);
	writel(upper_32_bits(cp->private_node.start), cp->bar0 + CP_PRIVATE_HIGH);
	writel(0, cp->bar0 + CP_IRQ_MASK_LOW);
	writel(0, cp->bar0 + CP_IRQ_MASK_HIGH);
	/* The Kintex-7's one core at 50 MHz runs long fragment loops for tens of
	 * seconds a wave; the progress-aware fence deadline still catches a hang. */
	if (cp->profile == APEX_PROFILE_K7)
		writel(U32_MAX, cp->bar0 + CP_WAVE_WATCHDOG);
	writel(CP_CONTROL_ENABLE, cp->bar0 + CP_CONTROL);
	kring_enable(cp);
}

#define KFENCE_DWORDS 3

/* Append packets and a KFENCE to queue 0, which has room for them, and ring
 * its doorbell (kring_lock held). Returns the fence's sequence. */
static u64 kring_append(struct apex_cp *cp, const u32 *packets, unsigned int count)
{
	u32 fence[KFENCE_DWORDS];
	unsigned int n, i;
	u64 seq = ++cp->kseq;

	n = apex_cp_kfence(fence, seq);
	for (i = 0; i < count; i++)
		cp->kring[(cp->kwptr + i) % KRING_DWORDS] = packets[i];
	for (i = 0; i < n; i++)
		cp->kring[(cp->kwptr + count + i) % KRING_DWORDS] = fence[i];
	cp->kwptr += count + n;
	wmb(); /* ring and page-table stores before the doorbell */
	writel(cp->kwptr, cp->bar4 + APEX_CP_DOORBELL_WPTR);
	return seq;
}

/* Wait until the kernel ring's fence reaches seq or the deadline passes. */
static bool kring_wait(struct apex_cp *cp, u64 seq, unsigned long deadline)
{
	unsigned int i;

	for (i = 0; i < 50 && !kring_done(cp, seq); i++)
		udelay(1);
	while (!kring_done(cp, seq) && !time_after(jiffies, deadline))
		wait_event_timeout(cp->wait, kring_done(cp, seq), 1);
	return status_u64(&cp->kq, APEX_CP_STATUS_KFENCE) >= seq;
}

/* The queues holding work when a native reset starts: queue state, CP and
 * status-page positions and fences (kring_lock held). */
static void cp_reset_report(struct apex_cp *cp)
{
	unsigned long flags;
	int q;

	for (q = 0; q < CP_QUEUES; q++) {
		struct apex_queue *queue = cp->slots[q];
		u32 state, rptr, wptr;

		if (!queue)
			continue;
		spin_lock_irqsave(&cp->reg_lock, flags);
		writel(q, cp->bar0 + CP_QUEUE_SELECT);
		state = readl(cp->bar0 + CP_QUEUE_STATE);
		rptr = readl(cp->bar0 + CP_QUEUE_RPTR);
		wptr = readl(cp->bar0 + CP_QUEUE_WPTR);
		spin_unlock_irqrestore(&cp->reg_lock, flags);
		if (rptr == wptr && !(state & (CP_QUEUE_ACTIVE | CP_QUEUE_WAITING)))
			continue;
		dev_err(cp->dev, "queue %d state %#x rptr %u wptr %u; status page rptr %u kfence %llu, last kernel fence %llu\n",
			q, state, rptr, wptr, status_u32(queue, APEX_CP_STATUS_RPTR),
			status_u64(queue, APEX_CP_STATUS_KFENCE),
			q ? queue->fence_seq : cp->kseq);
	}
}

/* Native reset (kring_lock and hw held). Success proves that no GPU access
 * to any backing remains: every user queue is lost and queue 0 restarts. */
static int cp_reset_locked(struct apex_cp *cp)
{
	u32 status;
	int q, ret;

	if (cp->failed)
		return -EIO;
	dev_err(cp->dev, "command processor native reset: status %#x first fault %#x cause %#x address %#x%08x\n",
		readl(cp->bar0 + CP_STATUS), readl(cp->bar0 + CP_FAULT_QUEUE),
		readl(cp->bar0 + CP_FAULT_CAUSE), readl(cp->bar0 + CP_FAULT_ADDRESS_HIGH),
		readl(cp->bar0 + CP_FAULT_ADDRESS_LOW));
	cp_reset_report(cp);

	/* The memory system drops in-flight BAR2, host DMA and display accesses:
	 * revoke user BAR2 mappings, stop the parser, abort every user queue's
	 * work (the copy engine and DMA cancel their segments) and wait for the
	 * units to drain, then stop scanout. */
	down_write(&cp->map_lock);
	local_unmap_all(cp);
	writel(CP_CONTROL_ENABLE | CP_CONTROL_HALT, cp->bar0 + CP_CONTROL);
	for (q = 1; q < CP_QUEUES; q++)
		if (cp->slots[q])
			cp_queue_command(cp, q, CP_COMMAND_ABORT);
	if (readl_poll_timeout(cp->bar0 + CP_STATUS, status,
			       status == U32_MAX || (status & CP_STATUS_IDLE),
			       100, QUIESCE_TIMEOUT_US))
		dev_err(cp->dev, "units did not drain before the native reset (status %#x)\n",
			status);
	cp->ops->reset_begin(cp->owner);
	writel(CP_CONTROL_RESET, cp->bar0 + CP_CONTROL);
	ret = readl_poll_timeout(cp->bar0 + CP_STATUS, status,
				 status == U32_MAX || (status & CP_STATUS_RESET_DONE),
				 100, RESET_TIMEOUT_US);
	if (ret || status == U32_MAX) {
		cp->ops->reset_end(cp->owner, false);
		cp_quarantine(cp);
		up_write(&cp->map_lock);
		return -EIO;
	}
	cp_fail_fences(cp, -EIO);
	/* The reset disabled every queue; re-arm the first-fault latch. */
	status = readl(cp->bar0 + CP_FAULT_QUEUE);
	if (status & CP_FAULT_VALID)
		cp_queue_command(cp, status & CP_FAULT_QUEUE_MASK, CP_COMMAND_CLEAR_FAULT);
	{
		unsigned long flags;

		spin_lock_irqsave(&cp->fence_lock, flags);
		for (q = 1; q < CP_QUEUES; q++)
			if (cp->slots[q])
				WRITE_ONCE(*(u32 *)((u8 *)cp->slots[q]->status +
						    APEX_CP_STATUS_STATE),
					   APEX_CP_STATE_RESET);
		cp->kq.lost = false;
		spin_unlock_irqrestore(&cp->fence_lock, flags);
	}
	cp_program(cp);
	/* reset_done covers HBM only: a kernel-ring KFENCE proves the parser and
	 * the SYSTEM path before any backing returns. A BAR2 access the reset
	 * dropped latches the BAR2 fault, after which BAR2 drops every write
	 * (page tables included) until transport reset. */
	status = readl(cp->bar0 + BAR2_FAULT) & 0xf;
	if (status)
		dev_err(cp->dev, "BAR2 faulted (%#x at %#x%08x); reload the driver to reset the transport\n",
			status, readl(cp->bar0 + BAR2_FAULT_ADDRESS_HIGH),
			readl(cp->bar0 + BAR2_FAULT_ADDRESS_LOW));
	if (status || !kring_wait(cp, kring_append(cp, NULL, 0), jiffies + RESET_CHECK_TIMEOUT)) {
		if (!status)
			dev_err(cp->dev, "command processor did not run after the native reset\n");
		cp->ops->reset_end(cp->owner, false);
		cp_quarantine(cp);
		up_write(&cp->map_lock);
		return -EIO;
	}
	cp->ops->reset_end(cp->owner, true);
	up_write(&cp->map_lock);
	return 0;
}

static void cp_reset_work(struct work_struct *work)
{
	struct apex_cp *cp = container_of(work, struct apex_cp, reset_work);

	if (!cp_hw_enter(cp))
		return;
	mutex_lock(&cp->kring_lock);
	if (cp->present && (READ_ONCE(cp->kq.faulted) || cp_stuck(cp)))
		cp_reset_locked(cp);
	mutex_unlock(&cp->kring_lock);
	cp_hw_exit(cp);
}

/* Append packets and a KFENCE to queue 0 and wait for it (hw and kring_lock
 * held). Returns 0 once the packets completed, -ECANCELED when a native reset
 * proved instead that no GPU work remains, and -EIO when backing must stay
 * quarantined. A VM_DRAIN waits for work its VM already issued, so the wait
 * has a fence's deadline, restarted like a fence's while the CP issues work:
 * a Kintex-7 queue can run for minutes ahead of it. */
static int kring_run_locked(struct apex_cp *cp, const u32 *packets, unsigned int count)
{
	unsigned long timeout = max(DRAIN_TIMEOUT, cp_fence_timeout(cp));
	unsigned long head = jiffies, deadline = head + timeout;
	u32 issued = cp_issued(cp), now;
	u64 seq;

	if (cp->failed || !cp->present)
		return -EIO;
	while (cp->kwptr - status_u32(&cp->kq, APEX_CP_STATUS_RPTR) + count +
	       KFENCE_DWORDS > KRING_DWORDS) {
		if (time_after(jiffies, deadline) || cp->kq.faulted)
			return cp_reset_locked(cp) ?: -ECANCELED;
		wait_event_timeout(cp->wait, cp->failed, 1);
		if (cp->failed)
			return -EIO;
	}
	seq = kring_append(cp, packets, count);
	while (!kring_wait(cp, seq, deadline)) {
		if (cp->failed)
			return -EIO;
		now = cp_issued(cp);
		if (cp->kq.faulted || now == issued ||
		    time_after(jiffies, head + FENCE_PROGRESS_LIMIT * timeout))
			return cp_reset_locked(cp) ?: -ECANCELED;
		issued = now;
		deadline = jiffies + timeout;
	}
	return 0;
}

static int kring_run(struct apex_cp *cp, const u32 *packets, unsigned int count)
{
	int ret;

	mutex_lock(&cp->kring_lock);
	ret = kring_run_locked(cp, packets, count);
	mutex_unlock(&cp->kring_lock);
	return ret;
}

/* Drop the VMID's translations and wait for all work it issued (hw held). */
static int kring_vm_fence(struct apex_cp *cp, u32 vmid)
{
	u32 packets[4];
	unsigned int n;

	n = apex_cp_vmid_packet(packets, APEX_CP_TLB_INVALIDATE, vmid);
	n += apex_cp_vmid_packet(packets + n, APEX_CP_VM_DRAIN, vmid);
	return kring_run(cp, packets, n);
}

/* ---------------------------------------------------------- page tables */

static struct apex_pt *pt_alloc(struct apex_cp *cp, unsigned int children,
				u64 bytes)
{
	struct apex_pt *pt = kzalloc(struct_size(pt, child, children), GFP_KERNEL);
	int ret;

	if (!pt)
		return ERR_PTR(-ENOMEM);
	ret = local_alloc(cp, &pt->node, bytes, bytes);
	if (ret) {
		kfree(pt);
		return ERR_PTR(ret);
	}
	down_read(&cp->map_lock);
	memset_io(local_map(cp, pt->node.start), 0, bytes);
	up_read(&cp->map_lock);
	return pt;
}

/* Kernel BAR2 stores hold map_lock for read, so a native reset never drops
 * one in flight. */
static void pt_write(struct apex_cp *cp, struct apex_pt *pt, unsigned int index,
		     u64 value)
{
	down_read(&cp->map_lock);
	writeq(value, local_map(cp, pt->node.start + index * 8ull));
	up_read(&cp->map_lock);
}

/* Point VMID's context entry at root, or disable it. */
static void context_write(struct apex_cp *cp, u32 vmid, u64 root, bool enabled)
{
	void __iomem *entry = local_map(cp, cp->context_node.start + vmid * 16ull);

	down_read(&cp->map_lock);
	if (enabled) {
		writeq(root, entry);
		writeq(1, entry + 8);
	} else {
		writeq(0, entry + 8);
		writeq(0, entry);
	}
	up_read(&cp->map_lock);
}

static void pt_free(struct apex_cp *cp, struct apex_pt *pt, unsigned int children)
{
	unsigned int i;

	if (!pt)
		return;
	for (i = 0; i < children; i++)
		pt_free(cp, pt->child[i], children == 1024 ? 512 : 0);
	local_free(cp, &pt->node);
	kfree(pt);
}

/* Ensure the tables above one leaf exist (ctx->lock held). */
static int pt_ensure(struct apex_ctx *ctx, u64 va, bool large)
{
	unsigned int r = (va >> 30) & 1023, m = (va >> 21) & 511;
	struct apex_pt *l1 = ctx->root->child[r], *l2;

	if (!l1) {
		l1 = pt_alloc(ctx->cp, 512, SZ_4K);
		if (IS_ERR(l1))
			return PTR_ERR(l1);
		wmb();
		pt_write(ctx->cp, ctx->root, r, l1->node.start | PTE_VALID);
		ctx->root->child[r] = l1;
	}
	if (large || l1->child[m])
		return 0;
	l2 = pt_alloc(ctx->cp, 0, SZ_4K);
	if (IS_ERR(l2))
		return PTR_ERR(l2);
	wmb();
	pt_write(ctx->cp, l1, m, l2->node.start | PTE_VALID);
	l1->child[m] = l2;
	return 0;
}

static void pt_set(struct apex_ctx *ctx, u64 va, u64 pte, bool large)
{
	struct apex_pt *l1 = ctx->root->child[(va >> 30) & 1023];
	unsigned int m = (va >> 21) & 511;

	if (large) {
		pt_write(ctx->cp, l1, m, pte);
		set_bit(m, l1->large);
	} else {
		pt_write(ctx->cp, l1->child[m], (va >> 12) & 511, pte);
	}
}

static void pt_clear(struct apex_ctx *ctx, u64 va, u64 end)
{
	while (va < end) {
		struct apex_pt *l1 = ctx->root->child[(va >> 30) & 1023];
		unsigned int m = (va >> 21) & 511;

		if (l1 && test_bit(m, l1->large)) {
			pt_write(ctx->cp, l1, m, 0);
			clear_bit(m, l1->large);
			va += SZ_2M;
			continue;
		}
		if (l1 && l1->child[m])
			pt_write(ctx->cp, l1->child[m], (va >> 12) & 511, 0);
		va += SZ_4K;
	}
}

/* Leaves for one mapping, in VA order. */
struct pte_cursor {
	u64 va, end, bits;
	struct apex_extent *extent; /* LOCAL: current extent */
	u64 extent_offset;
	struct scatterlist *sg;     /* SYSTEM: current DMA segment */
	u64 sg_offset;
	bool system, null;
};

static bool l2_present(struct apex_ctx *ctx, u64 va)
{
	struct apex_pt *l1 = ctx->root->child[(va >> 30) & 1023];

	return l1 && l1->child[(va >> 21) & 511];
}

static bool cursor_next(struct apex_ctx *ctx, struct pte_cursor *c, u64 *va,
			u64 *pte, bool *large)
{
	if (c->va >= c->end)
		return false;
	*va = c->va;
	*large = false;
	if (c->null) {
		*pte = c->bits | PTE_NULL;
		c->va += SZ_4K;
		return true;
	}
	if (!c->system) {
		u64 local, run, step;

		while (c->extent_offset >= c->extent->node.size) {
			c->extent_offset -= c->extent->node.size;
			c->extent = list_next_entry(c->extent, link);
		}
		local = c->extent->node.start + c->extent_offset;
		run = c->extent->node.size - c->extent_offset;
		*large = IS_ALIGNED(c->va | local, SZ_2M) && run >= SZ_2M &&
			 c->end - c->va >= SZ_2M && !l2_present(ctx, c->va);
		*pte = local | c->bits;
		step = *large ? SZ_2M : SZ_4K;
		c->va += step;
		c->extent_offset += step;
		return true;
	}
	while (c->sg && c->sg_offset >= sg_dma_len(c->sg)) {
		c->sg_offset -= sg_dma_len(c->sg);
		c->sg = sg_next(c->sg);
	}
	if (!c->sg)
		return false;
	*pte = ((sg_dma_address(c->sg) + c->sg_offset) & PTE_ADDRESS) | c->bits | PTE_SYSTEM;
	c->va += SZ_4K;
	c->sg_offset += SZ_4K;
	return true;
}

/* --------------------------------------------------------------- objects */

static struct apex_local_bo *to_local(struct drm_gem_object *object)
{
	return container_of(object, struct apex_local_bo, base);
}

/* The LOCAL address of object byte `offset` and the bytes contiguous from it. */
static u64 local_at(struct apex_local_bo *bo, u64 offset, u64 *run)
{
	struct apex_extent *extent;

	list_for_each_entry(extent, &bo->extents, link) {
		if (offset < extent->node.size) {
			*run = extent->node.size - offset;
			return extent->node.start + offset;
		}
		offset -= extent->node.size;
	}
	*run = 0;
	return 0;
}

static void apex_local_free(struct drm_gem_object *object)
{
	struct apex_local_bo *bo = to_local(object);

	mutex_lock(&bo->cp->lock);
	list_del(&bo->link);
	mutex_unlock(&bo->cp->lock);
	local_extents_free(bo->cp, &bo->extents);
	drm_gem_object_release(object);
	apex_cp_put(bo->cp);
	kfree(bo);
}

/* A revoked LOCAL mapping maps through BAR2 again on its next access, the
 * whole VMA at once. */
static vm_fault_t apex_local_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct drm_gem_object *object = vma->vm_private_data;
	struct apex_local_bo *bo = to_local(object);
	struct apex_cp *cp = bo->cp;
	u64 offset = (u64)(vma->vm_pgoff - drm_vma_node_start(&object->vma_node)) << PAGE_SHIFT;
	unsigned long address = vma->vm_start;
	vm_fault_t ret = VM_FAULT_SIGBUS;

	down_read(&cp->map_lock);
	if (READ_ONCE(cp->failed) || READ_ONCE(cp->hw_gone))
		goto out;
	while (address < vma->vm_end) {
		u64 run, local = local_at(bo, offset, &run);

		if (!run) {
			ret = VM_FAULT_SIGBUS;
			break;
		}
		for (; run && address < vma->vm_end; run -= PAGE_SIZE) {
			ret = vmf_insert_pfn(vma, address, (cp->bar2_phys + local) >> PAGE_SHIFT);
			if (ret & VM_FAULT_ERROR)
				goto out;
			address += PAGE_SIZE;
			local += PAGE_SIZE;
			offset += PAGE_SIZE;
		}
	}
out:
	up_read(&cp->map_lock);
	return ret;
}

static const struct vm_operations_struct apex_local_vm_ops = {
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
	.fault = apex_local_fault,
};

static int apex_local_mmap(struct drm_gem_object *object, struct vm_area_struct *vma)
{
	struct apex_local_bo *bo = to_local(object);
	unsigned long pages = vma_pages(vma);
	u64 first = vma->vm_pgoff - drm_vma_node_start(&object->vma_node);
	u64 bytes = vma->vm_end - vma->vm_start, at, run;
	int ret = 0;

	if (!(vma->vm_flags & VM_SHARED) || READ_ONCE(bo->cp->hw_gone) ||
	    first + pages > object->size >> PAGE_SHIFT)
		return -EINVAL;
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_page_prot = pgprot_writecombine(vm_get_page_prot(vma->vm_flags));
	down_read(&bo->cp->map_lock);
	if (!READ_ONCE(bo->cp->failed) && !READ_ONCE(bo->cp->hw_gone)) {
		for (at = 0; !ret && at < bytes; at += run) {
			u64 local = local_at(bo, (first << PAGE_SHIFT) + at, &run);

			if (!run) {
				ret = -EINVAL;
				break;
			}
			run = min(run, bytes - at);
			ret = io_remap_pfn_range(vma, vma->vm_start + at,
						 (bo->cp->bar2_phys + local) >> PAGE_SHIFT, run,
						 vma->vm_page_prot);
		}
	}
	up_read(&bo->cp->map_lock);
	return ret;
}

static int apex_local_vmap(struct drm_gem_object *object, struct iosys_map *map)
{
	struct apex_local_bo *bo = to_local(object);
	u64 run, local = local_at(bo, 0, &run);

	if (READ_ONCE(bo->cp->hw_gone))
		return -ENODEV;
	if (run < object->size)
		return -EOPNOTSUPP;
	iosys_map_set_vaddr_iomem(map, local_map(bo->cp, local));
	return 0;
}

static void apex_local_vunmap(struct drm_gem_object *object, struct iosys_map *map)
{
}

/* PRIME shares LOCAL objects only within this device: without an SG table a
 * foreign importer cannot attach. */
static const struct drm_gem_object_funcs apex_local_funcs = {
	.free = apex_local_free,
	.mmap = apex_local_mmap,
	.vmap = apex_local_vmap,
	.vunmap = apex_local_vunmap,
	.vm_ops = &apex_local_vm_ops,
};

static const struct drm_gem_object_funcs apex_system_funcs = {
	.free = drm_gem_shmem_object_free,
	.print_info = drm_gem_shmem_object_print_info,
	.pin = drm_gem_shmem_object_pin,
	.unpin = drm_gem_shmem_object_unpin,
	.get_sg_table = drm_gem_shmem_object_get_sg_table,
	.vmap = drm_gem_shmem_object_vmap,
	.vunmap = drm_gem_shmem_object_vunmap,
	.mmap = drm_gem_shmem_object_mmap,
	.vm_ops = &drm_gem_shmem_vm_ops,
};

static struct drm_gem_object *apex_cp_gem_create_object(struct drm_device *drm,
							 size_t bytes)
{
	struct drm_gem_shmem_object *shmem = kzalloc(sizeof(*shmem), GFP_KERNEL);

	if (!shmem)
		return ERR_PTR(-ENOMEM);
	shmem->base.funcs = &apex_system_funcs;
	return &shmem->base;
}

/* Scanout needs the object in one LOCAL range. */
bool apex_cp_gem_local(struct drm_gem_object *object, u64 *address)
{
	u64 run;

	if (object->funcs != &apex_local_funcs)
		return false;
	*address = local_at(to_local(object), 0, &run);
	return run == object->size;
}

/* Zero new LOCAL backing with a FILL on the kernel ring. */
static int local_zero(struct apex_cp *cp, u64 address, u64 bytes)
{
	u32 packets[6];
	int ret;

	if (!cp->present)
		return -ENODEV;
	ret = kring_run(cp, packets, apex_cp_fill(packets, address, bytes, 0));
	return ret == -ECANCELED ? -EIO : ret;
}

static struct drm_gem_object *local_bo_create(struct apex_cp *cp, struct drm_device *drm,
					      u64 bytes, bool contiguous)
{
	struct apex_local_bo *bo;
	struct apex_extent *extent;
	int ret;

	if (bytes > cp->usable)
		return ERR_PTR(-ENOMEM);
	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return ERR_PTR(-ENOMEM);
	bo->base.funcs = &apex_local_funcs;
	drm_gem_private_object_init(drm, &bo->base, bytes);
	bo->cp = cp_get(cp);
	INIT_LIST_HEAD(&bo->extents);
	mutex_lock(&cp->lock);
	list_add_tail(&bo->link, &cp->local_bos);
	mutex_unlock(&cp->lock);
	ret = local_extents_alloc(cp, &bo->extents, bytes, contiguous);
	list_for_each_entry(extent, &bo->extents, link) {
		if (ret)
			break;
		ret = local_zero(cp, extent->node.start, extent->node.size);
	}
	if (!ret)
		ret = drm_gem_create_mmap_offset(&bo->base);
	if (ret) {
		drm_gem_object_put(&bo->base);
		return ERR_PTR(ret);
	}
	return &bo->base;
}

/* Status and doorbell pages: tiny GEM objects so that mmap stays within
 * drm_gem_mmap's offset lookup and file access checks. */
static struct apex_page_bo *to_page(struct drm_gem_object *object)
{
	return container_of(object, struct apex_page_bo, base);
}

static void apex_page_free(struct drm_gem_object *object)
{
	struct apex_page_bo *bo = to_page(object);

	drm_gem_object_release(object);
	apex_cp_put(bo->cp);
	kfree(bo);
}

static int apex_page_mmap(struct drm_gem_object *object, struct vm_area_struct *vma)
{
	struct apex_page_bo *bo = to_page(object);
	int ret = -ENODEV;

	if (vma_pages(vma) != 1 || !(vma->vm_flags & VM_SHARED) ||
	    (vma->vm_flags & VM_EXEC) || (bo->cpu && (vma->vm_flags & VM_WRITE)))
		return -EINVAL;
	mutex_lock(&bo->lock);
	if (bo->revoked || READ_ONCE(bo->cp->hw_gone))
		goto out;
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
	if (bo->cpu) {
		/* The fake offset stays in vm_pgoff so revocation can zap it. */
		unsigned long pgoff = vma->vm_pgoff;

		vm_flags_clear(vma, VM_MAYWRITE | VM_MAYEXEC);
		vma->vm_pgoff = 0;
		ret = dma_mmap_coherent(bo->cp->dev, vma, bo->cpu, bo->dma, PAGE_SIZE);
		vma->vm_pgoff = pgoff;
	} else {
		vm_flags_set(vma, VM_IO | VM_PFNMAP);
		vm_flags_clear(vma, VM_MAYEXEC);
		vma->vm_page_prot = pgprot_noncached(vm_get_page_prot(vma->vm_flags));
		ret = io_remap_pfn_range(vma, vma->vm_start, bo->phys >> PAGE_SHIFT,
					 PAGE_SIZE, vma->vm_page_prot);
	}
out:
	mutex_unlock(&bo->lock);
	return ret;
}

/* Pages map at mmap time; revocation zaps them and they stay gone. */
static vm_fault_t apex_page_fault(struct vm_fault *vmf)
{
	return VM_FAULT_SIGBUS;
}

static const struct vm_operations_struct apex_page_vm_ops = {
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
	.fault = apex_page_fault,
};

static const struct drm_gem_object_funcs apex_page_funcs = {
	.free = apex_page_free,
	.mmap = apex_page_mmap,
	.vm_ops = &apex_page_vm_ops,
};

static struct apex_page_bo *page_bo_create(struct apex_cp *cp, struct drm_file *file,
					   void *cpu, dma_addr_t dma, phys_addr_t phys)
{
	struct apex_page_bo *bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	int ret;

	if (!bo)
		return ERR_PTR(-ENOMEM);
	bo->base.funcs = &apex_page_funcs;
	drm_gem_private_object_init(file->minor->dev, &bo->base, PAGE_SIZE);
	mutex_init(&bo->lock);
	bo->cp = cp_get(cp);
	bo->file = file;
	bo->cpu = cpu;
	bo->dma = dma;
	bo->phys = phys;
	ret = drm_gem_create_mmap_offset(&bo->base);
	if (!ret)
		ret = drm_vma_node_allow(&bo->base.vma_node, file);
	if (ret) {
		drm_gem_object_put(&bo->base);
		return ERR_PTR(ret);
	}
	return bo;
}

/* Revoke CPU access before the page is freed or reused. */
static void page_bo_revoke(struct apex_page_bo *bo)
{
	if (!bo)
		return;
	mutex_lock(&bo->lock);
	bo->revoked = true;
	mutex_unlock(&bo->lock);
	drm_vma_node_unmap(&bo->base.vma_node, bo->base.dev->anon_inode->i_mapping);
	drm_vma_node_revoke(&bo->base.vma_node, bo->file);
	drm_gem_object_put(&bo->base);
}

/* -------------------------------------------------------------- VM state */

/* Allocate the file's VMID, root table and context entry (ctx->lock, hw). */
static int ctx_vm_init(struct apex_ctx *ctx, bool hw)
{
	struct apex_cp *cp = ctx->cp;
	struct apex_pt *root;
	u32 packets[2];
	u32 vmid;
	int ret;

	if (ctx->vmid)
		return 0;
	mutex_lock(&cp->lock);
	vmid = find_next_zero_bit(cp->vmids, CP_VMS, 1);
	if (vmid < CP_VMS)
		set_bit(vmid, cp->vmids);
	mutex_unlock(&cp->lock);
	if (vmid >= CP_VMS)
		return -EBUSY;
	root = pt_alloc(cp, 1024, SZ_8K);
	if (IS_ERR(root)) {
		ret = PTR_ERR(root);
		goto release;
	}
	ctx->root = root;
	ctx->vmid = vmid;
	if (!cp->present || !hw)
		return 0;
	context_write(cp, vmid, root->node.start, true);
	ret = kring_run(cp, packets,
			apex_cp_vmid_packet(packets, APEX_CP_TLB_INVALIDATE, vmid));
	if (!ret || ret == -ECANCELED)
		return 0;
	ctx->root = NULL;
	ctx->vmid = 0;
	pt_free(cp, root, 1024);
release:
	mutex_lock(&cp->lock);
	clear_bit(vmid, cp->vmids);
	mutex_unlock(&cp->lock);
	return ret;
}

static struct sg_table *system_sgt(struct drm_gem_object *object)
{
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(object);

	if (drm_gem_is_imported(object))
		return shmem->sgt ? shmem->sgt : ERR_PTR(-EINVAL);
	return drm_gem_shmem_get_pages_sgt(shmem);
}

/* SYSTEM leaves hold page-aligned IOVAs below 2^52. */
static int sgt_check(struct sg_table *sgt)
{
	struct scatterlist *sg;
	int i;

	for_each_sgtable_dma_sg(sgt, sg, i) {
		u64 address = sg_dma_address(sg), length = sg_dma_len(sg);

		if (!IS_ALIGNED(address | length, SZ_4K) || address >= PHYSICAL_LIMIT ||
		    length > PHYSICAL_LIMIT - address)
			return -ERANGE;
	}
	return 0;
}

static int vm_map(struct apex_ctx *ctx, struct drm_file *file,
		  const struct drm_apex_vm_bind_op *op)
{
	struct drm_gem_object *object = NULL;
	struct apex_mapping *mapping;
	struct pte_cursor c = {0};
	struct sg_table *sgt;
	u64 va, pte;
	bool large;
	int ret;

	if (op->op == APEX_VM_MAP_NULL) {
		if (op->handle || op->offset || (op->flags & ~(APEX_VM_READ | APEX_VM_WRITE)))
			return -EINVAL;
		c.null = true;
	} else {
		object = drm_gem_object_lookup(file, op->handle);
		if (!object)
			return -ENOENT;
		ret = -EINVAL;
		if (op->offset > object->size || op->bytes > object->size - op->offset)
			goto put;
		if (object->funcs == &apex_local_funcs) {
			c.extent = list_first_entry(&to_local(object)->extents, struct apex_extent,
						    link);
			c.extent_offset = op->offset;
		} else if (object->funcs == &apex_system_funcs) {
			sgt = system_sgt(object);
			if (IS_ERR(sgt)) {
				ret = PTR_ERR(sgt);
				goto put;
			}
			ret = sgt_check(sgt);
			if (ret)
				goto put;
			c.system = true;
			c.sg = sgt->sgl;
			c.sg_offset = op->offset;
		} else {
			goto put;
		}
	}
	c.va = op->va;
	c.end = op->va + op->bytes;
	c.bits = PTE_VALID | PTE_LEAF |
		 ((op->flags & APEX_VM_READ) ? PTE_READ : 0) |
		 ((op->flags & APEX_VM_WRITE) ? PTE_WRITE : 0) |
		 ((op->flags & APEX_VM_EXEC) ? PTE_EXEC : 0);
	mapping = kzalloc(sizeof(*mapping), GFP_KERNEL);
	ret = -ENOMEM;
	if (!mapping)
		goto put;
	mapping->va.start = op->va;
	mapping->va.size = op->bytes;
	mapping->object = object;
	mapping->flags = op->flags;
	ret = drm_mm_reserve_node(&ctx->va, &mapping->va);
	if (ret) {
		kfree(mapping);
		ret = ret == -ENOSPC ? -EEXIST : ret;
		goto put;
	}
	/* Build every table first so a failed allocation leaves no leaf. */
	{
		struct pte_cursor scan = c;

		while (cursor_next(ctx, &scan, &va, &pte, &large)) {
			ret = pt_ensure(ctx, va, large);
			if (ret)
				goto remove;
		}
		if (scan.va < scan.end) {
			ret = -EINVAL;
			goto remove;
		}
	}
	while (cursor_next(ctx, &c, &va, &pte, &large))
		pt_set(ctx, va, pte, large);
	wmb();
	return 0;
remove:
	drm_mm_remove_node(&mapping->va);
	kfree(mapping);
put:
	if (object)
		drm_gem_object_put(object);
	return ret;
}

/* Drop a removed mapping. A quarantined device may still address the
 * backing, so the object stays allocated; only its mmap offset leaves, so the
 * DRM device's offset manager tears down clean on unload. */
static void mapping_release(struct apex_mapping *mapping, bool leak)
{
	if (leak) {
		if (mapping->object)
			drm_gem_free_mmap_offset(mapping->object);
		return;
	}
	if (mapping->object)
		drm_gem_object_put(mapping->object);
	kfree(mapping);
}

/* Remove whole mappings in [va, va + bytes) and fence the VM before their
 * backing returns (ctx->lock, hw state as given). */
static int vm_unmap(struct apex_ctx *ctx, u64 va, u64 bytes, bool hw)
{
	struct apex_cp *cp = ctx->cp;
	struct apex_mapping *mapping, *next;
	struct drm_mm_node *node;
	LIST_HEAD(removed);
	u64 end = va + bytes;
	int ret = 0;

	drm_mm_for_each_node_in_range(node, &ctx->va, va, end) {
		if (node->start < va || node->start + node->size > end)
			return -EINVAL;
		list_add_tail(&container_of(node, struct apex_mapping, va)->link, &removed);
	}
	if (list_empty(&removed))
		return -ENOENT;
	list_for_each_entry(mapping, &removed, link) {
		pt_clear(ctx, mapping->va.start, mapping->va.start + mapping->va.size);
		drm_mm_remove_node(&mapping->va);
	}
	wmb();
	if (ctx->gpu_used && cp->present) {
		ret = hw ? kring_vm_fence(cp, ctx->vmid) : -EIO;
		if (ret && !cp->quarantined)
			ret = 0; /* hardware gone after a drained remove */
	}
	list_for_each_entry_safe(mapping, next, &removed, link) {
		list_del(&mapping->link);
		mapping_release(mapping, cp->quarantined);
	}
	return ret;
}

static int vm_bind_ioctl(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_apex_vm_bind *args = data;
	struct apex_ctx *ctx = file->driver_priv;
	struct drm_apex_vm_bind_op *ops;
	unsigned int i;
	bool hw;
	int ret = 0;

	if (args->reserved || !args->op_count || args->op_count > 4096)
		return -EINVAL;
	ops = kvmalloc_array(args->op_count, sizeof(*ops), GFP_KERNEL);
	if (!ops)
		return -ENOMEM;
	if (copy_from_user(ops, u64_to_user_ptr(args->ops),
			   array_size(args->op_count, sizeof(*ops)))) {
		kvfree(ops);
		return -EFAULT;
	}
	for (i = 0; i < args->op_count; i++) {
		const struct drm_apex_vm_bind_op *op = &ops[i];

		if (op->reserved || !op->bytes ||
		    !IS_ALIGNED(op->va | op->offset | op->bytes, SZ_4K) ||
		    op->va < VA_USER_BASE || op->va >= VA_LIMIT ||
		    op->bytes > VA_LIMIT - op->va) {
			ret = -EINVAL;
			goto out;
		}
		if (op->op == APEX_VM_UNMAP) {
			if (op->flags || op->handle || op->offset) {
				ret = -EINVAL;
				goto out;
			}
		} else if (op->op == APEX_VM_MAP) {
			if (op->flags & ~(APEX_VM_READ | APEX_VM_WRITE | APEX_VM_EXEC)) {
				ret = -EINVAL;
				goto out;
			}
		} else if (op->op != APEX_VM_MAP_NULL) {
			ret = -EINVAL;
			goto out;
		}
	}
	mutex_lock(&ctx->lock);
	hw = cp_hw_enter(ctx->cp);
	if (!hw || ctx->cp->failed) {
		ret = -ENODEV;
		goto unlock;
	}
	ret = ctx_vm_init(ctx, hw);
	for (i = 0; !ret && i < args->op_count; i++) {
		if (ops[i].op == APEX_VM_UNMAP)
			ret = vm_unmap(ctx, ops[i].va, ops[i].bytes, hw);
		else
			ret = vm_map(ctx, file, &ops[i]);
	}
unlock:
	if (hw)
		cp_hw_exit(ctx->cp);
	mutex_unlock(&ctx->lock);
out:
	kvfree(ops);
	return ret;
}

/* ---------------------------------------------------------------- queues */

static bool ring_mapped(struct apex_ctx *ctx, u64 va, u64 bytes)
{
	struct drm_mm_node *node;
	u64 covered = va;

	drm_mm_for_each_node_in_range(node, &ctx->va, va, va + bytes) {
		struct apex_mapping *mapping = container_of(node, struct apex_mapping, va);

		if (node->start > covered || !(mapping->flags & APEX_VM_READ))
			return false;
		covered = node->start + node->size;
	}
	return covered >= va + bytes;
}

static bool apex_inject_drain_timeout;
module_param_named(inject_drain_timeout, apex_inject_drain_timeout, bool, 0644);
MODULE_PARM_DESC(inject_drain_timeout,
		 "qualification: the next user-queue drain times out and escalates to the native reset (clears itself)");

static int queue_wait_drained(struct apex_cp *cp, struct apex_queue *q,
			      unsigned long timeout)
{
	unsigned long deadline = jiffies + timeout;
	u32 state;

	for (;;) {
		state = cp_queue_state(cp, q->slot);
		if (state == U32_MAX)
			return -EIO;
		if (!(state & CP_QUEUE_ENABLED) && (state & CP_QUEUE_DRAINED))
			return 0;
		if (time_after(jiffies, deadline) || cp->failed || cp_stuck(cp))
			return -ETIMEDOUT;
		usleep_range(50, 200);
	}
}

/* fence_lock held: take ownership of every registered wait of q. */
static void waits_take(struct apex_queue *q, struct list_head *waits)
{
	struct apex_wait *wait;

	list_for_each_entry(wait, &q->waits, link)
		wait->queue = NULL;
	list_splice_init(&q->waits, waits);
}

static void waits_release(struct list_head *waits)
{
	struct apex_wait *wait, *next;

	list_for_each_entry_safe(wait, next, waits, link) {
		list_del(&wait->link);
		dma_fence_remove_callback(wait->fence, &wait->cb);
		dma_fence_put(wait->fence);
		kfree(wait);
	}
}

static void queue_detach(struct apex_cp *cp, struct apex_queue *q)
{
	unsigned long flags;
	LIST_HEAD(done);
	LIST_HEAD(waits);

	spin_lock_irqsave(&cp->fence_lock, flags);
	if (cp->slots[q->slot] == q)
		cp->slots[q->slot] = NULL;
	queue_collect(cp, q, -ECANCELED, &done);
	waits_take(q, &waits);
	spin_unlock_irqrestore(&cp->fence_lock, flags);
	fences_signal(&done);
	waits_release(&waits);
}

/* Disable, drain and free one user queue (ctx->lock held). Issued work runs
 * to completion within its fence deadline, as it would have had the file
 * stayed open: a process that exits mid-frame must not have its healthy work
 * aborted. Work still running after the deadline is aborted in every unit,
 * so a hung client leaves the other queues running; a queue that has not
 * drained 5 s after its abort escalates to the native reset. Unproven
 * drains leak the page. */
static void queue_destroy(struct apex_ctx *ctx, struct apex_queue *q, bool hw)
{
	struct apex_cp *cp = ctx->cp;
	bool safe;

	if (hw && !cp->failed && !q->lost) {
		int ret;

		cp_queue_command(cp, q->slot, CP_COMMAND_DISABLE);
		if (xchg(&apex_inject_drain_timeout, false)) {
			dev_warn(cp->dev, "injected drain timeout on queue %u\n", q->slot);
			ret = -ETIMEDOUT;
		} else {
			ret = queue_wait_drained(cp, q, cp_fence_timeout(cp));
			if (ret == -ETIMEDOUT && !cp_stuck(cp)) {
				dev_warn(cp->dev, "queue %u did not drain: aborting its work\n",
					 q->slot);
				cp_queue_command(cp, q->slot, CP_COMMAND_ABORT);
				ret = queue_wait_drained(cp, q, DRAIN_TIMEOUT);
			}
		}
		if (ret) {
			mutex_lock(&cp->kring_lock);
			if (!q->lost)
				cp_reset_locked(cp);
			mutex_unlock(&cp->kring_lock);
		} else {
			/* Frees the slot's fault and the first-fault latch naming it. */
			cp_queue_command(cp, q->slot, CP_COMMAND_CLEAR_FAULT);
		}
	}
	queue_detach(cp, q);
	if (hw && ctx->root) {
		pt_clear(ctx, q->status_va, q->status_va + SZ_4K);
		pt_clear(ctx, q->kwait_va, q->kwait_va + SZ_4K);
		wmb();
	}
	if (hw && !cp->failed)
		kring_vm_fence(cp, ctx->vmid);
	safe = !cp->quarantined;
	page_bo_revoke(q->status_bo);
	page_bo_revoke(q->doorbell_bo);
	ctx->queues[q->index] = NULL;
	if (!safe) {
		dev_warn(cp->dev, "leaking status page of queue %u\n", q->slot);
		return;
	}
	mutex_lock(&cp->lock);
	clear_bit(q->slot, cp->slot_used);
	mutex_unlock(&cp->lock);
	dma_free_coherent(cp->dev, PAGE_SIZE, q->status, q->status_dma);
	local_free(cp, &q->kwait_node);
	kfree(q);
}

static const struct file_operations apex_drm_fops;

static int master_fd(const void *data, struct file *f, unsigned int fd)
{
	const struct drm_device *drm = data;
	struct drm_file *priv = f->private_data;

	return f->f_op == &apex_drm_fops && priv->minor->dev == drm && drm_is_current_master(priv);
}

/* Whether the calling process holds this device's DRM master file, as the
 * compositor does on the card node that logind handed it while it creates
 * queues on the render node. */
static bool process_is_master(struct drm_device *drm)
{
	return iterate_fd(current->files, 0, master_fd, drm) != 0;
}

static int queue_create_ioctl(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_apex_queue_create *args = data;
	struct apex_ctx *ctx = file->driver_priv;
	struct apex_cp *cp = ctx->cp;
	struct apex_queue *q;
	unsigned int index;
	u32 slot, state, priority;
	bool hw, master;
	int ret;

	if (args->reserved || args->queue_id || args->status_va || args->status_offset ||
	    args->doorbell_offset || args->kwait_va || args->priority > 1 ||
	    args->ring_bytes < SZ_4K || args->ring_bytes > SZ_1M ||
	    !is_power_of_2(args->ring_bytes) || !IS_ALIGNED(args->ring_va, SZ_4K) ||
	    args->ring_va < VA_USER_BASE || args->ring_va >= VA_LIMIT ||
	    args->ring_bytes > VA_LIMIT - args->ring_va)
		return -EINVAL;
	/* The DRM master's process, the compositor, always runs high. */
	master = process_is_master(drm);
	if (args->priority && !master && !capable(CAP_SYS_NICE))
		return -EPERM;
	priority = args->priority || master;
	mutex_lock(&ctx->lock);
	hw = cp_hw_enter(cp);
	ret = -ENODEV;
	if (!hw || !cp->present || cp->failed)
		goto unlock;
	ret = atomic_read(&ctx->error);
	if (ret)
		goto unlock;
	ret = ctx_vm_init(ctx, hw);
	if (ret)
		goto unlock;
	ret = -EINVAL;
	if (!ring_mapped(ctx, args->ring_va, args->ring_bytes))
		goto unlock;
	for (index = 0; index < CP_QUEUES_PER_FILE && ctx->queues[index]; index++)
		;
	ret = -EBUSY;
	if (index == CP_QUEUES_PER_FILE)
		goto unlock;
	ret = -ENOMEM;
	q = kzalloc(sizeof(*q), GFP_KERNEL);
	if (!q)
		goto unlock;
	q->cp = cp;
	q->ctx = ctx;
	q->index = index;
	q->status_va = (index + 1ull) * SZ_4K;
	q->kwait_va = (CP_QUEUES_PER_FILE + index + 1ull) * SZ_4K;
	INIT_LIST_HEAD(&q->fences);
	INIT_LIST_HEAD(&q->waits);
	q->status = dma_alloc_coherent(cp->dev, PAGE_SIZE, &q->status_dma, GFP_KERNEL);
	if (!q->status)
		goto free;
	ret = -ERANGE;
	if (!IS_ALIGNED(q->status_dma, SZ_4K) || q->status_dma >= PHYSICAL_LIMIT)
		goto free_page;
	ret = pt_ensure(ctx, q->status_va, false) ?: pt_ensure(ctx, q->kwait_va, false) ?:
	      local_alloc(cp, &q->kwait_node, SZ_4K, SZ_4K);
	if (ret)
		goto free_page;
	down_read(&cp->map_lock);
	memset_io(local_map(cp, q->kwait_node.start), 0, SZ_4K);
	up_read(&cp->map_lock);
	mutex_lock(&cp->lock);
	slot = find_next_zero_bit(cp->slot_used, CP_QUEUES, 1);
	if (slot < CP_QUEUES)
		set_bit(slot, cp->slot_used);
	mutex_unlock(&cp->lock);
	ret = -EBUSY;
	if (slot == CP_QUEUES)
		goto free_page;
	q->slot = slot;
	q->status_bo = page_bo_create(cp, file, q->status, q->status_dma, 0);
	if (IS_ERR(q->status_bo)) {
		ret = PTR_ERR(q->status_bo);
		q->status_bo = NULL;
		goto release_slot;
	}
	q->doorbell_bo = page_bo_create(cp, file, NULL, 0, cp->bar4_phys + slot * SZ_4K);
	if (IS_ERR(q->doorbell_bo)) {
		ret = PTR_ERR(q->doorbell_bo);
		q->doorbell_bo = NULL;
		goto release_bo;
	}
	pt_set(ctx, q->status_va, (q->status_dma & PTE_ADDRESS) | PTE_VALID | PTE_LEAF |
	       PTE_READ | PTE_SYSTEM, false);
	pt_set(ctx, q->kwait_va, (q->kwait_node.start & PTE_ADDRESS) | PTE_VALID | PTE_LEAF |
	       PTE_READ, false);
	wmb();
	q->fence_context = dma_fence_context_alloc(1);
	{
		unsigned long flags;

		spin_lock_irqsave(&cp->fence_lock, flags);
		cp->slots[slot] = q;
		spin_unlock_irqrestore(&cp->fence_lock, flags);
	}
	cp_queue_program(cp, q, ctx->vmid, priority, args->ring_va,
			 ilog2(args->ring_bytes) - 12);
	state = cp_queue_state(cp, slot);
	if (!(state & CP_QUEUE_ENABLED) || state == U32_MAX) {
		ctx->queues[index] = q;
		ctx->gpu_used = true;
		queue_destroy(ctx, q, hw);
		ret = -EIO;
		goto unlock;
	}
	ctx->queues[index] = q;
	ctx->gpu_used = true;
	dev_dbg(cp->dev, "queue %u (%s[%d]) created, priority %u\n", slot, ctx->comm, ctx->pid, priority);
	args->queue_id = index;
	args->priority = priority;
	args->status_va = q->status_va;
	args->kwait_va = q->kwait_va;
	args->status_offset = drm_vma_node_offset_addr(&q->status_bo->base.vma_node);
	args->doorbell_offset = drm_vma_node_offset_addr(&q->doorbell_bo->base.vma_node);
	ret = 0;
	goto unlock;
release_bo:
	page_bo_revoke(q->status_bo);
release_slot:
	mutex_lock(&cp->lock);
	clear_bit(slot, cp->slot_used);
	mutex_unlock(&cp->lock);
free_page:
	dma_free_coherent(cp->dev, PAGE_SIZE, q->status, q->status_dma);
	local_free(cp, &q->kwait_node);
free:
	kfree(q);
unlock:
	if (hw)
		cp_hw_exit(cp);
	mutex_unlock(&ctx->lock);
	return ret;
}

static struct apex_queue *ctx_queue(struct apex_ctx *ctx, u32 id)
{
	return id < CP_QUEUES_PER_FILE ? ctx->queues[id] : NULL;
}

static int queue_destroy_ioctl(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_apex_queue_destroy *args = data;
	struct apex_ctx *ctx = file->driver_priv;
	struct apex_queue *q;
	bool hw;
	int ret = 0;

	if (args->reserved)
		return -EINVAL;
	mutex_lock(&ctx->lock);
	q = ctx_queue(ctx, args->queue_id);
	if (!q) {
		ret = -ENOENT;
	} else {
		hw = cp_hw_enter(ctx->cp);
		queue_destroy(ctx, q, hw);
		if (hw)
			cp_hw_exit(ctx->cp);
	}
	mutex_unlock(&ctx->lock);
	return ret;
}

static int queue_fence_ioctl(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_apex_queue_fence *args = data;
	struct apex_ctx *ctx = file->driver_priv;
	struct apex_cp *cp = ctx->cp;
	struct dma_fence_chain *chain = NULL;
	struct drm_syncobj *syncobj;
	struct apex_fence *fence;
	struct apex_queue *q;
	unsigned long flags;
	LIST_HEAD(done);
	bool schedule;
	int ret;

	syncobj = drm_syncobj_find(file, args->syncobj);
	if (!syncobj)
		return -ENOENT;
	fence = kzalloc(sizeof(*fence), GFP_KERNEL);
	if (args->point)
		chain = dma_fence_chain_alloc();
	ret = -ENOMEM;
	if (!fence || (args->point && !chain))
		goto free;
	mutex_lock(&ctx->lock);
	q = ctx_queue(ctx, args->queue_id);
	ret = -ENOENT;
	if (!q)
		goto unlock;
	ret = atomic_read(&ctx->error);
	if (ret)
		goto unlock;
	spin_lock_init(&fence->lock);
	INIT_LIST_HEAD(&fence->link);
	dma_fence_init(&fence->base, &apex_fence_ops, &fence->lock, q->fence_context,
		       ++q->fence_seq);
	fence->target = args->seqno;
	fence->start = fence->head = jiffies;
	fence->queue = q;
	dma_fence_get(&fence->base); /* pending list reference */
	spin_lock_irqsave(&cp->fence_lock, flags);
	fence->order = ++cp->fence_order;
	list_add_tail(&fence->link, &q->fences);
	schedule = !cp->pending_fences++;
	queue_service(cp, q, &done);
	spin_unlock_irqrestore(&cp->fence_lock, flags);
	fences_signal(&done);
	if (schedule)
		schedule_delayed_work(&cp->fence_work, FENCE_SCAN);
	if (args->point) {
		drm_syncobj_add_point(syncobj, chain, &fence->base, args->point);
		chain = NULL;
	} else {
		drm_syncobj_replace_fence(syncobj, &fence->base);
	}
	dma_fence_put(&fence->base);
	fence = NULL;
	ret = 0;
unlock:
	mutex_unlock(&ctx->lock);
free:
	dma_fence_chain_free(chain);
	kfree(fence);
	drm_syncobj_put(syncobj);
	return ret;
}

/* fence_lock held: publish a satisfied wait value and recheck waits. */
static void queue_kwait(struct apex_cp *cp, struct apex_queue *q, u64 value)
{
	if (value <= q->kwait)
		return;
	q->kwait = value;
	if (cp->hw_gone)
		return;
	/* A native reset holds map_lock for write and ends every queue's waits. */
	if (down_read_trylock(&cp->map_lock)) {
		writeq(value, local_map(cp, q->kwait_node.start));
		up_read(&cp->map_lock);
	}
	wmb();
	writel(1, cp->bar4 + q->slot * SZ_4K + APEX_CP_DOORBELL_RECHECK);
}

static void apex_wait_cb(struct dma_fence *signaled, struct dma_fence_cb *cb)
{
	struct apex_wait *wait = container_of(cb, struct apex_wait, cb);
	struct apex_cp *cp = wait->cp;
	unsigned long flags;
	bool owner;

	spin_lock_irqsave(&cp->fence_lock, flags);
	owner = wait->queue;
	if (owner) {
		list_del(&wait->link);
		queue_kwait(cp, wait->queue, wait->value);
		wait->queue = NULL;
	}
	spin_unlock_irqrestore(&cp->fence_lock, flags);
	if (owner) {
		dma_fence_put(wait->fence);
		kfree(wait);
	}
}

static int queue_wait_ioctl(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_apex_queue_wait *args = data;
	struct apex_ctx *ctx = file->driver_priv;
	struct apex_cp *cp = ctx->cp;
	struct apex_wait *wait;
	struct dma_fence *fence;
	struct apex_queue *q;
	unsigned long flags;
	int ret;

	ret = drm_syncobj_find_fence(file, args->syncobj, args->point, 0, &fence);
	if (ret)
		return ret;
	wait = kzalloc(sizeof(*wait), GFP_KERNEL);
	if (!wait) {
		dma_fence_put(fence);
		return -ENOMEM;
	}
	mutex_lock(&ctx->lock);
	q = ctx_queue(ctx, args->queue_id);
	ret = -ENOENT;
	if (!q)
		goto fail;
	ret = atomic_read(&ctx->error);
	if (ret)
		goto fail;
	wait->fence = fence;
	wait->cp = cp;
	wait->queue = q;
	wait->value = args->value;
	spin_lock_irqsave(&cp->fence_lock, flags);
	list_add_tail(&wait->link, &q->waits);
	spin_unlock_irqrestore(&cp->fence_lock, flags);
	if (dma_fence_add_callback(fence, &wait->cb, apex_wait_cb)) {
		/* Already signaled: the callback never runs and we own wait. */
		spin_lock_irqsave(&cp->fence_lock, flags);
		list_del(&wait->link);
		wait->queue = NULL;
		queue_kwait(cp, q, wait->value);
		spin_unlock_irqrestore(&cp->fence_lock, flags);
		dma_fence_put(fence);
		kfree(wait);
	}
	mutex_unlock(&ctx->lock);
	return 0;
fail:
	mutex_unlock(&ctx->lock);
	dma_fence_put(fence);
	kfree(wait);
	return ret;
}

static int queue_status_ioctl(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_apex_queue_status *args = data;
	struct apex_ctx *ctx = file->driver_priv;
	struct apex_queue *q;
	int ret = 0;

	if (args->reserved)
		return -EINVAL;
	mutex_lock(&ctx->lock);
	q = ctx_queue(ctx, args->queue_id);
	if (!q) {
		ret = -ENOENT;
	} else {
		args->state = status_u32(q, APEX_CP_STATUS_STATE);
		args->fault_status = status_u32(q, APEX_CP_STATUS_FAULT_STATUS);
		args->fault_unit = status_u32(q, APEX_CP_STATUS_FAULT_UNIT);
		args->fault_address = status_u64(q, APEX_CP_STATUS_FAULT_ADDRESS);
		args->error = atomic_read(&ctx->error);
	}
	mutex_unlock(&ctx->lock);
	return ret;
}

/* ---------------------------------------------------------- GEM ioctls */

static int info_ioctl(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct apex_ctx *ctx = file->driver_priv;
	struct apex_cp *cp = ctx->cp;
	struct drm_apex_info *info = data;

	*info = (struct drm_apex_info) {
		.local_bytes = cp->usable,
		.visible_bytes = cp->visible,
		.timestamp_hz = cp->timestamp_hz,
	};
	if (cp->profile == APEX_PROFILE_K7) {
		info->flags = APEX_INFO_LOCAL_USED;
		mutex_lock(&cp->lock);
		info->local_used = cp->local_used;
		mutex_unlock(&cp->lock);
	}
	if (cp->present) {
		info->queue_slots = cp->caps & 0xff;
		info->vm_slots = (cp->caps >> 8) & 0xff;
		info->queues_per_file = CP_QUEUES_PER_FILE;
		info->cores = (cp->caps >> 16) & 0xf;
		info->texture_units = (cp->caps >> 20) & 0xf;
		info->tile_planes = cp->caps >> 24;
	}
	return 0;
}

static int gem_create_ioctl(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_apex_gem_create *args = data;
	struct apex_ctx *ctx = file->driver_priv;
	struct apex_cp *cp = ctx->cp;
	struct drm_gem_object *object;
	int ret;

	if ((args->flags & ~APEX_GEM_SYSTEM) || args->handle || !args->size ||
	    !IS_ALIGNED(args->size, SZ_4K) || args->size >= VA_LIMIT)
		return -EINVAL;
	if (args->flags & APEX_GEM_SYSTEM) {
		struct drm_gem_shmem_object *shmem = drm_gem_shmem_create(drm, args->size);

		object = IS_ERR(shmem) ? ERR_CAST(shmem) : &shmem->base;
	} else {
		if (!cp_hw_enter(cp))
			return -ENODEV;
		object = local_bo_create(cp, drm, args->size, false);
		cp_hw_exit(cp);
	}
	if (IS_ERR(object))
		return PTR_ERR(object);
	ret = drm_gem_handle_create(file, object, &args->handle);
	drm_gem_object_put(object);
	return ret;
}

/* Dumb buffers are LOCAL scanout surfaces with 64-byte pitches. */
static int apex_cp_dumb_create(struct drm_file *file, struct drm_device *drm,
			       struct drm_mode_create_dumb *args)
{
	struct apex_ctx *ctx = file->driver_priv;
	struct drm_gem_object *object;
	int ret;

#if APEX_HAS_DRM_MODE_SIZE_DUMB
	ret = drm_mode_size_dumb(drm, args, 64, 0);
	if (ret)
		return ret;
#else
	/* drm_mode_size_dumb(drm, args, 64, 0): 64-byte pitch, page-sized. */
	{
		u64 pitch, size;

		if (!args->width || !args->height || !args->bpp)
			return -EINVAL;
		pitch = ALIGN((u64)args->width * DIV_ROUND_UP(args->bpp, 8), 64);
		size = PAGE_ALIGN(pitch * args->height);
		if (pitch > U32_MAX || !size)
			return -EINVAL;
		args->pitch = pitch;
		args->size = size;
	}
#endif
	if (!cp_hw_enter(ctx->cp))
		return -ENODEV;
	object = local_bo_create(ctx->cp, drm, args->size, true);
	cp_hw_exit(ctx->cp);
	if (IS_ERR(object))
		return PTR_ERR(object);
	ret = drm_gem_handle_create(file, object, &args->handle);
	drm_gem_object_put(object);
	return ret;
}

static int gem_mmap_ioctl(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_apex_gem_mmap *args = data;
	struct drm_gem_object *object;
	int ret;

	if (args->reserved)
		return -EINVAL;
	object = drm_gem_object_lookup(file, args->handle);
	if (!object)
		return -ENOENT;
	ret = drm_gem_create_mmap_offset(object);
	if (!ret)
		args->offset = drm_vma_node_offset_addr(&object->vma_node);
	drm_gem_object_put(object);
	return ret;
}

/* Every handler runs inside drm_dev_enter so that remove's unplug waits for
 * it before the command processor stops. */
#define APEX_IOCTL(name)                                                       \
	static int ioctl_##name(struct drm_device *drm, void *data,           \
				struct drm_file *file)                          \
	{                                                                      \
		int idx, ret;                                                  \
									       \
		if (!drm_dev_enter(drm, &idx))                                 \
			return -ENODEV;                                        \
		ret = name##_ioctl(drm, data, file);                           \
		drm_dev_exit(idx);                                             \
		return ret;                                                    \
	}
APEX_IOCTL(info)
APEX_IOCTL(gem_create)
APEX_IOCTL(gem_mmap)
APEX_IOCTL(vm_bind)
APEX_IOCTL(queue_create)
APEX_IOCTL(queue_destroy)
APEX_IOCTL(queue_fence)
APEX_IOCTL(queue_wait)
APEX_IOCTL(queue_status)

static const struct drm_ioctl_desc apex_cp_ioctls[] = {
	DRM_IOCTL_DEF_DRV(APEX_INFO, ioctl_info, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(APEX_GEM_CREATE, ioctl_gem_create, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(APEX_GEM_MMAP, ioctl_gem_mmap, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(APEX_VM_BIND, ioctl_vm_bind, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(APEX_QUEUE_CREATE, ioctl_queue_create, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(APEX_QUEUE_DESTROY, ioctl_queue_destroy, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(APEX_QUEUE_FENCE, ioctl_queue_fence, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(APEX_QUEUE_WAIT, ioctl_queue_wait, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(APEX_QUEUE_STATUS, ioctl_queue_status, DRM_RENDER_ALLOW),
};

/* ---------------------------------------------------------- DRM files */

static int apex_cp_open(struct drm_device *drm, struct drm_file *file)
{
	struct apex_cp *cp = apex_display_cp(drm);
	struct apex_ctx *ctx;

	if (!cp)
		return -ENODEV;
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->cp = cp_get(cp);
	ctx->drm = drm;
	get_task_comm(ctx->comm, current);
	ctx->pid = task_tgid_nr(current);
	mutex_init(&ctx->lock);
	drm_mm_init(&ctx->va, VA_USER_BASE, VA_LIMIT - VA_USER_BASE);
	file->driver_priv = ctx;
	return 0;
}

/* Teardown: disable queues, invalidate and drain the VMID on queue 0, then
 * free. An unproven drain leaves the device fenced and leaks the backing. */
static void apex_cp_postclose(struct drm_device *drm, struct drm_file *file)
{
	struct apex_ctx *ctx = file->driver_priv;
	struct apex_cp *cp;
	struct apex_mapping *mapping;
	struct drm_mm_node *node, *next;
	unsigned int i;
	bool hw, leak;

	if (!ctx)
		return;
	cp = ctx->cp;
	mutex_lock(&ctx->lock);
	hw = cp_hw_enter(cp);
	for (i = 0; i < CP_QUEUES_PER_FILE; i++)
		if (ctx->queues[i])
			queue_destroy(ctx, ctx->queues[i], hw);
	if (ctx->vmid && cp->present && hw && !cp->failed) {
		context_write(cp, ctx->vmid, 0, false);
		if (ctx->gpu_used)
			kring_vm_fence(cp, ctx->vmid);
	}
	if (hw)
		cp_hw_exit(cp);
	leak = cp->quarantined;
	drm_mm_for_each_node_safe(node, next, &ctx->va) {
		mapping = container_of(node, struct apex_mapping, va);
		drm_mm_remove_node(node);
		mapping_release(mapping, leak);
	}
	drm_mm_takedown(&ctx->va);
	pt_free(cp, ctx->root, 1024);
	if (ctx->vmid) {
		mutex_lock(&cp->lock);
		if (!leak)
			clear_bit(ctx->vmid, cp->vmids);
		mutex_unlock(&cp->lock);
	}
	mutex_unlock(&ctx->lock);
	mutex_destroy(&ctx->lock);
	kfree(ctx);
	apex_cp_put(cp);
}

DEFINE_DRM_GEM_FOPS(apex_drm_fops);

const struct drm_driver apex_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC | DRIVER_RENDER |
			   DRIVER_SYNCOBJ | DRIVER_SYNCOBJ_TIMELINE,
	.name = "apex-display",
	.desc = "Apex GPU",
	.major = 2,
	.minor = 0,
	.fops = &apex_drm_fops,
	.open = apex_cp_open,
	.postclose = apex_cp_postclose,
	.ioctls = apex_cp_ioctls,
	.num_ioctls = ARRAY_SIZE(apex_cp_ioctls),
	.dumb_create = apex_cp_dumb_create,
	.gem_create_object = apex_cp_gem_create_object,
	.gem_prime_import_sg_table = drm_gem_shmem_prime_import_sg_table,
};

/* ----------------------------------------------------- device lifetime */

struct apex_cp *apex_cp_create(struct pci_dev *pdev, void __iomem *bar0, u32 profile,
			       u64 usable, const struct apex_cp_ops *ops, void *owner)
{
	struct apex_cp *cp = kzalloc(sizeof(*cp), GFP_KERNEL);

	if (!cp)
		return ERR_PTR(-ENOMEM);
	kref_init(&cp->ref);
	cp->pdev = pci_dev_get(pdev);
	cp->dev = &pdev->dev;
	cp->bar0 = bar0;
	cp->ops = ops;
	cp->owner = owner;
	cp->profile = profile;
	cp->usable = usable;
	cp->visible = pci_resource_len(pdev, 2);
	cp->bar2_phys = pci_resource_start(pdev, 2);
	init_rwsem(&cp->hw_lock);
	init_rwsem(&cp->map_lock);
	INIT_LIST_HEAD(&cp->local_bos);
	mutex_init(&cp->lock);
	mutex_init(&cp->kring_lock);
	spin_lock_init(&cp->fence_lock);
	spin_lock_init(&cp->reg_lock);
	init_waitqueue_head(&cp->wait);
	INIT_DELAYED_WORK(&cp->fence_work, cp_fence_work);
	INIT_WORK(&cp->reset_work, cp_reset_work);
	INIT_WORK(&cp->fail_work, cp_fail_work);
	INIT_LIST_HEAD(&cp->kq.fences);
	INIT_LIST_HEAD(&cp->kq.waits);
	cp->kq.cp = cp;
	drm_mm_init(&cp->mm, 0, usable);
	cp->bar2 = ioremap_wc(cp->bar2_phys, usable);
	if (!cp->bar2) {
		drm_mm_takedown(&cp->mm);
		pci_dev_put(cp->pdev);
		kfree(cp);
		return ERR_PTR(-ENOMEM);
	}
	return cp;
}

/* Bring up the command processor: BAR4 doorbells, VM context table, private
 * area and the kernel ring. */
int apex_cp_start(struct apex_cp *cp)
{
	struct pci_dev *pdev = cp->pdev;
	int ret;

	cp->timestamp_hz = readl(cp->bar0 + CP_TIMEBASE);
	cp->caps = readl(cp->bar0 + CP_CAPS);
	if (readl(cp->bar0 + CP_ID) != CP_ID_VALUE || (cp->caps & 0xff) != CP_QUEUES ||
	    ((cp->caps >> 8) & 0xff) != CP_VMS ||
	    !(pci_resource_flags(pdev, 4) & IORESOURCE_MEM) ||
	    pci_resource_len(pdev, 4) < CP_QUEUES * SZ_4K) {
		dev_err(cp->dev, "image has no supported command processor (caps %#x)\n",
			cp->caps);
		return -ENODEV;
	}
	ret = pci_request_selected_regions(pdev, BIT(4), "apex_gpu");
	if (ret)
		return ret;
	ret = -ENOMEM;
	cp->bar4_phys = pci_resource_start(pdev, 4);
	cp->bar4 = pci_iomap(pdev, 4, CP_QUEUES * SZ_4K);
	if (!cp->bar4)
		goto release;
	if (local_alloc(cp, &cp->context_node, CP_CONTEXT_BYTES, SZ_4K) ||
	    local_alloc(cp, &cp->private_node, CP_PRIVATE_BYTES, CP_PRIVATE_ALIGN))
		goto free_local;
	memset_io(local_map(cp, cp->context_node.start), 0, CP_CONTEXT_BYTES);
	cp->kring = dma_alloc_coherent(cp->dev, KRING_BYTES, &cp->kring_dma, GFP_KERNEL);
	cp->kq.status = dma_alloc_coherent(cp->dev, PAGE_SIZE, &cp->kq.status_dma,
					   GFP_KERNEL);
	if (!cp->kring || !cp->kq.status || !IS_ALIGNED(cp->kring_dma, SZ_4K) ||
	    cp->kring_dma >= PHYSICAL_LIMIT || cp->kq.status_dma >= PHYSICAL_LIMIT)
		goto free_host;
	ret = cp->ops->master_get(cp->owner);
	if (ret)
		goto free_host;
	cp->master = true;
	cp->kq.slot = 0;
	cp->slots[0] = &cp->kq;
	WRITE_ONCE(cp->present, true);
	cp_program(cp);
	dev_info(cp->dev, "command processor: %u queues, %u VMs, %u cores\n",
		 cp->caps & 0xff, (cp->caps >> 8) & 0xff, (cp->caps >> 16) & 0xf);
	return 0;
free_host:
	if (cp->kring)
		dma_free_coherent(cp->dev, KRING_BYTES, cp->kring, cp->kring_dma);
	if (cp->kq.status)
		dma_free_coherent(cp->dev, PAGE_SIZE, cp->kq.status, cp->kq.status_dma);
	cp->kring = NULL;
	cp->kq.status = NULL;
free_local:
	local_free(cp, &cp->context_node);
	local_free(cp, &cp->private_node);
	pci_iounmap(pdev, cp->bar4);
	cp->bar4 = NULL;
release:
	pci_release_selected_regions(pdev, BIT(4));
	return ret;
}

/* Halt the command processor before the caller clears Bus Master Enable,
 * then stop all MMIO. */
void apex_cp_quiesce(struct apex_cp *cp)
{
	unsigned long flags;
	LIST_HEAD(waits);
	u32 status;
	int q;

	if (!cp)
		return;
	cancel_delayed_work_sync(&cp->fence_work);
	cancel_work_sync(&cp->reset_work);
	cancel_work_sync(&cp->fail_work);
	down_write(&cp->hw_lock);
	if (cp->present && !cp->failed) {
		writel(CP_CONTROL_ENABLE | CP_CONTROL_HALT, cp->bar0 + CP_CONTROL);
		if (readl_poll_timeout(cp->bar0 + CP_STATUS, status,
				       status == U32_MAX || (status & CP_STATUS_IDLE),
				       100, HALT_TIMEOUT_US) || status == U32_MAX) {
			dev_err(cp->dev, "command processor did not halt; backing quarantined\n");
			cp->quarantined = true;
		}
	}
	spin_lock_irqsave(&cp->fence_lock, flags);
	cp->hw_gone = true;
	for (q = 0; q < CP_QUEUES; q++)
		if (cp->slots[q])
			waits_take(cp->slots[q], &waits);
	spin_unlock_irqrestore(&cp->fence_lock, flags);
	up_write(&cp->hw_lock);
	local_unmap_all(cp);
	waits_release(&waits);
	WRITE_ONCE(cp->failed, true);
	cp_fail_fences(cp, -ENODEV);
}

/* After IRQ release and the Bus Master fence: unmap and free the kernel's
 * own host memory unless the device might still address it. */
void apex_cp_remove(struct apex_cp *cp, bool drained)
{
	if (!cp)
		return;
	if (!drained)
		cp->quarantined = true;
	if (cp->bar4) {
		pci_iounmap(cp->pdev, cp->bar4);
		pci_release_selected_regions(cp->pdev, BIT(4));
	}
	iounmap(cp->bar2);
	if (cp->master)
		cp->ops->master_put(cp->owner);
	if (cp->kring && !cp->quarantined) {
		dma_free_coherent(cp->dev, KRING_BYTES, cp->kring, cp->kring_dma);
		dma_free_coherent(cp->dev, PAGE_SIZE, cp->kq.status, cp->kq.status_dma);
	}
	local_free(cp, &cp->context_node);
	local_free(cp, &cp->private_node);
	WRITE_ONCE(cp->present, false);
}

static void cp_free(struct kref *ref)
{
	struct apex_cp *cp = container_of(ref, struct apex_cp, ref);

	cancel_work_sync(&cp->fail_work);
	cancel_work_sync(&cp->reset_work);
	cancel_delayed_work_sync(&cp->fence_work);
	drm_mm_takedown(&cp->mm);
	pci_dev_put(cp->pdev);
	kfree(cp);
}
