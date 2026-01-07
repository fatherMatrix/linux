// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/mm/swap.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 */

/*
 * This file contains the default values for the operation of the
 * Linux VM subsystem. Fine-tuning documentation can be found in
 * Documentation/admin-guide/sysctl/vm.rst.
 * Started 18.12.91
 * Swap aging added 23.2.95, Stephen Tweedie.
 * Buffermem limits added 12.3.98, Rik van Riel.
 */

/*
 * ============================================================================
 * LRU (Least Recently Used) 批处理机制概述
 * ============================================================================
 *
 * 本文件实现了 Linux 内核的 LRU 页面管理批处理机制，这是内存管理子系统
 * 的核心性能优化技术之一。
 *
 * === 核心设计理念 ===
 *
 * 问题：
 * - 页面的 LRU 操作（添加、移除、激活、去活化等）非常频繁
 * - 每次操作都需要获取全局 LRU 锁，导致严重的锁竞争
 * - 在多核系统中，频繁的锁操作严重影响性能和可扩展性
 *
 * 解决方案：Per-CPU 批处理队列
 * - 使用 Per-CPU 的 folio_batch 作为页面操作的缓冲区
 * - 页面操作先记录在本地 CPU 的批处理队列中（无锁或仅本地锁）
 * - 当队列满或需要时，批量刷新到全局 LRU 链表（一次性获取锁）
 * - 大幅减少全局锁的竞争和获取次数
 *
 * === 主要组件 ===
 *
 * 1. Per-CPU 批处理结构（cpu_fbatches）：
 *    - lru_add: 等待加入 LRU 的新页面
 *    - lru_deactivate_file: 文件页去活化（降低优先级）
 *    - lru_deactivate: 一般页面去活化
 *    - lru_lazyfree: 延迟释放的匿名页
 *    - activate: 页面激活（提升优先级）
 *    - lru_rotate: 页面轮转（移至链表尾部）
 *
 * 2. 排空接口：
 *    - lru_add_drain(): 排空当前 CPU 的批处理队列
 *    - lru_add_drain_all(): 排空所有 CPU 的批处理队列
 *    - lru_add_drain_cpu(): 排空指定 CPU 的批处理队列
 *
 * 3. 页面操作接口：
 *    - folio_add_lru(): 将页面添加到 LRU（通过批处理）
 *    - folio_activate(): 激活页面
 *    - folio_deactivate(): 去活化页面
 *    - folio_mark_lazyfree(): 标记为延迟释放
 *
 * === 关键流程 ===
 *
 * 添加页面到 LRU：
 * 1. folio_add_lru() 将页面加入 Per-CPU 的 lru_add 队列
 * 2. 增加页面引用计数（防止被提前释放）
 * 3. 如果队列满，触发 folio_batch_move_lru() 批量刷新
 * 4. 刷新时获取 LRU 锁，批量将所有页面移入全局 LRU 链表
 *
 * 排空批处理队列：
 * 1. 禁用抢占（防止 CPU 迁移）
 * 2. 依次处理各类批处理队列（lru_add、rotate、deactivate 等）
 * 3. 对每个非空队列调用相应的移动函数
 * 4. 释放页面引用计数
 *
 * === 性能优化技术 ===
 *
 * 1. 批量处理：
 *    - 一次锁获取可处理多个页面（最多 15 个/批次）
 *    - 减少锁开销和缓存行抖动
 *
 * 2. Per-CPU 设计：
 *    - 避免 CPU 间的缓存行竞争
 *    - 本地操作无需全局同步
 *
 * 3. 延迟处理：
 *    - 非紧急操作可以延迟到队列满或显式刷新
 *    - 减少不必要的立即操作
 *
 * 4. 代数优化（Generation）：
 *    - lru_add_drain_all() 使用代数计数器避免重复工作
 *    - 高竞争场景下多个并发调用可以合并
 *
 * === 同步和一致性 ===
 *
 * 何时需要排空？
 * 1. 页面迁移/隔离前：确保页面不在批处理队列中
 * 2. mlock/munlock 操作：确保锁定状态立即生效
 * 3. 内存统计：获取准确的 LRU 状态
 * 4. 页面释放路径：及时归还页面给分配器
 * 5. 内存回收：准确识别可回收页面
 *
 * 内存屏障：
 * - smp_mb(): 保证跨 CPU 的可见性顺序
 * - smp_load_acquire/smp_store_release: 保证加载/存储顺序
 * - 关键路径使用内存屏障防止指令重排和保证可见性
 *
 * === 使用注意事项 ===
 *
 * 1. lru_add_drain() vs lru_add_drain_all()：
 *    - drain(): 本地 CPU，快速，用于局部一致性
 *    - drain_all(): 全局，慢速，用于全局一致性
 *
 * 2. 性能权衡：
 *    - 过早排空会降低批处理效果
 *    - 过晚排空可能导致内存视图不一致
 *    - 应在必要时才调用排空操作
 *
 * 3. 并发安全：
 *    - local_lock 保护 Per-CPU 数据
 *    - 排空时需要获取全局 LRU 锁
 *    - 调用者需要注意锁的嵌套顺序
 */

#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/mman.h>
#include <linux/pagemap.h>
#include <linux/pagevec.h>
#include <linux/init.h>
#include <linux/export.h>
#include <linux/mm_inline.h>
#include <linux/percpu_counter.h>
#include <linux/memremap.h>
#include <linux/percpu.h>
#include <linux/cpu.h>
#include <linux/notifier.h>
#include <linux/backing-dev.h>
#include <linux/memcontrol.h>
#include <linux/gfp.h>
#include <linux/uio.h>
#include <linux/hugetlb.h>
#include <linux/page_idle.h>
#include <linux/local_lock.h>
#include <linux/buffer_head.h>

#include "internal.h"

#define CREATE_TRACE_POINTS
#include <trace/events/pagemap.h>

/* How many pages do we try to swap or page in/out together? As a power of 2 */
int page_cluster;
const int page_cluster_max = 31;

/* Protecting only lru_rotate.fbatch which requires disabling interrupts */
struct lru_rotate {
	local_lock_t lock;
	struct folio_batch fbatch;
};
static DEFINE_PER_CPU(struct lru_rotate, lru_rotate) = {
	.lock = INIT_LOCAL_LOCK(lock),
};

/*
 * Per-CPU 页面批处理结构（folio batch）：
 *
 * 以下 folio batch 被分组在一起，因为它们受到禁用抢占的保护（中断保持启用状态）。
 * 这是 LRU 批处理机制的核心数据结构，用于延迟和批量处理页面的 LRU 操作。
 *
 * LRU 批处理机制的设计理念：
 * 1. 性能优化：避免频繁获取 LRU 锁，将多个页面的操作批量处理
 * 2. 减少锁竞争：通过 Per-CPU 缓存减少对全局 LRU 链表的并发访问
 * 3. 延迟处理：页面先放入 Per-CPU 批处理队列，等积累到一定数量或需要时再统一处理
 */
/*
 * The following folio batches are grouped together because they are protected
 * by disabling preemption (and interrupts remain enabled).
 */
struct cpu_fbatches {
	/* 本地锁，保护以下批处理队列 */
	local_lock_t lock;

	/* 等待加入 LRU 的页面队列 */
	struct folio_batch lru_add;

	/* 等待去活化的文件页队列 */
	struct folio_batch lru_deactivate_file;

	/* 等待去活化的页面队列 */
	struct folio_batch lru_deactivate;

	/* 等待延迟释放的页面队列 */
	struct folio_batch lru_lazyfree;
#ifdef CONFIG_SMP
	/* 等待激活的页面队列（仅 SMP） */
	struct folio_batch activate;
#endif
};
static DEFINE_PER_CPU(struct cpu_fbatches, cpu_fbatches) = {
	.lock = INIT_LOCAL_LOCK(lock),
};

/*
 * This path almost never happens for VM activity - pages are normally freed
 * in batches.  But it gets used by networking - and for compound pages.
 */
static void __page_cache_release(struct folio *folio)
{
	if (folio_test_lru(folio)) {
		struct lruvec *lruvec;
		unsigned long flags;

		lruvec = folio_lruvec_lock_irqsave(folio, &flags);
		lruvec_del_folio(lruvec, folio);
		__folio_clear_lru_flags(folio);
		unlock_page_lruvec_irqrestore(lruvec, flags);
	}
	/* See comment on folio_test_mlocked in release_pages() */
	if (unlikely(folio_test_mlocked(folio))) {
		long nr_pages = folio_nr_pages(folio);

		__folio_clear_mlocked(folio);
		zone_stat_mod_folio(folio, NR_MLOCK, -nr_pages);
		count_vm_events(UNEVICTABLE_PGCLEARED, nr_pages);
	}
}

static void __folio_put_small(struct folio *folio)
{
	__page_cache_release(folio);
	mem_cgroup_uncharge(folio);
	free_unref_page(&folio->page, 0);
}

static void __folio_put_large(struct folio *folio)
{
	/*
	 * __page_cache_release() is supposed to be called for thp, not for
	 * hugetlb. This is because hugetlb page does never have PageLRU set
	 * (it's never listed to any LRU lists) and no memcg routines should
	 * be called for hugetlb (it has a separate hugetlb_cgroup.)
	 */
	if (!folio_test_hugetlb(folio))
		__page_cache_release(folio);
	destroy_large_folio(folio);
}

void __folio_put(struct folio *folio)
{
	if (unlikely(folio_is_zone_device(folio)))
		free_zone_device_page(&folio->page);
	else if (unlikely(folio_test_large(folio)))
		__folio_put_large(folio);
	else
		__folio_put_small(folio);
}
EXPORT_SYMBOL(__folio_put);

/**
 * put_pages_list() - release a list of pages
 * @pages: list of pages threaded on page->lru
 *
 * Release a list of pages which are strung together on page.lru.
 */
void put_pages_list(struct list_head *pages)
{
	struct folio *folio, *next;

	list_for_each_entry_safe(folio, next, pages, lru) {
		if (!folio_put_testzero(folio)) {
			list_del(&folio->lru);
			continue;
		}
		if (folio_test_large(folio)) {
			list_del(&folio->lru);
			__folio_put_large(folio);
			continue;
		}
		/* LRU flag must be clear because it's passed using the lru */
	}

	free_unref_page_list(pages);
	INIT_LIST_HEAD(pages);
}
EXPORT_SYMBOL(put_pages_list);

typedef void (*move_fn_t)(struct lruvec *lruvec, struct folio *folio);

static void lru_add_fn(struct lruvec *lruvec, struct folio *folio)
{
	int was_unevictable = folio_test_clear_unevictable(folio);
	long nr_pages = folio_nr_pages(folio);

	VM_BUG_ON_FOLIO(folio_test_lru(folio), folio);

	/*
	 * Is an smp_mb__after_atomic() still required here, before
	 * folio_evictable() tests the mlocked flag, to rule out the possibility
	 * of stranding an evictable folio on an unevictable LRU?  I think
	 * not, because __munlock_folio() only clears the mlocked flag
	 * while the LRU lock is held.
	 *
	 * (That is not true of __page_cache_release(), and not necessarily
	 * true of release_pages(): but those only clear the mlocked flag after
	 * folio_put_testzero() has excluded any other users of the folio.)
	 */
	if (folio_evictable(folio)) {
		if (was_unevictable)
			__count_vm_events(UNEVICTABLE_PGRESCUED, nr_pages);
	} else {
		folio_clear_active(folio);
		folio_set_unevictable(folio);
		/*
		 * folio->mlock_count = !!folio_test_mlocked(folio)?
		 * But that leaves __mlock_folio() in doubt whether another
		 * actor has already counted the mlock or not.  Err on the
		 * safe side, underestimate, let page reclaim fix it, rather
		 * than leaving a page on the unevictable LRU indefinitely.
		 */
		folio->mlock_count = 0;
		if (!was_unevictable)
			__count_vm_events(UNEVICTABLE_PGCULLED, nr_pages);
	}

	lruvec_add_folio(lruvec, folio);
	trace_mm_lru_insertion(folio);
}

static void folio_batch_move_lru(struct folio_batch *fbatch, move_fn_t move_fn)
{
	int i;
	struct lruvec *lruvec = NULL;
	unsigned long flags = 0;

	for (i = 0; i < folio_batch_count(fbatch); i++) {
		struct folio *folio = fbatch->folios[i];

		/* block memcg migration while the folio moves between lru */
		if (move_fn != lru_add_fn && !folio_test_clear_lru(folio))
			continue;

		lruvec = folio_lruvec_relock_irqsave(folio, lruvec, &flags);
		move_fn(lruvec, folio);

		folio_set_lru(folio);
	}

	if (lruvec)
		unlock_page_lruvec_irqrestore(lruvec, flags);
	folios_put(fbatch->folios, folio_batch_count(fbatch));
	folio_batch_reinit(fbatch);
}

static void folio_batch_add_and_move(struct folio_batch *fbatch,
		struct folio *folio, move_fn_t move_fn)
{
	if (folio_batch_add(fbatch, folio) && !folio_test_large(folio) &&
	    !lru_cache_disabled())
		return;
	folio_batch_move_lru(fbatch, move_fn);
}

static void lru_move_tail_fn(struct lruvec *lruvec, struct folio *folio)
{
	if (!folio_test_unevictable(folio)) {
		lruvec_del_folio(lruvec, folio);
		folio_clear_active(folio);
		lruvec_add_folio_tail(lruvec, folio);
		__count_vm_events(PGROTATED, folio_nr_pages(folio));
	}
}

/*
 * Writeback is about to end against a folio which has been marked for
 * immediate reclaim.  If it still appears to be reclaimable, move it
 * to the tail of the inactive list.
 *
 * folio_rotate_reclaimable() must disable IRQs, to prevent nasty races.
 */
void folio_rotate_reclaimable(struct folio *folio)
{
	if (!folio_test_locked(folio) && !folio_test_dirty(folio) &&
	    !folio_test_unevictable(folio) && folio_test_lru(folio)) {
		struct folio_batch *fbatch;
		unsigned long flags;

		folio_get(folio);
		local_lock_irqsave(&lru_rotate.lock, flags);
		fbatch = this_cpu_ptr(&lru_rotate.fbatch);
		folio_batch_add_and_move(fbatch, folio, lru_move_tail_fn);
		local_unlock_irqrestore(&lru_rotate.lock, flags);
	}
}

void lru_note_cost(struct lruvec *lruvec, bool file,
		   unsigned int nr_io, unsigned int nr_rotated)
{
	unsigned long cost;

	/*
	 * Reflect the relative cost of incurring IO and spending CPU
	 * time on rotations. This doesn't attempt to make a precise
	 * comparison, it just says: if reloads are about comparable
	 * between the LRU lists, or rotations are overwhelmingly
	 * different between them, adjust scan balance for CPU work.
	 */
	cost = nr_io * SWAP_CLUSTER_MAX + nr_rotated;

	do {
		unsigned long lrusize;

		/*
		 * Hold lruvec->lru_lock is safe here, since
		 * 1) The pinned lruvec in reclaim, or
		 * 2) From a pre-LRU page during refault (which also holds the
		 *    rcu lock, so would be safe even if the page was on the LRU
		 *    and could move simultaneously to a new lruvec).
		 */
		spin_lock_irq(&lruvec->lru_lock);
		/* Record cost event */
		if (file)
			lruvec->file_cost += cost;
		else
			lruvec->anon_cost += cost;

		/*
		 * Decay previous events
		 *
		 * Because workloads change over time (and to avoid
		 * overflow) we keep these statistics as a floating
		 * average, which ends up weighing recent refaults
		 * more than old ones.
		 */
		lrusize = lruvec_page_state(lruvec, NR_INACTIVE_ANON) +
			  lruvec_page_state(lruvec, NR_ACTIVE_ANON) +
			  lruvec_page_state(lruvec, NR_INACTIVE_FILE) +
			  lruvec_page_state(lruvec, NR_ACTIVE_FILE);

		if (lruvec->file_cost + lruvec->anon_cost > lrusize / 4) {
			lruvec->file_cost /= 2;
			lruvec->anon_cost /= 2;
		}
		spin_unlock_irq(&lruvec->lru_lock);
	} while ((lruvec = parent_lruvec(lruvec)));
}

void lru_note_cost_refault(struct folio *folio)
{
	lru_note_cost(folio_lruvec(folio), folio_is_file_lru(folio),
		      folio_nr_pages(folio), 0);
}

static void folio_activate_fn(struct lruvec *lruvec, struct folio *folio)
{
	if (!folio_test_active(folio) && !folio_test_unevictable(folio)) {
		long nr_pages = folio_nr_pages(folio);

		lruvec_del_folio(lruvec, folio);
		folio_set_active(folio);
		lruvec_add_folio(lruvec, folio);
		trace_mm_lru_activate(folio);

		__count_vm_events(PGACTIVATE, nr_pages);
		__count_memcg_events(lruvec_memcg(lruvec), PGACTIVATE,
				     nr_pages);
	}
}

#ifdef CONFIG_SMP
static void folio_activate_drain(int cpu)
{
	struct folio_batch *fbatch = &per_cpu(cpu_fbatches.activate, cpu);

	if (folio_batch_count(fbatch))
		folio_batch_move_lru(fbatch, folio_activate_fn);
}

void folio_activate(struct folio *folio)
{
	if (folio_test_lru(folio) && !folio_test_active(folio) &&
	    !folio_test_unevictable(folio)) {
		struct folio_batch *fbatch;

		folio_get(folio);
		local_lock(&cpu_fbatches.lock);
		fbatch = this_cpu_ptr(&cpu_fbatches.activate);
		folio_batch_add_and_move(fbatch, folio, folio_activate_fn);
		local_unlock(&cpu_fbatches.lock);
	}
}

#else
static inline void folio_activate_drain(int cpu)
{
}

void folio_activate(struct folio *folio)
{
	struct lruvec *lruvec;

	if (folio_test_clear_lru(folio)) {
		lruvec = folio_lruvec_lock_irq(folio);
		folio_activate_fn(lruvec, folio);
		unlock_page_lruvec_irq(lruvec);
		folio_set_lru(folio);
	}
}
#endif

static void __lru_cache_activate_folio(struct folio *folio)
{
	struct folio_batch *fbatch;
	int i;

	local_lock(&cpu_fbatches.lock);
	fbatch = this_cpu_ptr(&cpu_fbatches.lru_add);

	/*
	 * Search backwards on the optimistic assumption that the folio being
	 * activated has just been added to this batch. Note that only
	 * the local batch is examined as a !LRU folio could be in the
	 * process of being released, reclaimed, migrated or on a remote
	 * batch that is currently being drained. Furthermore, marking
	 * a remote batch's folio active potentially hits a race where
	 * a folio is marked active just after it is added to the inactive
	 * list causing accounting errors and BUG_ON checks to trigger.
	 */
	for (i = folio_batch_count(fbatch) - 1; i >= 0; i--) {
		struct folio *batch_folio = fbatch->folios[i];

		if (batch_folio == folio) {
			folio_set_active(folio);
			break;
		}
	}

	local_unlock(&cpu_fbatches.lock);
}

#ifdef CONFIG_LRU_GEN
static void folio_inc_refs(struct folio *folio)
{
	unsigned long new_flags, old_flags = READ_ONCE(folio->flags);

	if (folio_test_unevictable(folio))
		return;

	if (!folio_test_referenced(folio)) {
		folio_set_referenced(folio);
		return;
	}

	if (!folio_test_workingset(folio)) {
		folio_set_workingset(folio);
		return;
	}

	/* see the comment on MAX_NR_TIERS */
	do {
		new_flags = old_flags & LRU_REFS_MASK;
		if (new_flags == LRU_REFS_MASK)
			break;

		new_flags += BIT(LRU_REFS_PGOFF);
		new_flags |= old_flags & ~LRU_REFS_MASK;
	} while (!try_cmpxchg(&folio->flags, &old_flags, new_flags));
}
#else
static void folio_inc_refs(struct folio *folio)
{
}
#endif /* CONFIG_LRU_GEN */

/*
 * Mark a page as having seen activity.
 *
 * inactive,unreferenced	->	inactive,referenced
 * inactive,referenced		->	active,unreferenced
 * active,unreferenced		->	active,referenced
 *
 * When a newly allocated page is not yet visible, so safe for non-atomic ops,
 * __SetPageReferenced(page) may be substituted for mark_page_accessed(page).
 */
void folio_mark_accessed(struct folio *folio)
{
	if (lru_gen_enabled()) {
		folio_inc_refs(folio);
		return;
	}

	if (!folio_test_referenced(folio)) {
		folio_set_referenced(folio);
	} else if (folio_test_unevictable(folio)) {
		/*
		 * Unevictable pages are on the "LRU_UNEVICTABLE" list. But,
		 * this list is never rotated or maintained, so marking an
		 * unevictable page accessed has no effect.
		 */
	} else if (!folio_test_active(folio)) {
		/*
		 * If the folio is on the LRU, queue it for activation via
		 * cpu_fbatches.activate. Otherwise, assume the folio is in a
		 * folio_batch, mark it active and it'll be moved to the active
		 * LRU on the next drain.
		 */
		if (folio_test_lru(folio))
			folio_activate(folio);
		else
			__lru_cache_activate_folio(folio);
		folio_clear_referenced(folio);
		workingset_activation(folio);
	}
	if (folio_test_idle(folio))
		folio_clear_idle(folio);
}
EXPORT_SYMBOL(folio_mark_accessed);

/**
 * folio_add_lru - Add a folio to an LRU list.
 * @folio: The folio to be added to the LRU.
 *
 * Queue the folio for addition to the LRU. The decision on whether
 * to add the page to the [in]active [file|anon] list is deferred until the
 * folio_batch is drained. This gives a chance for the caller of folio_add_lru()
 * have the folio added to the active list using folio_mark_accessed().
 */
void folio_add_lru(struct folio *folio)
{
	struct folio_batch *fbatch;

	VM_BUG_ON_FOLIO(folio_test_active(folio) &&
			folio_test_unevictable(folio), folio);
	VM_BUG_ON_FOLIO(folio_test_lru(folio), folio);

	/* see the comment in lru_gen_add_folio() */
	if (lru_gen_enabled() && !folio_test_unevictable(folio) &&
	    lru_gen_in_fault() && !(current->flags & PF_MEMALLOC))
		folio_set_active(folio);

	folio_get(folio);
	local_lock(&cpu_fbatches.lock);
	fbatch = this_cpu_ptr(&cpu_fbatches.lru_add);
	folio_batch_add_and_move(fbatch, folio, lru_add_fn);
	local_unlock(&cpu_fbatches.lock);
}
EXPORT_SYMBOL(folio_add_lru);

/**
 * folio_add_lru_vma() - Add a folio to the appropate LRU list for this VMA.
 * @folio: The folio to be added to the LRU.
 * @vma: VMA in which the folio is mapped.
 *
 * If the VMA is mlocked, @folio is added to the unevictable list.
 * Otherwise, it is treated the same way as folio_add_lru().
 */
void folio_add_lru_vma(struct folio *folio, struct vm_area_struct *vma)
{
	VM_BUG_ON_FOLIO(folio_test_lru(folio), folio);

	if (unlikely((vma->vm_flags & (VM_LOCKED | VM_SPECIAL)) == VM_LOCKED))
		mlock_new_folio(folio);
	else
		folio_add_lru(folio);
}

/*
 * If the folio cannot be invalidated, it is moved to the
 * inactive list to speed up its reclaim.  It is moved to the
 * head of the list, rather than the tail, to give the flusher
 * threads some time to write it out, as this is much more
 * effective than the single-page writeout from reclaim.
 *
 * If the folio isn't mapped and dirty/writeback, the folio
 * could be reclaimed asap using the reclaim flag.
 *
 * 1. active, mapped folio -> none
 * 2. active, dirty/writeback folio -> inactive, head, reclaim
 * 3. inactive, mapped folio -> none
 * 4. inactive, dirty/writeback folio -> inactive, head, reclaim
 * 5. inactive, clean -> inactive, tail
 * 6. Others -> none
 *
 * In 4, it moves to the head of the inactive list so the folio is
 * written out by flusher threads as this is much more efficient
 * than the single-page writeout from reclaim.
 */
static void lru_deactivate_file_fn(struct lruvec *lruvec, struct folio *folio)
{
	bool active = folio_test_active(folio);
	long nr_pages = folio_nr_pages(folio);

	if (folio_test_unevictable(folio))
		return;

	/* Some processes are using the folio */
	if (folio_mapped(folio))
		return;

	lruvec_del_folio(lruvec, folio);
	folio_clear_active(folio);
	folio_clear_referenced(folio);

	if (folio_test_writeback(folio) || folio_test_dirty(folio)) {
		/*
		 * Setting the reclaim flag could race with
		 * folio_end_writeback() and confuse readahead.  But the
		 * race window is _really_ small and  it's not a critical
		 * problem.
		 */
		lruvec_add_folio(lruvec, folio);
		folio_set_reclaim(folio);
	} else {
		/*
		 * The folio's writeback ended while it was in the batch.
		 * We move that folio to the tail of the inactive list.
		 */
		lruvec_add_folio_tail(lruvec, folio);
		__count_vm_events(PGROTATED, nr_pages);
	}

	if (active) {
		__count_vm_events(PGDEACTIVATE, nr_pages);
		__count_memcg_events(lruvec_memcg(lruvec), PGDEACTIVATE,
				     nr_pages);
	}
}

static void lru_deactivate_fn(struct lruvec *lruvec, struct folio *folio)
{
	if (!folio_test_unevictable(folio) && (folio_test_active(folio) || lru_gen_enabled())) {
		long nr_pages = folio_nr_pages(folio);

		lruvec_del_folio(lruvec, folio);
		folio_clear_active(folio);
		folio_clear_referenced(folio);
		lruvec_add_folio(lruvec, folio);

		__count_vm_events(PGDEACTIVATE, nr_pages);
		__count_memcg_events(lruvec_memcg(lruvec), PGDEACTIVATE,
				     nr_pages);
	}
}

static void lru_lazyfree_fn(struct lruvec *lruvec, struct folio *folio)
{
	if (folio_test_anon(folio) && folio_test_swapbacked(folio) &&
	    !folio_test_swapcache(folio) && !folio_test_unevictable(folio)) {
		long nr_pages = folio_nr_pages(folio);

		lruvec_del_folio(lruvec, folio);
		folio_clear_active(folio);
		folio_clear_referenced(folio);
		/*
		 * Lazyfree folios are clean anonymous folios.  They have
		 * the swapbacked flag cleared, to distinguish them from normal
		 * anonymous folios
		 */
		folio_clear_swapbacked(folio);
		lruvec_add_folio(lruvec, folio);

		__count_vm_events(PGLAZYFREE, nr_pages);
		__count_memcg_events(lruvec_memcg(lruvec), PGLAZYFREE,
				     nr_pages);
	}
}

/*
 * lru_add_drain_cpu - 排空指定 CPU 的 LRU 批处理队列
 * @cpu: 要排空的 CPU 编号
 *
 * 将指定 CPU 上所有 Per-CPU LRU 批处理队列中的页面刷新到全局 LRU 链表中。
 * 这是 LRU 批处理机制的核心排空函数。
 *
 * 调用上下文：
 * - "cpu" 必须是当前 CPU（抢占已被禁用）
 * - 或者 "cpu" 正在被热拔出，此时它已经处于死亡状态
 *
 * 处理的批处理队列（按顺序）：
 * 1. lru_add: 将新页面添加到 LRU 链表
 * 2. lru_rotate: 将页面移动到 inactive 链表尾部（用于回收提示）
 * 3. lru_deactivate_file: 去活化文件页（加速回收）
 * 4. lru_deactivate: 去活化页面
 * 5. lru_lazyfree: 标记匿名页为延迟释放
 * 6. activate: 激活页面（仅 SMP 系统）
 *
 * 设计要点：
 * - 批处理机制用于提高性能：减少对 LRU 锁的争用
 * - 延迟处理：页面操作先缓存在 Per-CPU 队列中，批量刷新时才真正操作 LRU 链表
 * - 内存一致性：在某些关键路径上（如页面迁移、mlock等）必须先排空这些队列
 */
/*
 * Drain pages out of the cpu's folio_batch.
 * Either "cpu" is the current CPU, and preemption has already been
 * disabled; or "cpu" is being hot-unplugged, and is already dead.
 */
void lru_add_drain_cpu(int cpu)
{
	struct cpu_fbatches *fbatches = &per_cpu(cpu_fbatches, cpu);
	struct folio_batch *fbatch = &fbatches->lru_add;

	if (folio_batch_count(fbatch))
		folio_batch_move_lru(fbatch, lru_add_fn);

	fbatch = &per_cpu(lru_rotate.fbatch, cpu);
	/* Disabling interrupts below acts as a compiler barrier. */
	if (data_race(folio_batch_count(fbatch))) {
		unsigned long flags;

		/* No harm done if a racing interrupt already did this */
		local_lock_irqsave(&lru_rotate.lock, flags);
		folio_batch_move_lru(fbatch, lru_move_tail_fn);
		local_unlock_irqrestore(&lru_rotate.lock, flags);
	}

	fbatch = &fbatches->lru_deactivate_file;
	if (folio_batch_count(fbatch))
		folio_batch_move_lru(fbatch, lru_deactivate_file_fn);

	fbatch = &fbatches->lru_deactivate;
	if (folio_batch_count(fbatch))
		folio_batch_move_lru(fbatch, lru_deactivate_fn);

	fbatch = &fbatches->lru_lazyfree;
	if (folio_batch_count(fbatch))
		folio_batch_move_lru(fbatch, lru_lazyfree_fn);

	folio_activate_drain(cpu);
}

/**
 * deactivate_file_folio() - Deactivate a file folio.
 * @folio: Folio to deactivate.
 *
 * This function hints to the VM that @folio is a good reclaim candidate,
 * for example if its invalidation fails due to the folio being dirty
 * or under writeback.
 *
 * Context: Caller holds a reference on the folio.
 */
void deactivate_file_folio(struct folio *folio)
{
	struct folio_batch *fbatch;

	/* Deactivating an unevictable folio will not accelerate reclaim */
	if (folio_test_unevictable(folio))
		return;

	folio_get(folio);
	local_lock(&cpu_fbatches.lock);
	fbatch = this_cpu_ptr(&cpu_fbatches.lru_deactivate_file);
	folio_batch_add_and_move(fbatch, folio, lru_deactivate_file_fn);
	local_unlock(&cpu_fbatches.lock);
}

/*
 * folio_deactivate - deactivate a folio
 * @folio: folio to deactivate
 *
 * folio_deactivate() moves @folio to the inactive list if @folio was on the
 * active list and was not unevictable. This is done to accelerate the
 * reclaim of @folio.
 */
void folio_deactivate(struct folio *folio)
{
	if (folio_test_lru(folio) && !folio_test_unevictable(folio) &&
	    (folio_test_active(folio) || lru_gen_enabled())) {
		struct folio_batch *fbatch;

		folio_get(folio);
		local_lock(&cpu_fbatches.lock);
		fbatch = this_cpu_ptr(&cpu_fbatches.lru_deactivate);
		folio_batch_add_and_move(fbatch, folio, lru_deactivate_fn);
		local_unlock(&cpu_fbatches.lock);
	}
}

/**
 * folio_mark_lazyfree - make an anon folio lazyfree
 * @folio: folio to deactivate
 *
 * folio_mark_lazyfree() moves @folio to the inactive file list.
 * This is done to accelerate the reclaim of @folio.
 */
void folio_mark_lazyfree(struct folio *folio)
{
	if (folio_test_lru(folio) && folio_test_anon(folio) &&
	    folio_test_swapbacked(folio) && !folio_test_swapcache(folio) &&
	    !folio_test_unevictable(folio)) {
		struct folio_batch *fbatch;

		folio_get(folio);
		local_lock(&cpu_fbatches.lock);
		fbatch = this_cpu_ptr(&cpu_fbatches.lru_lazyfree);
		folio_batch_add_and_move(fbatch, folio, lru_lazyfree_fn);
		local_unlock(&cpu_fbatches.lock);
	}
}

/*
 * lru_add_drain - 排空当前 CPU 的 LRU 批处理队列
 *
 * 这是用户最常调用的接口函数，用于将当前 CPU 的所有 Per-CPU LRU 批处理队列
 * 中缓存的页面刷新到全局 LRU 链表中。
 *
 * 功能：
 * 1. 获取本地锁（禁用抢占）
 * 2. 调用 lru_add_drain_cpu() 排空所有 LRU 批处理队列
 * 3. 排空 mlock 相关的本地队列
 * 4. 释放本地锁
 *
 * 典型使用场景：
 * 1. 页面迁移前：确保待迁移页面不在 Per-CPU 队列中
 * 2. 页面隔离前：保证能看到页面的最新 LRU 状态
 * 3. mlock/munlock：确保页面的锁定状态及时生效
 * 4. 内存回收路径：获取准确的 LRU 链表状态
 * 5. 页面释放路径：将缓存页面尽快归还给页面分配器
 * 6. GUP (get_user_pages) 后：确保新映射的页面进入 LRU
 *
 * 为什么需要 lru_add_drain？
 * - LRU 批处理机制将页面操作延迟到 Per-CPU 队列中以提高性能
 * - 但在某些关键路径上需要保证内存视图的一致性和实时性
 * - 此函数确保所有延迟的页面操作立即生效
 *
 * 性能考虑：
 * - 这是一个相对昂贵的操作（需要获取 LRU 锁并遍历所有批处理队列）
 * - 应该只在确实需要一致性视图的地方调用
 * - 高频调用会抵消批处理机制的性能优势
 */
void lru_add_drain(void)
{
	local_lock(&cpu_fbatches.lock);
	lru_add_drain_cpu(smp_processor_id());
	local_unlock(&cpu_fbatches.lock);
	mlock_drain_local();
}

/*
 * lru_add_and_bh_lrus_drain - 排空 LRU 和 buffer_head LRU 缓存
 *
 * 这是一个组合排空函数，在 SMP 系统中从 per-cpu 工作队列上下文调用。
 *
 * 执行步骤：
 * 1. 排空当前 CPU 的所有 LRU 批处理队列（folio_batch）
 * 2. 使当前 CPU 的 buffer_head LRU 缓存失效
 * 3. 排空 mlock 相关的本地队列
 *
 * 调用上下文：
 * - 在 SMP 系统中从 per-cpu 工作队列调用
 * - lru_add_drain_cpu 和 invalidate_bh_lrus_cpu 应该在同一个 CPU 上运行
 * - 在非 SMP 系统中不是问题，因为核心只有一个且锁会禁用抢占
 *
 * 为什么需要同时排空？
 * - buffer_head 缓存中的页面可能也在 LRU 批处理队列中
 * - 统一排空保证内存视图的一致性
 */
/*
 * It's called from per-cpu workqueue context in SMP case so
 * lru_add_drain_cpu and invalidate_bh_lrus_cpu should run on
 * the same cpu. It shouldn't be a problem in !SMP case since
 * the core is only one and the locks will disable preemption.
 */
static void lru_add_and_bh_lrus_drain(void)
{
	local_lock(&cpu_fbatches.lock);
	lru_add_drain_cpu(smp_processor_id());
	local_unlock(&cpu_fbatches.lock);
	invalidate_bh_lrus_cpu();
	mlock_drain_local();
}

void lru_add_drain_cpu_zone(struct zone *zone)
{
	local_lock(&cpu_fbatches.lock);
	lru_add_drain_cpu(smp_processor_id());
	drain_local_pages(zone);
	local_unlock(&cpu_fbatches.lock);
	mlock_drain_local();
}

#ifdef CONFIG_SMP

static DEFINE_PER_CPU(struct work_struct, lru_add_drain_work);

static void lru_add_drain_per_cpu(struct work_struct *dummy)
{
	lru_add_and_bh_lrus_drain();
}

/*
 * cpu_needs_drain - 检查指定 CPU 是否需要排空 LRU 缓存
 * @cpu: 要检查的 CPU 编号
 *
 * 返回值：如果该 CPU 的任何批处理队列中有待处理的页面，返回 true
 *
 * 检查的队列（按可能性从大到小排序）：
 * 1. lru_add: 最常见，新页面加入 LRU
 * 2. lru_rotate: 页面轮转队列
 * 3. lru_deactivate_file: 文件页去活化
 * 4. lru_deactivate: 一般去活化
 * 5. lru_lazyfree: 延迟释放队列
 * 6. activate: 页面激活队列
 * 7. mlock: 内存锁定相关
 * 8. buffer_head: buffer_head LRU 缓存
 *
 * 注意：对 lru_rotate.fbatch 使用 data_race() 是因为它受中断锁保护，
 * 而其他队列由 cpu_fbatches.lock 保护。data_race() 表明这是一个
 * 已知的良性竞态（benign race）。
 */
static bool cpu_needs_drain(unsigned int cpu)
{
	struct cpu_fbatches *fbatches = &per_cpu(cpu_fbatches, cpu);

	/*
	 * 按照它们不为零的可能性顺序检查这些队列
	 */
	/* Check these in order of likelihood that they're not zero */
	return folio_batch_count(&fbatches->lru_add) ||
		data_race(folio_batch_count(&per_cpu(lru_rotate.fbatch, cpu))) ||
		folio_batch_count(&fbatches->lru_deactivate_file) ||
		folio_batch_count(&fbatches->lru_deactivate) ||
		folio_batch_count(&fbatches->lru_lazyfree) ||
		folio_batch_count(&fbatches->activate) ||
		need_mlock_drain(cpu) ||
		has_bh_in_lru(cpu, NULL);
}

/*
 * __lru_add_drain_all - 排空所有 CPU 的 LRU 批处理队列（内部实现）
 * @force_all_cpus: 是否强制排空所有 CPU（忽略代数优化）
 *
 * 这是 lru_add_drain_all() 的核心实现，负责协调多个 CPU 的 LRU 排空操作。
 * 使用了一个复杂的代数（generation）机制来优化高竞争场景。
 *
 * === 代数（Generation）优化机制 ===
 *
 * 问题场景：
 * 在高负载系统中，多个 CPU 可能同时调用 lru_add_drain_all()，导致：
 * - 频繁的全局同步开销
 * - 重复的 IPI（处理器间中断）
 * - 不必要的工作队列调度
 *
 * 解决方案：全局代数计数器（lru_drain_gen）
 *
 * 定义 (A)：全局 lru_drain_gen = x 意味着所有代数 0 < n <= x
 *           已经被调度进行排空操作
 *
 * 工作流程：
 * 1. 每个 CPU 在进入函数时记录当前的全局代数（B 点）
 * 2. 获取互斥锁后，检查是否有更新的代数已经被调度（C 点）
 * 3. 如果有，直接退出（避免重复工作）
 * 4. 否则，递增全局代数，执行排空操作（D 点）
 *
 * === 内存屏障和同步 ===
 *
 * smp_mb() 在进入前：
 * - 确保当前 CPU 对 folio_batch 计数器的存储对其他 CPU 可见
 * - 之后才加载全局 lru_drain_gen
 *
 * smp_load_acquire() 在 (B) 点：
 * - 确保读取代数在获取互斥锁之前完成
 * - 与 (D) 点的 smp_mb() 配对
 *
 * smp_mb() 在 (D) 点：
 * - 确保新的全局代数被存储后，才开始加载 folio_batch 计数器
 * - 防止 CPU #x 的更新被 CPU #z 漏掉
 *
 * === 关键时序示例 ===
 *
 * 假设 CPU #x < #y < #z，CPU #z 正在遍历所有 CPU 并已经检查完 CPU #y：
 *
 * 时间线：
 * T1: CPU #z 开始遍历，已检查 CPU #y，准备检查后续 CPU
 * T2: CPU #x 向其 per-cpu 向量添加页面
 * T3: CPU #x 调用 lru_add_drain_all()
 *
 * 如果内存屏障在循环后（错误做法）：
 * - CPU #x 会在 (C) 点看到相同的代数并直接退出
 * - CPU #x 的新页面被漏掉，不会被排空
 *
 * 正确做法（当前实现）：
 * - (D) 点的 smp_mb() 在循环前执行
 * - 保证代数递增和计数器加载之间的顺序
 * - CPU #x 要么被当前遍历捕获，要么会启动新一轮排空
 *
 * === 热拔插处理 ===
 *
 * 不需要 CPU 热拔插锁，因为：
 * - per-cpu 工作队列会在 page_alloc_cpu_dead 回调之前关闭
 * - 持有热拔插锁调用此函数可能导致通过 WQ 上下文产生间接依赖
 *
 * @force_all_cpus:
 * - false: 使用代数优化，如果其他 CPU 已调度更新的代数则跳过
 * - true:  强制排空所有 CPU，忽略代数优化（用于 lru_cache_disable）
 */
/*
 * Doesn't need any cpu hotplug locking because we do rely on per-cpu
 * kworkers being shut down before our page_alloc_cpu_dead callback is
 * executed on the offlined cpu.
 * Calling this function with cpu hotplug locks held can actually lead
 * to obscure indirect dependencies via WQ context.
 */
static inline void __lru_add_drain_all(bool force_all_cpus)
{
	/*
	 * lru_drain_gen - 全局页面代数编号
	 *
	 * (A) 定义：全局 lru_drain_gen = x 意味着所有代数
	 *     0 < n <= x 已经被*调度*进行排空操作。
	 *
	 * 这是针对高竞争使用场景的优化，在该场景中用户空间工作负载
	 * 为每个 CPU 持续生成页面流。
	 */
	/*
	 * lru_drain_gen - Global pages generation number
	 *
	 * (A) Definition: global lru_drain_gen = x implies that all generations
	 *     0 < n <= x are already *scheduled* for draining.
	 *
	 * This is an optimization for the highly-contended use case where a
	 * user space workload keeps constantly generating a flow of pages for
	 * each CPU.
	 */
	static unsigned int lru_drain_gen;
	static struct cpumask has_work;
	static DEFINE_MUTEX(lock);
	unsigned cpu, this_gen;

	/*
	 * 确保在 mm_percpu_wq 完全初始化之前没有人触发此路径。
	 */
	/*
	 * Make sure nobody triggers this path before mm_percpu_wq is fully
	 * initialized.
	 */
	if (WARN_ON(!mm_percpu_wq))
		return;

	/*
	 * 保证当前 CPU 对 folio_batch 计数器的存储对其他 CPU 可见，
	 * 然后再加载当前的排空代数。
	 */
	/*
	 * Guarantee folio_batch counter stores visible by this CPU
	 * are visible to other CPUs before loading the current drain
	 * generation.
	 */
	smp_mb();

	/*
	 * (B) 本地缓存全局 LRU 排空代数编号
	 *
	 * 读屏障确保在获取互斥锁之前加载计数器。
	 * 它与 (D) 点互斥临界区内的 smp_mb() 配对。
	 */
	/*
	 * (B) Locally cache global LRU draining generation number
	 *
	 * The read barrier ensures that the counter is loaded before the mutex
	 * is taken. It pairs with smp_mb() inside the mutex critical section
	 * at (D).
	 */
	this_gen = smp_load_acquire(&lru_drain_gen);

	mutex_lock(&lock);

	/*
	 * (C) 如果来自另一个 lru_add_drain_all() 的更新代数已经被
	 * 调度进行排空，则退出排空操作。参见检查 (A)。
	 */
	/*
	 * (C) Exit the draining operation if a newer generation, from another
	 * lru_add_drain_all(), was already scheduled for draining. Check (A).
	 */
	if (unlikely(this_gen != lru_drain_gen && !force_all_cpus))
		goto done;

	/*
	 * (D) 递增全局代数编号
	 *
	 * 与临界区外的 (B) 点的 smp_load_acquire() 配对。
	 * 使用完全内存屏障以保证新的全局排空代数编号被存储后，
	 * 再加载 folio_batch 计数器。
	 *
	 * 此配对必须在这里完成，在下面的 for_each_online_cpu 循环之前，
	 * 该循环排空页面向量。
	 *
	 * 设 x, y, z 代表一些系统 CPU 编号，其中 x < y < z。
	 * 假设 CPU #z 在下面的 for_each_online_cpu 循环中间，
	 * 并且已经到达 CPU #y 的 per-cpu 数据。CPU #x 出现，
	 * 向其 per-cpu 向量添加一些页面，然后调用 lru_add_drain_all()。
	 *
	 * 如果配对屏障在稍后的步骤完成，例如在循环之后，
	 * CPU #x 将只会在 (C) 点退出并错过刷新其所有添加的页面。
	 */
	/*
	 * (D) Increment global generation number
	 *
	 * Pairs with smp_load_acquire() at (B), outside of the critical
	 * section. Use a full memory barrier to guarantee that the
	 * new global drain generation number is stored before loading
	 * folio_batch counters.
	 *
	 * This pairing must be done here, before the for_each_online_cpu loop
	 * below which drains the page vectors.
	 *
	 * Let x, y, and z represent some system CPU numbers, where x < y < z.
	 * Assume CPU #z is in the middle of the for_each_online_cpu loop
	 * below and has already reached CPU #y's per-cpu data. CPU #x comes
	 * along, adds some pages to its per-cpu vectors, then calls
	 * lru_add_drain_all().
	 *
	 * If the paired barrier is done at any later step, e.g. after the
	 * loop, CPU #x will just exit at (C) and miss flushing out all of its
	 * added pages.
	 */
	WRITE_ONCE(lru_drain_gen, lru_drain_gen + 1);
	smp_mb();

	cpumask_clear(&has_work);
	/*
	 * 遍历所有在线 CPU，检查是否需要排空
	 */
	for_each_online_cpu(cpu) {
		struct work_struct *work = &per_cpu(lru_add_drain_work, cpu);

		if (cpu_needs_drain(cpu)) {
			/*
			 * 为需要排空的 CPU 初始化工作项并加入工作队列
			 */
			INIT_WORK(work, lru_add_drain_per_cpu);
			queue_work_on(cpu, mm_percpu_wq, work);
			__cpumask_set_cpu(cpu, &has_work);
		}
	}

	/*
	 * 等待所有排空工作完成
	 */
	for_each_cpu(cpu, &has_work)
		flush_work(&per_cpu(lru_add_drain_work, cpu));

done:
	mutex_unlock(&lock);
}

/*
 * lru_add_drain_all - 排空所有在线 CPU 的 LRU 批处理队列
 *
 * 这是系统级的 LRU 排空接口，用于确保所有 CPU 的 LRU 批处理队列被刷新。
 *
 * 功能：
 * - 协调所有在线 CPU 的 LRU 排空操作
 * - 使用工作队列机制在各个 CPU 上异步执行排空
 * - 使用代数优化避免重复工作
 * - 等待所有排空工作完成后返回
 *
 * 典型使用场景：
 * 1. 内存压缩（compaction）：需要全局一致的内存视图
 * 2. 内存迁移：确保待迁移页面不在任何 CPU 的批处理队列中
 * 3. 内存热拔插：在移除内存前确保页面状态一致
 * 4. 内存统计：获取准确的全局 LRU 统计信息
 * 5. 某些系统调用（如 mlock）需要全局一致的页面状态
 *
 * 性能影响：
 * - 这是一个非常昂贵的操作（需要所有 CPU 协作）
 * - 会导致全系统的 IPI 和工作队列调度
 * - 应该谨慎使用，只在确实需要全局一致性时调用
 * - 代数优化机制可以减轻高频调用的影响
 *
 * 与 lru_add_drain() 的区别：
 * - lru_add_drain(): 仅排空当前 CPU，开销小，局部影响
 * - lru_add_drain_all(): 排空所有 CPU，开销大，全局影响
 */
void lru_add_drain_all(void)
{
	__lru_add_drain_all(false);
}
#else
void lru_add_drain_all(void)
{
	lru_add_drain();
}
#endif /* CONFIG_SMP */

atomic_t lru_disable_count = ATOMIC_INIT(0);

/*
 * lru_cache_disable() needs to be called before we start compiling
 * a list of pages to be migrated using isolate_lru_page().
 * It drains pages on LRU cache and then disable on all cpus until
 * lru_cache_enable is called.
 *
 * Must be paired with a call to lru_cache_enable().
 */
void lru_cache_disable(void)
{
	atomic_inc(&lru_disable_count);
	/*
	 * Readers of lru_disable_count are protected by either disabling
	 * preemption or rcu_read_lock:
	 *
	 * preempt_disable, local_irq_disable  [bh_lru_lock()]
	 * rcu_read_lock		       [rt_spin_lock CONFIG_PREEMPT_RT]
	 * preempt_disable		       [local_lock !CONFIG_PREEMPT_RT]
	 *
	 * Since v5.1 kernel, synchronize_rcu() is guaranteed to wait on
	 * preempt_disable() regions of code. So any CPU which sees
	 * lru_disable_count = 0 will have exited the critical
	 * section when synchronize_rcu() returns.
	 */
	synchronize_rcu_expedited();
#ifdef CONFIG_SMP
	__lru_add_drain_all(true);
#else
	lru_add_and_bh_lrus_drain();
#endif
}

/**
 * release_pages - batched put_page()
 * @arg: array of pages to release
 * @nr: number of pages
 *
 * Decrement the reference count on all the pages in @arg.  If it
 * fell to zero, remove the page from the LRU and free it.
 *
 * Note that the argument can be an array of pages, encoded pages,
 * or folio pointers. We ignore any encoded bits, and turn any of
 * them into just a folio that gets free'd.
 */
void release_pages(release_pages_arg arg, int nr)
{
	int i;
	struct encoded_page **encoded = arg.encoded_pages;
	LIST_HEAD(pages_to_free);
	struct lruvec *lruvec = NULL;
	unsigned long flags = 0;
	unsigned int lock_batch;

	for (i = 0; i < nr; i++) {
		struct folio *folio;

		/* Turn any of the argument types into a folio */
		folio = page_folio(encoded_page_ptr(encoded[i]));

		/*
		 * Make sure the IRQ-safe lock-holding time does not get
		 * excessive with a continuous string of pages from the
		 * same lruvec. The lock is held only if lruvec != NULL.
		 */
		if (lruvec && ++lock_batch == SWAP_CLUSTER_MAX) {
			unlock_page_lruvec_irqrestore(lruvec, flags);
			lruvec = NULL;
		}

		if (is_huge_zero_page(&folio->page))
			continue;

		if (folio_is_zone_device(folio)) {
			if (lruvec) {
				unlock_page_lruvec_irqrestore(lruvec, flags);
				lruvec = NULL;
			}
			if (put_devmap_managed_page(&folio->page))
				continue;
			if (folio_put_testzero(folio))
				free_zone_device_page(&folio->page);
			continue;
		}

		if (!folio_put_testzero(folio))
			continue;

		if (folio_test_large(folio)) {
			if (lruvec) {
				unlock_page_lruvec_irqrestore(lruvec, flags);
				lruvec = NULL;
			}
			__folio_put_large(folio);
			continue;
		}

		if (folio_test_lru(folio)) {
			struct lruvec *prev_lruvec = lruvec;

			lruvec = folio_lruvec_relock_irqsave(folio, lruvec,
									&flags);
			if (prev_lruvec != lruvec)
				lock_batch = 0;

			lruvec_del_folio(lruvec, folio);
			__folio_clear_lru_flags(folio);
		}

		/*
		 * In rare cases, when truncation or holepunching raced with
		 * munlock after VM_LOCKED was cleared, Mlocked may still be
		 * found set here.  This does not indicate a problem, unless
		 * "unevictable_pgs_cleared" appears worryingly large.
		 */
		if (unlikely(folio_test_mlocked(folio))) {
			__folio_clear_mlocked(folio);
			zone_stat_sub_folio(folio, NR_MLOCK);
			count_vm_event(UNEVICTABLE_PGCLEARED);
		}

		list_add(&folio->lru, &pages_to_free);
	}
	if (lruvec)
		unlock_page_lruvec_irqrestore(lruvec, flags);

	mem_cgroup_uncharge_list(&pages_to_free);
	free_unref_page_list(&pages_to_free);
}
EXPORT_SYMBOL(release_pages);

/*
 * The folios which we're about to release may be in the deferred lru-addition
 * queues.  That would prevent them from really being freed right now.  That's
 * OK from a correctness point of view but is inefficient - those folios may be
 * cache-warm and we want to give them back to the page allocator ASAP.
 *
 * So __folio_batch_release() will drain those queues here.
 * folio_batch_move_lru() calls folios_put() directly to avoid
 * mutual recursion.
 */
void __folio_batch_release(struct folio_batch *fbatch)
{
	if (!fbatch->percpu_pvec_drained) {
		lru_add_drain();
		fbatch->percpu_pvec_drained = true;
	}
	release_pages(fbatch->folios, folio_batch_count(fbatch));
	folio_batch_reinit(fbatch);
}
EXPORT_SYMBOL(__folio_batch_release);

/**
 * folio_batch_remove_exceptionals() - Prune non-folios from a batch.
 * @fbatch: The batch to prune
 *
 * find_get_entries() fills a batch with both folios and shadow/swap/DAX
 * entries.  This function prunes all the non-folio entries from @fbatch
 * without leaving holes, so that it can be passed on to folio-only batch
 * operations.
 */
void folio_batch_remove_exceptionals(struct folio_batch *fbatch)
{
	unsigned int i, j;

	for (i = 0, j = 0; i < folio_batch_count(fbatch); i++) {
		struct folio *folio = fbatch->folios[i];
		if (!xa_is_value(folio))
			fbatch->folios[j++] = folio;
	}
	fbatch->nr = j;
}

/*
 * Perform any setup for the swap system
 */
void __init swap_setup(void)
{
	unsigned long megs = totalram_pages() >> (20 - PAGE_SHIFT);

	/* Use a smaller cluster for small-memory machines */
	if (megs < 16)
		page_cluster = 2;
	else
		page_cluster = 3;
	/*
	 * Right now other parts of the system means that we
	 * _really_ don't want to cluster much more
	 */
}
