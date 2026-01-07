# tlb_flush_batched 字段详解

## 一、概述

`tlb_flush_batched` 是 `mm_struct` 结构体中的一个原子计数器，用于跟踪页面回收（reclaim）期间的**批量 TLB 刷新状态**。它解决了一个关键的并发竞争问题：页面回收操作在批量 TLB 刷新模式下可能与 `mprotect()` 或 `munmap()` 等系统调用产生竞争，导致出现**陈旧的 TLB 表项**。

---

## 二、核心问题：批量 TLB 刷新的竞争

### 2.1 问题场景

**位置**：`mm/rmap.c:698-711`

```c
/*
 * Reclaim unmaps pages under the PTL but do not flush the TLB prior to
 * releasing the PTL if TLB flushes are batched. It's possible for a parallel
 * operation such as mprotect or munmap to race between reclaim unmapping
 * the page and flushing the page. If this race occurs, it potentially allows
 * access to data via a stale TLB entry.
 */
```

#### 问题分析

1. **页面回收（reclaim）操作**：
   - 在持有 PTL (Page Table Lock) 的情况下解除页面映射（unmap）
   - 如果启用了批量 TLB 刷新，**不会立即刷新 TLB**
   - 释放 PTL 锁

2. **并行系统调用（如 mprotect/munmap）**：
   - 在回收解除映射和刷新 TLB 之间执行
   - 可能读取到**陈旧的 TLB 表项**（stale TLB entry）
   - 通过陈旧表项访问已解除映射的页面数据

3. **安全风险**：
   - 访问已释放的页面数据
   - 绕过权限检查（如 mprotect 修改的权限）

### 2.2 为什么不追踪所有正在批量刷新的 mm？

```
Tracking all mm's that have TLB batching in flight would be expensive during
reclaim so instead track whether TLB batching occurred in the past.
```

**设计权衡**：
- ❌ **实时追踪所有批量刷新中的 mm**：开销太大
- ✅ **记录历史批量刷新状态**：简单高效
- **代价**：每次回收周期中，第一个风险操作（mprotect/munmap）需要额外刷新一次 TLB

---

## 三、tlb_flush_batched 的设计

### 3.1 字段定义

**位置**：`include/linux/mm_types.h:874-877`

```c
#ifdef CONFIG_ARCH_WANT_BATCHED_UNMAP_TLB_FLUSH
	/* See flush_tlb_batched_pending() */
	atomic_t tlb_flush_batched;
#endif
```

### 3.2 数据结构：位分段编码

**位置**：`mm/rmap.c:636-643`

```c
/*
 * Bits 0-14 of mm->tlb_flush_batched record pending generations.
 * Bits 16-30 of mm->tlb_flush_batched bit record flushed generations.
 */
#define TLB_FLUSH_BATCH_FLUSHED_SHIFT	16
#define TLB_FLUSH_BATCH_PENDING_MASK			\
	((1 << (TLB_FLUSH_BATCH_FLUSHED_SHIFT - 1)) - 1)
#define TLB_FLUSH_BATCH_PENDING_LARGE			\
	(TLB_FLUSH_BATCH_PENDING_MASK / 2)
```

#### 位字段布局

```
  31                  16 15                   0
  +--------------------+---------------------+
  |   flushed (15位)   |   pending (15位)    |
  +--------------------+---------------------+
```

- **Bits 0-14 (pending)**：记录**待处理的批量刷新代数**（pending generations）
- **Bits 16-30 (flushed)**：记录**已完成的批量刷新代数**（flushed generations）

#### 设计原理

- **代数差异**：`pending != flushed` 表示有未完成的批量 TLB 刷新
- **原子操作**：使用原子递增和 CAS 操作保证并发安全
- **溢出保护**：当 pending 过大时重置为 1，防止溢出

---

## 四、关键函数分析

### 4.1 set_tlb_ubc_flush_pending() - 设置批量刷新待处理状态

**位置**：`mm/rmap.c:645-684`

```c
static void set_tlb_ubc_flush_pending(struct mm_struct *mm, pte_t pteval,
				      unsigned long uaddr)
{
	struct tlbflush_unmap_batch *tlb_ubc = &current->tlb_ubc;
	int batch;
	bool writable = pte_dirty(pteval);

	if (!pte_accessible(mm, pteval))
		return;

	arch_tlbbatch_add_pending(&tlb_ubc->arch, mm, uaddr);
	tlb_ubc->flush_required = true;

	/*
	 * Ensure compiler does not re-order the setting of tlb_flush_batched
	 * before the PTE is cleared.
	 */
	barrier();
	batch = atomic_read(&mm->tlb_flush_batched);
retry:
	if ((batch & TLB_FLUSH_BATCH_PENDING_MASK) > TLB_FLUSH_BATCH_PENDING_LARGE) {
		/*
		 * Prevent `pending' from catching up with `flushed' because of
		 * overflow.  Reset `pending' and `flushed' to be 1 and 0 if
		 * `pending' becomes large.
		 */
		if (!atomic_try_cmpxchg(&mm->tlb_flush_batched, &batch, 1))
			goto retry;
	} else {
		atomic_inc(&mm->tlb_flush_batched);
	}

	/*
	 * If the PTE was dirty then it's best to assume it's writable. The
	 * caller must use try_to_unmap_flush_dirty() or try_to_unmap_flush()
	 * before the page is queued for IO.
	 */
	if (writable)
		tlb_ubc->writable = true;
}
```

#### 关键步骤

1. **检查 PTE 可访问性**：
   - 如果 PTE 不可访问（`!pte_accessible`），直接返回
   - 避免对无效页面进行批量刷新追踪

2. **架构相关添加**：
   - 调用 `arch_tlbbatch_add_pending()` 将页面添加到批量刷新队列
   - x86：记录 CPU mask 和增加 TLB generation
   - ARM64：直接发出 TLBI 指令（不等待 DSB 同步）

3. **内存屏障**：
   - `barrier()` 确保编译器不会重排序，在 PTE 清除之前设置 `tlb_flush_batched`
   - 保证其他 CPU 能看到正确的顺序

4. **原子递增 pending 计数**：
   - 检查是否溢出（pending > PENDING_LARGE）
   - 如果溢出：CAS 重置为 1（pending=1, flushed=0）
   - 否则：原子递增 `tlb_flush_batched`

5. **记录脏页状态**：
   - 如果 PTE 是脏的，设置 `tlb_ubc->writable = true`
   - 调用者必须在页面排队 IO 之前调用 `try_to_unmap_flush_dirty()`

### 4.2 flush_tlb_batched_pending() - 刷新待处理的批量 TLB

**位置**：`mm/rmap.c:713-728`

```c
void flush_tlb_batched_pending(struct mm_struct *mm)
{
	int batch = atomic_read(&mm->tlb_flush_batched);
	int pending = batch & TLB_FLUSH_BATCH_PENDING_MASK;
	int flushed = batch >> TLB_FLUSH_BATCH_FLUSHED_SHIFT;

	if (pending != flushed) {
		arch_flush_tlb_batched_pending(mm);
		/*
		 * If the new TLB flushing is pending during flushing, leave
		 * mm->tlb_flush_batched as is, to avoid losing flushing.
		 */
		atomic_cmpxchg(&mm->tlb_flush_batched, batch,
			       pending | (pending << TLB_FLUSH_BATCH_FLUSHED_SHIFT));
	}
}
```

#### 工作流程

1. **读取当前状态**：
   - 原子读取 `tlb_flush_batched`
   - 提取 pending 和 flushed 值

2. **比较代数**：
   - 如果 `pending != flushed`：存在未刷新的批量 TLB
   - 如果 `pending == flushed`：所有批量刷新已完成，无需操作

3. **执行架构相关刷新**：
   - x86：调用 `flush_tlb_mm(mm)` 刷新所有 CPU 的 TLB
   - ARM64：执行 `dsb(ish)` 等待所有先前的 TLBI 完成

4. **更新 flushed 计数**：
   - CAS 操作：设置 `flushed = pending`
   - 格式：`pending | (pending << 16)`
   - **关键**：如果在刷新期间有新的批量刷新（pending 增加），CAS 会失败
   - **设计意图**：避免丢失正在进行的新刷新（"avoid losing flushing"）

#### 调用时机

**必须在 PTL (Page Table Lock) 保护下调用**：

```c
/*
 * This must be called under the PTL so that an access to tlb_flush_batched
 * that is potentially a "reclaim vs mprotect/munmap/etc" race will synchronise
 * via the PTL.
 */
```

- 确保与回收操作的内存访问同步
- 防止竞争条件

---

## 五、架构相关实现

### 5.1 支持的架构

**位置**：`arch/x86/Kconfig:125` 和 `arch/arm64/Kconfig:100`

```kconfig
# x86 架构
select ARCH_WANT_BATCHED_UNMAP_TLB_FLUSH

# ARM64 架构
select ARCH_WANT_BATCHED_UNMAP_TLB_FLUSH
```

目前支持：
- **x86** (x86_64)
- **ARM64** (aarch64)

### 5.2 x86 实现

**位置**：`arch/x86/include/asm/tlbflush.h:280-294`

#### (1) arch_tlbbatch_add_pending()

```c
static inline void arch_tlbbatch_add_pending(struct arch_tlbflush_unmap_batch *batch,
					     struct mm_struct *mm,
					     unsigned long uaddr)
{
	inc_mm_tlb_gen(mm);
	cpumask_or(&batch->cpumask, &batch->cpumask, mm_cpumask(mm));
	mmu_notifier_arch_invalidate_secondary_tlbs(mm, 0, -1UL);
}
```

**操作**：
- 增加 mm 的 TLB generation 计数
- 将 mm 的 CPU mask 合并到批量刷新的 cpumask
- 通知 MMU notifier（用于虚拟化场景）

#### (2) arch_flush_tlb_batched_pending()

```c
static inline void arch_flush_tlb_batched_pending(struct mm_struct *mm)
{
	flush_tlb_mm(mm);
}
```

**操作**：
- 刷新整个 mm 的 TLB（所有 CPU）
- 发送 IPI（处理器间中断）到所有相关 CPU

### 5.3 ARM64 实现

**位置**：`arch/arm64/include/asm/tlbflush.h:300-330`

#### (1) arch_tlbbatch_add_pending()

```c
static inline void arch_tlbbatch_add_pending(struct arch_tlbflush_unmap_batch *batch,
					     struct mm_struct *mm,
					     unsigned long uaddr)
{
	__flush_tlb_page_nosync(mm, uaddr);
}
```

**操作**：
- 发出 TLBI 指令刷新单个页面的 TLB
- **不等待同步**（no DSB）
- 延迟所有 DSB 到 `arch_tlbbatch_flush()`

#### (2) arch_flush_tlb_batched_pending()

```c
static inline void arch_flush_tlb_batched_pending(struct mm_struct *mm)
{
	dsb(ish);
}
```

**操作**：
- 执行 DSB (Data Synchronization Barrier) 指令
- 等待所有先前的 TLBI 指令完成
- `ish` (Inner Shareable)：影响所有共享内存的 CPU

#### (3) arch_tlbbatch_flush()

```c
/*
 * To support TLB batched flush for multiple pages unmapping, we only send
 * the TLBI for each page in arch_tlbbatch_add_pending() and wait for the
 * completion at the end in arch_tlbbatch_flush(). Since we've already issued
 * TLBI for each page so only a DSB is needed to synchronise its effect on the
 * other CPUs.
 *
 * This will save the time waiting on DSB comparing issuing a TLBI;DSB sequence
 * for each page.
 */
static inline void arch_tlbbatch_flush(struct arch_tlbflush_unmap_batch *batch)
{
	dsb(ish);
}
```

**优化策略**：
- 为每个页面发送 TLBI，但不等待
- 最后统一执行一次 DSB
- **性能提升**：避免为每个页面都执行 TLBI;DSB 序列

---

## 六、使用场景分析

### 6.1 场景：页面回收（Page Reclaim）

**位置**：`mm/rmap.c:1600-1614` (try_to_unmap_one)

```c
if (should_defer_flush(mm, flags)) {
	/*
	 * We clear the PTE but do not flush so potentially
	 * a remote CPU could still be writing to the folio.
	 * If the entry was previously clean then the
	 * architecture must guarantee that a clear->dirty
	 * transition on a cached TLB entry is written through
	 * and traps if the PTE is unmapped.
	 */
	pteval = ptep_get_and_clear(mm, address, pvmw.pte);

	set_tlb_ubc_flush_pending(mm, pteval, address);
} else {
	pteval = ptep_clear_flush(vma, address, pvmw.pte);
}
```

#### 工作流程

1. **判断是否延迟刷新**：
   - `should_defer_flush(mm, flags)` 检查 `TTU_BATCH_FLUSH` 标志
   - 调用 `arch_tlbbatch_should_defer(mm)` 判断架构是否支持

2. **如果延迟刷新**：
   - 清除 PTE：`ptep_get_and_clear()`
   - **不刷新 TLB**
   - 调用 `set_tlb_ubc_flush_pending()` 记录待处理状态

3. **硬件保证**：
   - 架构必须保证：缓存的 TLB 表项上的 clean→dirty 转换会被写入
   - 如果 PTE 已解除映射，访问会触发陷阱（trap）

### 6.2 场景：mprotect() 系统调用

**调用路径**：

```
mprotect()
  → change_protection()
    → change_pte_range()
      → flush_tlb_batched_pending(vma->vm_mm)
```

#### 关键代码（假设）

```c
void change_pte_range(...)
{
	spinlock(pte_lock);

	/* 刷新任何待处理的批量 TLB */
	flush_tlb_batched_pending(vma->vm_mm);

	/* 修改 PTE 权限 */
	for_each_pte(...) {
		ptep_modify_prot_start(...);
		pte = pte_modify(...);
		ptep_modify_prot_commit(...);
	}

	spinunlock(pte_lock);
}
```

#### 为什么需要刷新？

1. **竞争场景**：
   - 线程 A：页面回收，清除 PTE，延迟 TLB 刷新
   - 线程 B：mprotect() 修改页面权限为只读
   - **风险**：如果不刷新，线程 A 的陈旧 TLB 表项仍然可写

2. **同步点**：
   - 在 PTL 保护下调用 `flush_tlb_batched_pending()`
   - 确保在修改权限之前，所有陈旧的 TLB 表项已失效

### 6.3 场景：munmap() 系统调用

**调用路径**：

```
munmap()
  → do_munmap()
    → unmap_region()
      → free_pgtables()
        → flush_tlb_batched_pending(mm)
```

#### 为什么需要刷新？

1. **竞争场景**：
   - 线程 A：页面回收，清除 PTE，延迟 TLB 刷新
   - 线程 B：munmap() 释放页表
   - **风险**：如果不刷新，释放的页表可能被陈旧 TLB 表项引用

2. **同步点**：
   - 在释放页表之前刷新所有批量 TLB
   - 确保页表不会被意外访问

---

## 七、关键设计要点

### 7.1 为什么使用代数（generation）而不是布尔值？

#### 问题

如果使用简单的布尔标志：

```c
atomic_t tlb_flush_batched;  // 0 = no batching, 1 = batching
```

**竞争问题**：

```
时间线：
T1: 回收操作 A 开始，设置 tlb_flush_batched = 1
T2: mprotect() 读取 tlb_flush_batched = 1，刷新 TLB
T3: mprotect() 设置 tlb_flush_batched = 0
T4: 回收操作 B 开始，设置 tlb_flush_batched = 1
T5: mprotect() 读取 tlb_flush_batched = 1，刷新 TLB（错误地刷新了两次）
T6: 回收操作 A 完成
T7: 回收操作 B 完成
```

**问题**：无法区分"新的批量刷新"和"旧的批量刷新"

#### 解决方案：代数计数

```c
pending 和 flushed 分别记录：
- pending: 当前批量刷新的"代数"
- flushed: 上次刷新时的"代数"
```

**优势**：
- 可以区分多个批量刷新操作
- `pending != flushed` 准确判断是否有未刷新的批量操作
- 支持并发的多个批量刷新

### 7.2 为什么在 PTL 保护下调用？

```c
/*
 * This must be called under the PTL so that an access to tlb_flush_batched
 * that is potentially a "reclaim vs mprotect/munmap/etc" race will synchronise
 * via the PTL.
 */
```

#### 原因

1. **内存顺序保证**：
   - PTL 提供获取-释放语义（acquire-release semantics）
   - 确保在 PTL 保护下的内存访问对所有 CPU 可见

2. **避免竞争**：
   - 回收操作在 PTL 下清除 PTE 并设置 `tlb_flush_batched`
   - mprotect/munmap 在 PTL 下读取 `tlb_flush_batched` 并刷新
   - 通过 PTL 同步，确保操作顺序正确

3. **与 PTE 修改原子性**：
   - 刷新 TLB 和修改 PTE 必须是原子的
   - PTL 保护整个临界区

### 7.3 为什么 CAS 可能失败？

**位置**：`mm/rmap.c:720-726`

```c
/*
 * If the new TLB flushing is pending during flushing, leave
 * mm->tlb_flush_batched as is, to avoid losing flushing.
 */
atomic_cmpxchg(&mm->tlb_flush_batched, batch,
	       pending | (pending << TLB_FLUSH_BATCH_FLUSHED_SHIFT));
```

#### 场景

```
时间线：
T1: mprotect() 读取 batch = 0x00010001 (pending=1, flushed=1)
T2: mprotect() 调用 arch_flush_tlb_batched_pending()
T3: 【并发】回收操作递增 pending: 0x00010002 (pending=2, flushed=1)
T4: mprotect() 尝试 CAS(0x00010001, 0x00010001) → 失败
T5: mprotect() 的 CAS 失败，保留 pending=2
```

**关键**：
- CAS 失败意味着在刷新期间有新的批量操作
- **不更新 flushed**，保留 `pending > flushed` 状态
- 下一次风险操作会再次刷新，确保新的批量操作被处理

---

## 八、与 tlb_flush_pending 的对比

### 8.1 tlb_flush_pending

**位置**：`include/linux/mm_types.h:873`

```c
atomic_t tlb_flush_pending;
```

**用途**：
- 追踪**所有**正在进行的 TLB 批量刷新（mmu_gather）
- 用于 `PROT_NONE` 页面的迁移同步

**工作机制**：
- `inc_tlb_flush_pending()`: 开始批量操作时递增
- `dec_tlb_flush_pending()`: 完成批量操作时递减
- 非零值：存在正在进行的批量刷新

### 8.2 tlb_flush_batched

**用途**：
- 追踪**页面回收**期间的延迟 TLB 刷新
- 用于 mprotect/munmap 等操作的同步

**工作机制**：
- 记录批量刷新的"历史状态"
- 使用代数（generation）区分多个批量操作
- 仅在特定风险操作时刷新

### 8.3 对比表

| 特性 | tlb_flush_pending | tlb_flush_batched |
|------|------------------|------------------|
| **追踪范围** | 所有 mmu_gather 批量操作 | 仅页面回收的延迟刷新 |
| **计数语义** | 引用计数（递增/递减） | 代数计数（pending/flushed） |
| **同步目标** | 页面迁移（PROT_NONE） | mprotect/munmap 等 |
| **刷新时机** | 批量操作完成时 | 风险操作时按需刷新 |
| **架构依赖** | 所有架构 | 仅 x86/ARM64 |
| **配置选项** | 无 | CONFIG_ARCH_WANT_BATCHED_UNMAP_TLB_FLUSH |

---

## 九、性能影响

### 9.1 优化收益

1. **批量 TLB 刷新**：
   - **x86**：减少 IPI（处理器间中断）开销
   - **ARM64**：减少 DSB 同步开销
   - **收益**：大规模页面回收时性能显著提升

2. **按需刷新**：
   - 仅在 mprotect/munmap 等风险操作时刷新
   - 避免在每次回收操作后都立即刷新

### 9.2 代价

1. **额外刷新**：
   - 每次回收周期中，第一个风险操作需要额外刷新一次
   - **代价**：一次 TLB 刷新开销

2. **内存开销**：
   - 每个 mm_struct 增加 4 字节（atomic_t）
   - 忽略不计

### 9.3 适用场景

- ✅ **大规模页面回收**：内存压力大，频繁回收
- ✅ **少量 mprotect/munmap**：风险操作频率低
- ❌ **频繁 mprotect/munmap**：额外刷新开销可能超过批量刷新收益

---

## 十、总结

### 10.1 核心作用

`tlb_flush_batched` 字段用于：
1. **追踪页面回收期间的批量 TLB 刷新状态**
2. **防止 reclaim vs mprotect/munmap 的竞争**
3. **确保陈旧 TLB 表项不会导致安全问题**

### 10.2 设计亮点

1. **代数计数**：
   - 使用 pending/flushed 两个代数
   - 准确区分多个批量刷新操作
   - 支持并发场景

2. **按需刷新**：
   - 不追踪所有正在进行的批量操作
   - 记录历史状态，按需刷新
   - 平衡性能和正确性

3. **架构抽象**：
   - 通用接口：`arch_tlbbatch_add_pending()` / `arch_flush_tlb_batched_pending()`
   - 架构特定实现：x86（IPI） / ARM64（DSB）

4. **同步机制**：
   - PTL 保护下调用，确保内存顺序
   - 原子操作和 CAS 保证并发安全

### 10.3 与 mmu_gather 的协作

- **mmu_gather**：管理批量页表操作和 TLB 刷新
- **tlb_flush_batched**：追踪页面回收的延迟刷新状态
- **协作点**：页面回收使用 mmu_gather 的批量能力，tlb_flush_batched 记录状态

### 10.4 关键要点

1. **批量刷新不是"错误"，而是"性能优化"**
2. **tlb_flush_batched 解决的是优化带来的竞争问题**
3. **设计权衡：额外的按需刷新 vs 实时追踪开销**
4. **PTL 是同步的关键保证**

---

## 十一、代码位置索引

| 功能 | 文件路径 | 行号 |
|------|---------|------|
| tlb_flush_batched 定义 | include/linux/mm_types.h | 876 |
| 位字段宏定义 | mm/rmap.c | 636-643 |
| set_tlb_ubc_flush_pending() | mm/rmap.c | 645-684 |
| flush_tlb_batched_pending() | mm/rmap.c | 713-728 |
| should_defer_flush() | mm/rmap.c | 690-696 |
| try_to_unmap_one() 使用 | mm/rmap.c | 1600-1614 |
| x86 arch_tlbbatch_add_pending() | arch/x86/include/asm/tlbflush.h | 280-287 |
| x86 arch_flush_tlb_batched_pending() | arch/x86/include/asm/tlbflush.h | 289-292 |
| ARM64 arch_tlbbatch_add_pending() | arch/arm64/include/asm/tlbflush.h | 300-305 |
| ARM64 arch_flush_tlb_batched_pending() | arch/arm64/include/asm/tlbflush.h | 312-315 |
| ARM64 arch_tlbbatch_flush() | arch/arm64/include/asm/tlbflush.h | 327-330 |
