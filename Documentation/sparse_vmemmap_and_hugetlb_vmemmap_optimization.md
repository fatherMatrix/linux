# Sparse VMEMMAP 与 HugeTLB VMEMMAP 优化分析

## 一、Sparse VMEMMAP 机制

### 1.1 概述

Sparse VMEMMAP 是一种虚拟内存映射机制，用于为每个物理页帧的 `struct page` 结构提供连续的虚拟地址空间。

**核心思想**：
- 通过虚拟地址空间的 `vmemmap` 区域映射所有 `struct page` 结构
- 提供 O(1) 时间复杂度的 `pfn_to_page()` 和 `page_to_pfn()` 转换
- 使用页表映射，可以利用大页（PMD/PUD 级别）减少 TLB miss

**位置**：`mm/sparse-vmemmap.c`

---

## 二、Sparse VMEMMAP 关键调用流程

### 2.1 内存初始化阶段

#### 流程图

```
系统启动
  ↓
sparse_init()                                    [mm/sparse.c]
  ↓
sparse_init_nid()                                [mm/sparse.c]
  ↓
__populate_section_memmap()                      [mm/sparse.c]
  ↓
sparse_buffer_alloc() / memmap_alloc()          [分配物理页]
  ↓
vmemmap_populate()                               [架构相关]
  ├─ x86: arch/x86/mm/init_64.c
  ├─ ARM64: arch/arm64/mm/mmu.c
  └─ ...
  ↓
vmemmap_populate_hugepages()                     [mm/sparse-vmemmap.c]
  ↓
vmemmap_pmd_populate() / vmemmap_pud_populate()
  ↓
vmemmap_alloc_block_buf()
  ↓
[建立页表映射]
  ├─ 优先使用大页（PMD/PUD）映射
  └─ 回退到基础页（4KB）映射
```

#### 关键函数详解

##### (1) vmemmap_populate()

**位置**：`arch/x86/mm/init_64.c:1535-1556`（x86 架构）

```c
int __meminit vmemmap_populate(unsigned long start, unsigned long end, int node,
		struct vmem_altmap *altmap)
{
	int err;

	VM_BUG_ON(!PAGE_ALIGNED(start));
	VM_BUG_ON(!PAGE_ALIGNED(end));

	/*
	 * 根据范围大小和 CPU 特性选择映射策略：
	 * 1. 小范围：使用基础页映射
	 * 2. 大范围 + PSE 支持：使用大页映射
	 * 3. 有 altmap：使用设备内存映射
	 */
	if (end - start < PAGES_PER_SECTION * sizeof(struct page))
		err = vmemmap_populate_basepages(start, end, node, NULL);
	else if (boot_cpu_has(X86_FEATURE_PSE))
		err = vmemmap_populate_hugepages(start, end, node, altmap);
	else if (altmap) {
		pr_err_once("%s: no cpu support for altmap allocations\n",
				__func__);
		err = -ENOMEM;
	} else
		err = vmemmap_populate_basepages(start, end, node, NULL);

	if (!err)
		sync_global_pgds(start, end - 1);
	return err;
}
```

**功能**：
- 为 vmemmap 虚拟地址范围 `[start, end)` 建立页表映射
- 选择映射策略（基础页 vs 大页）
- 同步全局页目录

##### (2) vmemmap_populate_hugepages()

**位置**：`mm/sparse-vmemmap.c:309-359`

```c
int __meminit vmemmap_populate_hugepages(unsigned long start, unsigned long end,
					 int node, struct vmem_altmap *altmap)
{
	unsigned long addr;
	unsigned long next;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;

	/*
	 * 遍历 vmemmap 地址范围，以 PMD 粒度建立映射
	 */
	for (addr = start; addr < end; addr = next) {
		next = pmd_addr_end(addr, end);

		/*
		 * 逐级填充页表：PGD → P4D → PUD → PMD
		 */
		pgd = vmemmap_pgd_populate(addr, node);
		if (!pgd)
			return -ENOMEM;

		p4d = vmemmap_p4d_populate(pgd, addr, node);
		if (!p4d)
			return -ENOMEM;

		pud = vmemmap_pud_populate(p4d, addr, node);
		if (!pud)
			return -ENOMEM;

		pmd = vmemmap_pmd_populate(pud, addr, node);
		if (!pmd)
			return -ENOMEM;

		/*
		 * 检查是否可以使用 PMD 大页映射
		 */
		if (vmemmap_check_pmd(pmd, node, addr, next))
			continue;

		/*
		 * 如果已经有映射，跳过
		 */
		if (pmd_none(*pmd)) {
			void *p;

			/*
			 * 分配物理页
			 */
			p = vmemmap_alloc_block_buf(PMD_SIZE, node, altmap);
			if (p) {
				/*
				 * 使用 PMD 大页映射
				 */
				vmemmap_set_pmd(pmd, p, node, addr, next);
				continue;
			} else if (altmap) {
				/*
				 * altmap 分配失败，返回错误
				 */
				return -ENOMEM; /* no fallback */
			}
		}

		/*
		 * 回退到基础页映射
		 */
		if (vmemmap_populate_basepages(addr, next, node, altmap))
			return -ENOMEM;
	}
	return 0;
}
```

**特点**：
- 优先尝试使用 2MB PMD 大页映射
- 如果无法使用大页，回退到 4KB 基础页映射
- 支持设备内存的特殊分配（altmap）

##### (3) vmemmap_alloc_block_buf()

**位置**：`mm/sparse-vmemmap.c:76-88`

```c
void * __meminit vmemmap_alloc_block_buf(unsigned long size, int node,
					 struct vmem_altmap *altmap)
{
	void *ptr;

	/*
	 * 如果有 altmap，使用设备内存分配
	 */
	if (altmap)
		return altmap_alloc_block_buf(size, altmap);

	/*
	 * 尝试从预分配的缓冲区分配
	 */
	ptr = sparse_buffer_alloc(size);
	if (!ptr)
		/*
		 * 缓冲区耗尽，从 buddy 或 memblock 分配
		 */
		ptr = vmemmap_alloc_block(size, node);
	return ptr;
}
```

### 2.2 热插拔内存阶段

#### 流程图

```
热插拔内存添加
  ↓
sparse_add_section()                             [mm/sparse.c]
  ↓
section_activate()                               [mm/sparse.c]
  ↓
populate_section_memmap()                        [mm/sparse.c]
  ↓
__populate_section_memmap()
  ↓
vmemmap_populate()                               [架构相关]
  ↓
vmemmap_populate_hugepages() / vmemmap_populate_basepages()
  ↓
[建立页表映射]
```

**关键点**：
- 热插拔内存使用运行时分配器（buddy 系统）
- 不再使用 memblock 或预分配缓冲区
- 映射策略与初始化阶段相同

### 2.3 内存移除阶段

#### 流程图

```
热插拔内存移除
  ↓
sparse_remove_section()                          [mm/sparse.c]
  ↓
section_deactivate()                             [mm/sparse.c]
  ↓
depopulate_section_memmap()                      [mm/sparse.c]
  ↓
vmemmap_free()                                   [架构相关]
  ↓
remove_pte_table() / remove_pmd_table()
  ↓
[释放页表和物理页]
```

---

## 三、HugeTLB VMEMMAP 优化机制

### 3.1 优化原理

**问题**：
- 一个 2MB HugeTLB 页需要 512 个 `struct page`（每个 4KB 基础页一个）
- 这些 `struct page` 占用 8 个页帧（512 × 64 字节 / 4096 字节）
- 但实际上只有前 4 个 `struct page` 包含有用信息
- 其余的 `struct page` 只是为了设置 `compound_head` 指针

**优化方案**：
- 保留第 1 个页帧（包含前 4 个有用的 `struct page`）
- 将第 2-8 个页帧的虚拟地址重映射到第 1 个页帧
- 释放第 2-8 个页帧到 buddy 系统
- **节省空间**：每个 2MB HugeTLB 页节省 7 个页帧（28KB）

**位置**：`mm/hugetlb_vmemmap.c`

**文档**：`Documentation/mm/vmemmap_dedup.rst`

### 3.2 优化前后对比

#### 优化前

```
HugeTLB (2MB)        struct page (8 pages)      物理页帧 (8 pages)
+-----------+        +-----------+               +-----------+
|           |        |     0     | ------------> |     0     |
|           |        +-----------+               +-----------+
|           |        |     1     | ------------> |     1     |
|   PMD     |        +-----------+               +-----------+
|  level    |        |     2     | ------------> |     2     |
| mapping   |        +-----------+               +-----------+
|           |        |     3     | ------------> |     3     |
|           |        +-----------+               +-----------+
|           |        |     4     | ------------> |     4     |
|           |        +-----------+               +-----------+
|           |        |     5     | ------------> |     5     |
|           |        +-----------+               +-----------+
|           |        |     6     | ------------> |     6     |
|           |        +-----------+               +-----------+
|           |        |     7     | ------------> |     7     |
+-----------+        +-----------+               +-----------+
```

#### 优化后

```
HugeTLB (2MB)        struct page (8 pages)      物理页帧 (8 pages)
+-----------+        +-----------+               +-----------+
|           |        |     0     | ------------> |     0     |
|           |        +-----------+               +-----------+
|           |        |     1     | --------------^ (重映射)
|   PMD     |        +-----------+
|  level    |        |     2     | --------------^ (重映射)
| mapping   |        +-----------+
|           |        |     3     | --------------^ (重映射)
|           |        +-----------+
|           |        |     4     | --------------^ (重映射)
|           |        +-----------+               [页帧 1-7]
|           |        |     5     | --------------^ (重映射) [已释放]
|           |        +-----------+               [到 buddy]
|           |        |     6     | --------------^ (重映射)
|           |        +-----------+
|           |        |     7     | --------------^ (重映射)
+-----------+        +-----------+
```

**关键**：
- 所有 `struct page` 的虚拟地址都映射到物理页帧 0
- 物理页帧 1-7 被释放，可供其他用途使用

### 3.3 HugeTLB VMEMMAP 优化调用流程

#### 3.3.1 优化流程（分配 HugeTLB 时）

```
分配 HugeTLB 页
  ↓
alloc_fresh_huge_page()                          [mm/hugetlb.c]
  ↓
prep_new_huge_page()                             [mm/hugetlb.c]
  ↓
hugetlb_vmemmap_optimize()                       [mm/hugetlb_vmemmap.c]
  ↓
vmemmap_remap_free()                             [mm/hugetlb_vmemmap.c]
  ↓
[关键操作]
  ├─ 1. 分配新的 reuse_page（用于替换原 page 0）
  ├─ 2. 拷贝 page 0 内容到 reuse_page
  ├─ 3. 遍历 page 1-7 的 PTE
  ├─ 4. 重映射 PTE 到 reuse_page
  └─ 5. 释放原 page 0-7 到 buddy 系统
  ↓
SetHPageVmemmapOptimized(head)                   [设置优化标志]
```

#### 3.3.2 关键函数：vmemmap_remap_free()

**位置**：`mm/hugetlb_vmemmap.c:311-370`

```c
/*
 * vmemmap_remap_free - 重映射 vmemmap 虚拟地址范围 [@start, @end)
 *                      到 @reuse 映射的页面，然后释放原映射的页面
 * @start:  要重映射的 vmemmap 虚拟地址范围的起始地址
 * @end:    要重映射的 vmemmap 虚拟地址范围的结束地址
 * @reuse:  重用的地址（通常是 struct page 数组的第一个页面）
 *
 * Return: 成功返回 %0，否则返回负错误码
 */
static int vmemmap_remap_free(unsigned long start, unsigned long end,
			      unsigned long reuse)
{
	int ret;
	LIST_HEAD(vmemmap_pages);
	struct vmemmap_remap_walk walk = {
		.remap_pte	= vmemmap_remap_pte,      /* PTE 重映射回调 */
		.reuse_addr	= reuse,                  /* 重用地址 */
		.vmemmap_pages	= &vmemmap_pages,         /* 待释放页面链表 */
	};
	int nid = page_to_nid((struct page *)start);
	gfp_t gfp_mask = GFP_KERNEL | __GFP_THISNODE | __GFP_NORETRY |
			__GFP_NOWARN;

	/*
	 * 分配一个新的 head vmemmap 页面，避免破坏连续的 struct page 内存块。
	 * 当释放回页分配器时，这将保持可能连续的 struct page 后备内存的连续性，
	 * 允许更多的 hugepage 分配。如果分配失败，则回退到当前映射的 head 页。
	 */
	walk.reuse_page = alloc_pages_node(nid, gfp_mask, 0);
	if (walk.reuse_page) {
		/*
		 * 拷贝原 page 0 的内容到新页面
		 */
		copy_page(page_to_virt(walk.reuse_page),
			  (void *)walk.reuse_addr);
		list_add(&walk.reuse_page->lru, &vmemmap_pages);
	}

	/*
	 * 为了使重映射例程对大页最有效，我们需要确保
	 * start 和 reuse 地址之间的距离是 PAGE_SIZE。
	 */
	BUG_ON(start - reuse != PAGE_SIZE);

	/*
	 * 如果 PMD 映射的 vmemmap 页面使用大页，则将其拆分为基础页
	 */
	vmemmap_remap_split(reuse, end);

	/*
	 * 锁定 init_mm，保护页表操作
	 */
	mmap_read_lock(&init_mm);
	/*
	 * 遍历页表，重映射 [start, end) 范围的 PTE
	 */
	ret = vmemmap_remap_range(reuse, end, &walk);
	mmap_read_unlock(&init_mm);

	if (ret)
		return ret;

	/*
	 * 释放原来映射的页面到 buddy 系统
	 */
	free_vmemmap_page_list(&vmemmap_pages);

	return 0;
}
```

#### 3.3.3 核心操作：vmemmap_remap_pte()

**位置**：`mm/hugetlb_vmemmap.c:232-255`

```c
/*
 * 重映射单个 PTE 的回调函数
 */
static void vmemmap_remap_pte(pte_t *pte, unsigned long addr,
			      struct vmemmap_remap_walk *walk)
{
	/*
	 * 记录已遍历的 PTE 数量
	 */
	walk->nr_walked++;

	/*
	 * 获取当前 PTE 映射的物理页
	 */
	struct page *page = pte_page(ptep_get(pte));

	/*
	 * 第一个 PTE 对应 reuse_page（保留页）
	 */
	if (likely(walk->nr_walked == 1)) {
		walk->reuse_page = page;
		return;
	}

	/*
	 * 后续的 PTE 需要重映射到 reuse_page
	 */
	if (unlikely(PageHWPoison(page))) {
		/*
		 * 如果页面有硬件毒药标记，清除标记
		 * （因为页面即将被释放）
		 */
		ClearPageHWPoison(page);
	}

	if (PageHead(page)) {
		/*
		 * 使用内存屏障确保 vmemmap_remap_free() 中的写操作
		 * 在 set_pte_at() 之前可见
		 */
		smp_wmb();
	}

	/*
	 * 创建新的 PTE，映射到 reuse_page
	 */
	entry = mk_pte(walk->reuse_page, pgprot);

	/*
	 * 将原页面加入待释放链表
	 */
	list_add_tail(&page->lru, walk->vmemmap_pages);

	/*
	 * 更新 PTE
	 */
	set_pte_at(&init_mm, addr, pte, entry);
}
```

**关键点**：
1. 第一个 PTE（page 0）保留为 `reuse_page`
2. 后续 PTE（page 1-7）重映射到 `reuse_page`
3. 原页面加入释放链表
4. 使用内存屏障保证可见性

#### 3.3.4 恢复流程（释放 HugeTLB 时）

```
释放 HugeTLB 页到 buddy
  ↓
update_and_free_page()                           [mm/hugetlb.c]
  ↓
hugetlb_vmemmap_restore()                        [mm/hugetlb_vmemmap.c]
  ↓
vmemmap_remap_alloc()                            [mm/hugetlb_vmemmap.c]
  ↓
[关键操作]
  ├─ 1. 分配 7 个新页面（alloc_vmemmap_page_list）
  ├─ 2. 遍历 page 1-7 的 PTE
  ├─ 3. 恢复 PTE 映射到各自的新页面
  └─ 4. 重置 struct page 内容
  ↓
ClearHPageVmemmapOptimized(head)                 [清除优化标志]
```

#### 3.3.5 关键函数：vmemmap_remap_alloc()

**位置**：`mm/hugetlb_vmemmap.c:414-435`

```c
/*
 * vmemmap_remap_alloc - 重映射 vmemmap 虚拟地址范围 [@start, end)
 *                       到从 @vmemmap_pages 分配的页面
 * @start:  要重映射的 vmemmap 虚拟地址范围的起始地址
 * @end:    要重映射的 vmemmap 虚拟地址范围的结束地址
 * @reuse:  重用的地址
 *
 * Return: 成功返回 %0，否则返回负错误码
 */
static int vmemmap_remap_alloc(unsigned long start, unsigned long end,
			       unsigned long reuse)
{
	LIST_HEAD(vmemmap_pages);
	struct vmemmap_remap_walk walk = {
		.remap_pte	= vmemmap_restore_pte,    /* 恢复 PTE 回调 */
		.reuse_addr	= reuse,
		.vmemmap_pages	= &vmemmap_pages,
	};

	/*
	 * start 和 reuse 之间必须相差 PAGE_SIZE
	 */
	BUG_ON(start - reuse != PAGE_SIZE);

	/*
	 * 分配需要的页面
	 */
	if (alloc_vmemmap_page_list(start, end, &vmemmap_pages))
		return -ENOMEM;

	/*
	 * 遍历页表，恢复 PTE 映射
	 */
	mmap_read_lock(&init_mm);
	vmemmap_remap_range(reuse, end, &walk);
	mmap_read_unlock(&init_mm);

	return 0;
}
```

#### 3.3.6 核心操作：vmemmap_restore_pte()

**位置**：`mm/hugetlb_vmemmap.c:276-293`

```c
/*
 * 恢复单个 PTE 的回调函数
 */
static void vmemmap_restore_pte(pte_t *pte, unsigned long addr,
				struct vmemmap_remap_walk *walk)
{
	pte_t entry;
	struct page *page;

	/*
	 * 从链表中获取一个新分配的页面
	 */
	page = list_first_entry(walk->vmemmap_pages, struct page, lru);
	list_del(&page->lru);

	/*
	 * 创建新的 PTE，映射到新分配的页面
	 */
	entry = mk_pte(page, pgprot);
	set_pte_at(&init_mm, addr, pte, entry);

	/*
	 * 重置 struct page 内容
	 * 从 reuse_page 拷贝前 3 个 struct page 的内容
	 */
	reset_struct_pages((struct page *)addr);
}
```

**关键点**：
1. 为每个 PTE 分配新的物理页
2. 恢复 PTE 映射
3. 重置 `struct page` 内容（拷贝 head 页的元数据）

### 3.4 配置和控制

#### 编译时配置

```kconfig
CONFIG_HUGETLB_PAGE_OPTIMIZE_VMEMMAP=y
CONFIG_HUGETLB_PAGE_OPTIMIZE_VMEMMAP_DEFAULT_ON=y
```

#### 运行时控制

```bash
# 启用优化（默认）
echo 1 > /proc/sys/vm/hugetlb_optimize_vmemmap

# 禁用优化
echo 0 > /proc/sys/vm/hugetlb_optimize_vmemmap

# 内核启动参数
hugetlb_free_vmemmap=on   # 启用
hugetlb_free_vmemmap=off  # 禁用
```

#### 检查优化状态

```c
/*
 * 检查 HugeTLB 页是否已优化
 */
if (HPageVmemmapOptimized(head)) {
	/* 已优化 */
} else {
	/* 未优化 */
}
```

---

## 四、Transparent Huge Page (THP) 的情况

### 4.1 THP 没有类似的 VMEMMAP 优化

经过代码审查，**Transparent Huge Page (THP) 没有实现类似 HugeTLB 的 vmemmap 优化**。

**原因**：

#### (1) 设计目标不同

- **HugeTLB**：
  - 长期存在的大页
  - 预分配和保留
  - 适合优化内存开销

- **THP**：
  - 动态分配和回收
  - 可以随时拆分（split）为基础页
  - 生命周期短且不可预测

#### (2) 拆分复杂性

THP 可以随时被拆分为 512 个基础页（2MB THP）：

```c
/* mm/huge_memory.c */
void __split_huge_page(...)
{
	/*
	 * 拆分 THP 为 512 个基础页
	 * 每个基础页需要独立的 struct page
	 */
	for (i = 0; i < nr; i++) {
		struct page *subpage = head + i;
		/* 初始化每个基础页的 struct page */
	}
}
```

如果 THP 实现了 vmemmap 优化：
- 拆分时需要重新分配并映射 7 个页帧
- 增加拆分的复杂度和开销
- 可能导致内存碎片

#### (3) 性能考量

- **HugeTLB**：
  - 优化一次，长期受益
  - 分配和释放不频繁

- **THP**：
  - 频繁的分配、拆分、合并
  - 优化和恢复的开销可能抵消收益

### 4.2 THP 的内存管理方式

THP 的 `struct page` 管理采用标准方式：

```
THP (2MB)              struct page (512 个)         物理页帧 (8 个)
+-----------+          +-----------+                +-----------+
|           |          |     0     | (head)         |     0     |
|           |          +-----------+                +-----------+
|           |          |     1     | (tail)         |     1     |
|           |          +-----------+                +-----------+
|   PMD     |          |     2     | (tail)         |     2     |
|  level    |          +-----------+                +-----------+
| mapping   |          |    ...    |                |    ...    |
|           |          +-----------+                +-----------+
|           |          |    511    | (tail)         |     7     |
+-----------+          +-----------+                +-----------+
```

**特点**：
- 每个基础页都有独立的 `struct page`
- 所有 `struct page` 正常映射到各自的物理页帧
- 没有重映射和优化

### 4.3 未来可能性

虽然当前 THP 没有 vmemmap 优化，但理论上可以实现：

**可能的优化方向**：
1. **仅对长期存在的 THP 优化**
   - 跟踪 THP 的生命周期
   - 仅对超过一定时间阈值的 THP 应用优化

2. **延迟优化**
   - 分配时不优化
   - 当 THP 稳定后（例如 1 秒后）再优化

3. **拆分时的快速路径**
   - 保留预分配的页帧池
   - 拆分时快速恢复映射

**挑战**：
- 复杂的状态管理
- 拆分和优化的竞争条件
- 性能权衡

---

## 五、关键数据结构

### 5.1 vmemmap_remap_walk

**位置**：`mm/hugetlb_vmemmap.c:29-36`

```c
/*
 * vmemmap 页表遍历结构
 */
struct vmemmap_remap_walk {
	/* PTE 重映射回调函数 */
	void (*remap_pte)(pte_t *pte, unsigned long addr,
			  struct vmemmap_remap_walk *walk);

	/* 已遍历的 PTE 数量 */
	unsigned long nr_walked;

	/* 重用的页面（通常是 page 0） */
	struct page *reuse_page;

	/* 重用页面的虚拟地址 */
	unsigned long reuse_addr;

	/* 待释放或待映射的 vmemmap 页面链表 */
	struct list_head *vmemmap_pages;
};
```

### 5.2 vmem_altmap

**位置**：`include/linux/memremap.h`

```c
/*
 * 设备内存的替代映射结构
 * 用于 Device DAX 等场景
 */
struct vmem_altmap {
	/* 设备内存的起始 PFN */
	unsigned long base_pfn;

	/* 保留的页面数量（用于 struct page 自身） */
	unsigned long reserve;

	/* 释放的页面数量 */
	unsigned long free;

	/* 对齐的页面数量 */
	unsigned long align;

	/* 已分配的页面数量 */
	unsigned long alloc;
};
```

---

## 六、性能和收益分析

### 6.1 HugeTLB VMEMMAP 优化收益

#### 内存节省

| HugeTLB 大小 | struct page 总数 | 优化前占用 | 优化后占用 | 节省空间 |
|-------------|----------------|-----------|-----------|---------|
| 2MB (x86-64) | 512 | 8 页 (32KB) | 1 页 (4KB) | 7 页 (28KB) |
| 1GB (x86-64) | 262144 | 4096 页 (16MB) | 512 页 (2MB) | 3584 页 (14MB) |

#### 实际效果

假设系统有 1000 个 2MB HugeTLB 页：
- 优化前：8000 个页帧用于 `struct page`（31.25 MB）
- 优化后：1000 个页帧用于 `struct page`（3.90 MB）
- **节省**：7000 个页帧（27.34 MB）

假设系统有 100 个 1GB HugeTLB 页：
- 优化前：409600 个页帧用于 `struct page`（1600 MB）
- 优化后：51200 个页帧用于 `struct page`（200 MB）
- **节省**：358400 个页帧（1400 MB）

### 6.2 性能开销

#### 优化开销（分配 HugeTLB 时）

```
操作                          开销
----------------------------------------
分配 reuse_page                O(1)
拷贝 page 0                    O(1)
拆分 PMD 大页（如果需要）       O(512)
重映射 7 个 PTE                O(7)
释放 7 个页面                  O(7)
----------------------------------------
总计                           约 O(1) - O(512)
```

#### 恢复开销（释放 HugeTLB 时）

```
操作                          开销
----------------------------------------
分配 7 个新页面                O(7)
重映射 7 个 PTE                O(7)
重置 struct page               O(7)
----------------------------------------
总计                           O(7)
```

#### TLB 刷新开销

- 重映射时需要刷新 TLB
- 使用 `flush_tlb_kernel_range()` 刷新受影响的地址范围
- 通常影响较小（仅影响 vmemmap 区域的 TLB）

### 6.3 权衡

**适用场景**：
- ✅ 大量长期存在的 HugeTLB 页
- ✅ 内存受限的系统
- ✅ 内存开销敏感的应用

**不适用场景**：
- ❌ 频繁分配和释放 HugeTLB 页
- ❌ HugeTLB 页数量很少（节省不明显）

---

## 七、总结

### 7.1 Sparse VMEMMAP

**核心机制**：
- 为所有 `struct page` 提供连续的虚拟地址空间
- 使用页表映射，支持大页优化
- O(1) 的 `pfn_to_page()` 和 `page_to_pfn()` 转换

**关键流程**：
1. **初始化**：`sparse_init()` → `vmemmap_populate()` → 建立页表映射
2. **热插拔**：`sparse_add_section()` → `vmemmap_populate()` → 扩展映射
3. **移除**：`sparse_remove_section()` → `vmemmap_free()` → 释放映射

### 7.2 HugeTLB VMEMMAP 优化

**核心思想**：
- 利用 HugeTLB 页的 `struct page` 冗余性
- 重映射 tail pages 到 head page
- 释放冗余的物理页帧

**关键流程**：
1. **优化**：`hugetlb_vmemmap_optimize()` → `vmemmap_remap_free()` → 重映射并释放
2. **恢复**：`hugetlb_vmemmap_restore()` → `vmemmap_remap_alloc()` → 分配并恢复映射

**收益**：
- 每个 2MB HugeTLB 节省 28KB
- 每个 1GB HugeTLB 节省 14MB
- 适合大量长期 HugeTLB 的场景

### 7.3 THP 的情况

**现状**：
- THP 没有类似的 vmemmap 优化
- 采用标准的 `struct page` 管理方式

**原因**：
- 动态性强（频繁拆分、合并）
- 优化和恢复开销可能抵消收益
- 实现复杂度高

**未来**：
- 理论上可以实现，但需要解决拆分和状态管理的挑战

---

## 八、代码位置索引

| 功能 | 文件路径 | 关键函数 |
|------|---------|---------|
| Sparse VMEMMAP 核心 | mm/sparse-vmemmap.c | vmemmap_populate() |
| Sparse VMEMMAP 大页 | mm/sparse-vmemmap.c | vmemmap_populate_hugepages() |
| Sparse VMEMMAP 基础页 | mm/sparse-vmemmap.c | vmemmap_populate_basepages() |
| Sparse VMEMMAP 分配 | mm/sparse-vmemmap.c | vmemmap_alloc_block_buf() |
| x86 VMEMMAP 实现 | arch/x86/mm/init_64.c | vmemmap_populate() |
| ARM64 VMEMMAP 实现 | arch/arm64/mm/mmu.c | vmemmap_populate() |
| HugeTLB 优化核心 | mm/hugetlb_vmemmap.c | hugetlb_vmemmap_optimize() |
| HugeTLB 优化恢复 | mm/hugetlb_vmemmap.c | hugetlb_vmemmap_restore() |
| VMEMMAP 重映射释放 | mm/hugetlb_vmemmap.c | vmemmap_remap_free() |
| VMEMMAP 重映射分配 | mm/hugetlb_vmemmap.c | vmemmap_remap_alloc() |
| PTE 重映射回调 | mm/hugetlb_vmemmap.c | vmemmap_remap_pte() |
| PTE 恢复回调 | mm/hugetlb_vmemmap.c | vmemmap_restore_pte() |
| HugeTLB 主逻辑 | mm/hugetlb.c | alloc_fresh_huge_page() |
| THP 拆分 | mm/huge_memory.c | __split_huge_page() |
| 文档 | Documentation/mm/vmemmap_dedup.rst | - |
