# depopulate_section_memmap 与 free_map_bootmem 对比分析

## 一、概述

`depopulate_section_memmap` 和 `free_map_bootmem` 都是用于释放内存段（memory section）的 memmap（struct page 数组）的函数，但它们针对**不同来源**的 memmap：

- **depopulate_section_memmap**：释放**运行时动态分配**的 memmap（热插拔内存）
- **free_map_bootmem**：释放**启动时分配**的 memmap（early section）

---

## 二、函数定义对比

### 2.1 CONFIG_SPARSEMEM_VMEMMAP=y 场景

#### (1) depopulate_section_memmap

**位置**：`mm/sparse.c:638-645`

```c
static void depopulate_section_memmap(unsigned long pfn, unsigned long nr_pages,
		struct vmem_altmap *altmap)
{
	unsigned long start = (unsigned long) pfn_to_page(pfn);
	unsigned long end = start + nr_pages * sizeof(struct page);

	vmemmap_free(start, end, altmap);
}
```

**特点**：
- 接受 `altmap` 参数（用于设备内存的特殊分配器）
- 接受 `nr_pages` 参数（支持部分段释放，subsection）
- 通过 `pfn_to_page(pfn)` 计算起始地址
- 调用 `vmemmap_free()` 释放 vmemmap 区域

#### (2) free_map_bootmem

**位置**：`mm/sparse.c:646-652`

```c
static void free_map_bootmem(struct page *memmap)
{
	unsigned long start = (unsigned long)memmap;
	unsigned long end = (unsigned long)(memmap + PAGES_PER_SECTION);

	vmemmap_free(start, end, NULL);
}
```

**特点**：
- 接受 `memmap` 指针（直接指向 memmap 数组）
- **不接受** `altmap` 参数（始终为 NULL）
- 固定释放整个段（`PAGES_PER_SECTION`）
- 调用相同的 `vmemmap_free()` 函数

### 2.2 CONFIG_SPARSEMEM_VMEMMAP=n 场景

#### (1) depopulate_section_memmap

**位置**：`mm/sparse.c:711-715`

```c
static void depopulate_section_memmap(unsigned long pfn, unsigned long nr_pages,
		struct vmem_altmap *altmap)
{
	kvfree(pfn_to_page(pfn));
}
```

**特点**：
- 简单地调用 `kvfree()` 释放内存
- memmap 是通过 `kvmalloc_node()` 分配的

#### (2) free_map_bootmem

**位置**：`mm/sparse.c:717-745`

```c
static void free_map_bootmem(struct page *memmap)
{
	unsigned long maps_section_nr, removing_section_nr, i;
	unsigned long magic, nr_pages;
	struct page *page = virt_to_page(memmap);

	nr_pages = PAGE_ALIGN(PAGES_PER_SECTION * sizeof(struct page))
		>> PAGE_SHIFT;

	for (i = 0; i < nr_pages; i++, page++) {
		magic = page->index;

		BUG_ON(magic == NODE_INFO);

		maps_section_nr = pfn_to_section_nr(page_to_pfn(page));
		removing_section_nr = page_private(page);

		/*
		 * When this function is called, the removing section is
		 * logical offlined state. This means all pages are isolated
		 * from page allocator. If removing section's memmap is placed
		 * on the same section, it must not be freed.
		 * If it is freed, page allocator may allocate it which will
		 * be removed physically soon.
		 */
		if (maps_section_nr != removing_section_nr)
			put_page_bootmem(page);
	}
}
```

**特点**：
- 复杂的逻辑，逐页检查
- 检查 memmap 自身所在的段（`maps_section_nr`）
- 如果 memmap 和要移除的段是同一个段，**不释放**（避免页分配器重新分配）
- 否则调用 `put_page_bootmem()` 释放

---

## 三、调用场景分析

### 3.1 section_deactivate 函数

**位置**：`mm/sparse.c:779-825`

```c
static void section_deactivate(unsigned long pfn, unsigned long nr_pages,
		struct vmem_altmap *altmap)
{
	struct mem_section *ms = __pfn_to_section(pfn);
	bool section_is_early = early_section(ms);
	struct page *memmap = NULL;
	bool empty;

	if (clear_subsection_map(pfn, nr_pages))
		return;

	empty = is_subsection_map_empty(ms);
	if (empty) {
		unsigned long section_nr = pfn_to_section_nr(pfn);

		/*
		 * When removing an early section, the usage map is kept (as the
		 * usage maps of other sections fall into the same page). It
		 * will be re-used when re-adding the section - which is then no
		 * longer an early section. If the usage map is PageReserved, it
		 * was allocated during boot.
		 */
		if (!PageReserved(virt_to_page(ms->usage))) {
			kfree(ms->usage);
			ms->usage = NULL;
		}
		memmap = sparse_decode_mem_map(ms->section_mem_map, section_nr);
		/*
		 * Mark the section invalid so that valid_section()
		 * return false. This prevents code from dereferencing
		 * ms->usage array.
		 */
		ms->section_mem_map &= ~SECTION_HAS_MEM_MAP;
	}

	/*
	 * The memmap of early sections is always fully populated. See
	 * section_activate() and pfn_valid() .
	 */
	if (!section_is_early)
		depopulate_section_memmap(pfn, nr_pages, altmap);
	else if (memmap)
		free_map_bootmem(memmap);

	if (empty)
		ms->section_mem_map = (unsigned long)NULL;
}
```

#### 关键判断逻辑

```c
if (!section_is_early)
	depopulate_section_memmap(pfn, nr_pages, altmap);
else if (memmap)
	free_map_bootmem(memmap);
```

**决策依据**：
- `section_is_early`：检查段是否标记为 `SECTION_IS_EARLY`
- **早期段（early section）**：使用 `free_map_bootmem()`
- **非早期段（non-early section）**：使用 `depopulate_section_memmap()`

### 3.2 early_section() 判断

**位置**：`include/linux/mmzone.h:1910-1913`

```c
static inline int early_section(struct mem_section *section)
{
	return (section && (section->section_mem_map & SECTION_IS_EARLY));
}
```

**SECTION_IS_EARLY 标志**：
- 在内核启动时设置（`sparse_init_nid()` 函数）
- 表示该段的 memmap 是通过启动时分配器（memblock）分配的

---

## 四、核心区别总结

### 4.1 功能区别

| 特性 | depopulate_section_memmap | free_map_bootmem |
|------|--------------------------|------------------|
| **适用场景** | 热插拔内存移除（运行时分配） | 早期内存段移除（启动时分配） |
| **memmap 来源** | `populate_section_memmap()` 动态分配 | 启动时 `__populate_section_memmap()` 分配 |
| **分配方式** | VMEMMAP: vmemmap 映射<br>非 VMEMMAP: kvmalloc | VMEMMAP: vmemmap 映射<br>非 VMEMMAP: memblock 分配 |
| **支持部分释放** | ✅ 支持（subsection，通过 `nr_pages`） | ❌ 不支持（固定 `PAGES_PER_SECTION`） |
| **altmap 支持** | ✅ 支持（设备内存特殊分配） | ❌ 不支持（始终为 NULL） |
| **复杂度** | 简单，直接释放 | 复杂（非 VMEMMAP 时需检查自引用） |

### 4.2 调用时机区别

#### depopulate_section_memmap
```
场景：热插拔内存移除
调用路径：
  memory_block_offline()
    → offline_pages()
      → __offline_pages()
        → sparse_remove_section()
          → section_deactivate()
            → depopulate_section_memmap()  (if !section_is_early)
```

#### free_map_bootmem
```
场景：移除早期内存段（罕见，主要用于测试或特殊配置）
调用路径：
  sparse_remove_section()
    → section_deactivate()
      → free_map_bootmem()  (if section_is_early && memmap)
```

### 4.3 设计哲学区别

#### depopulate_section_memmap
- **目标**：释放运行时分配的资源
- **假设**：memmap 是独立分配的，不会与其他段共享
- **操作**：直接释放，简单高效

#### free_map_bootmem
- **目标**：释放启动时分配的资源
- **假设**：memmap 可能与其他段共享（特别是在非 VMEMMAP 配置下）
- **操作**：谨慎释放，避免破坏自引用（self-reference）

---

## 五、深入分析：自引用问题

### 5.1 什么是自引用？

**位置**：`mm/sparse.c:733-741`

```c
/*
 * When this function is called, the removing section is
 * logical offlined state. This means all pages are isolated
 * from page allocator. If removing section's memmap is placed
 * on the same section, it must not be freed.
 * If it is freed, page allocator may allocate it which will
 * be removed physically soon.
 */
if (maps_section_nr != removing_section_nr)
	put_page_bootmem(page);
```

#### 问题场景

```
假设：
  - Section A 的物理地址范围：[0x8000_0000, 0x8800_0000)（128MB）
  - Section A 的 memmap 数组需要空间：128MB / 4KB * sizeof(struct page) = 512KB
  - Section A 的 memmap 恰好存储在 Section A 自己的物理内存中

当移除 Section A 时：
  1. Section A 进入 offline 状态
  2. 调用 free_map_bootmem() 释放 memmap
  3. 如果释放了 Section A 自己的 memmap：
     - 这部分内存可能被页分配器重新分配
     - 但 Section A 即将被物理移除
     - 导致正在使用的内存突然消失！
```

#### 解决方案

- 检查 `maps_section_nr`（memmap 所在的段）
- 检查 `removing_section_nr`（正在移除的段）
- 如果两者相同：**不释放**，避免自引用问题
- 如果两者不同：安全释放

### 5.2 为什么 depopulate_section_memmap 不需要检查？

#### 原因 1：热插拔内存的 memmap 独立分配

```c
/* mm/sparse.c:703-709 - 非 VMEMMAP 配置 */
static struct page * __meminit populate_section_memmap(unsigned long pfn,
		unsigned long nr_pages, int nid, struct vmem_altmap *altmap,
		struct dev_pagemap *pgmap)
{
	return kvmalloc_node(array_size(sizeof(struct page),
					PAGES_PER_SECTION), GFP_KERNEL, nid);
}
```

- memmap 通过 `kvmalloc_node()` 动态分配
- 不会分配在被管理的物理内存段中
- 因此不存在自引用问题

#### 原因 2：VMEMMAP 配置下的虚拟映射

```c
/* mm/sparse.c:631-636 - VMEMMAP 配置 */
static struct page * __meminit populate_section_memmap(unsigned long pfn,
		unsigned long nr_pages, int nid, struct vmem_altmap *altmap,
		struct dev_pagemap *pgmap)
{
	return __populate_section_memmap(pfn, nr_pages, nid, altmap, pgmap);
}
```

- memmap 通过 vmemmap 虚拟地址空间映射
- 虚拟地址和物理地址分离
- 释放虚拟映射不会影响物理内存的可用性

---

## 六、代码路径对比

### 6.1 分配路径对比

#### depopulate_section_memmap 对应的分配
```
热插拔内存添加：
  sparse_add_section()
    → section_activate()
      → populate_section_memmap()
        ├─ VMEMMAP: __populate_section_memmap() + vmemmap 映射
        └─ 非 VMEMMAP: kvmalloc_node()
```

#### free_map_bootmem 对应的分配
```
内核启动初始化：
  sparse_init()
    → sparse_init_nid()
      → __populate_section_memmap()
        ├─ VMEMMAP: memblock_alloc() + vmemmap 映射
        └─ 非 VMEMMAP: memblock_alloc()
      → sparse_init_one_section(..., SECTION_IS_EARLY)  ← 设置 SECTION_IS_EARLY 标志
```

### 6.2 释放路径对比

#### depopulate_section_memmap
```
VMEMMAP 配置：
  vmemmap_free(start, end, altmap)
    → remove_pte_table() / remove_pmd_table()
      → 释放页表和物理页

非 VMEMMAP 配置：
  kvfree(pfn_to_page(pfn))
    → vfree() 或 kfree()
      → 释放虚拟内存和物理页
```

#### free_map_bootmem
```
VMEMMAP 配置：
  vmemmap_free(start, end, NULL)
    → 同上

非 VMEMMAP 配置：
  put_page_bootmem(page)
    → free_reserved_page(page)
      → 减少引用计数
      → 释放页到伙伴系统（如果引用计数为 0）
```

---

## 七、Subsection 支持差异

### 7.1 depopulate_section_memmap 支持 Subsection

```c
static void depopulate_section_memmap(unsigned long pfn, unsigned long nr_pages,
		struct vmem_altmap *altmap)
{
	unsigned long start = (unsigned long) pfn_to_page(pfn);
	unsigned long end = start + nr_pages * sizeof(struct page);  ← 支持部分段

	vmemmap_free(start, end, altmap);
}
```

**用途**：
- **Subsection**：一个 section 的子部分（CONFIG_SPARSEMEM_VMEMMAP 配置）
- 允许热插拔小于整个 section 的内存块
- `nr_pages` 可以小于 `PAGES_PER_SECTION`

### 7.2 free_map_bootmem 不支持 Subsection

```c
static void free_map_bootmem(struct page *memmap)
{
	unsigned long start = (unsigned long)memmap;
	unsigned long end = (unsigned long)(memmap + PAGES_PER_SECTION);  ← 固定整个段

	vmemmap_free(start, end, NULL);
}
```

**原因**：
- 早期段的 memmap 总是**完全填充**（fully populated）
- 启动时不支持部分段的概念
- 必须以完整的 section 为单位

---

## 八、VMEMMAP vs 非 VMEMMAP 对比

### 8.1 CONFIG_SPARSEMEM_VMEMMAP=y（推荐配置）

#### 特点
- memmap 通过虚拟地址空间 `vmemmap` 区域映射
- 提供连续的虚拟地址空间
- 支持大页映射（PMD/PUD level），减少 TLB miss

#### depopulate_section_memmap
```c
vmemmap_free(start, end, altmap);
```
- 释放 vmemmap 虚拟地址区域的页表映射
- 释放对应的物理页

#### free_map_bootmem
```c
vmemmap_free(start, end, NULL);
```
- 同上，但不支持 altmap

### 8.2 CONFIG_SPARSEMEM_VMEMMAP=n（传统配置）

#### 特点
- memmap 通过 `kvmalloc` 或 `memblock_alloc` 直接分配
- 每个 section 的 memmap 是独立的虚拟内存块
- 可能不连续

#### depopulate_section_memmap
```c
kvfree(pfn_to_page(pfn));
```
- 简单地释放通过 `kvmalloc` 分配的内存

#### free_map_bootmem
```c
for (i = 0; i < nr_pages; i++, page++) {
	if (maps_section_nr != removing_section_nr)
		put_page_bootmem(page);
}
```
- 逐页检查并释放
- 避免自引用问题

---

## 九、实际使用示例

### 9.1 热插拔内存移除（使用 depopulate_section_memmap）

```bash
# 查看内存块
ls /sys/devices/system/memory/

# 下线内存块
echo offline > /sys/devices/system/memory/memory32/state

# 内核调用链
memory_subsys_offline()
  → memory_block_change_state()
    → memory_block_offline()
      → offline_pages()
        → __offline_pages()
          → walk_system_ram_range()  # 隔离页面
          → dissolve_free_hugepages()
          → start_isolate_page_range()
          → sparse_remove_section()
            → section_deactivate()
              → depopulate_section_memmap()  # 释放 memmap
```

### 9.2 移除早期段（使用 free_map_bootmem）

```c
/* 理论场景（实际很少使用） */
void remove_early_memory_section(unsigned long pfn)
{
	struct mem_section *ms = __pfn_to_section(pfn);

	if (early_section(ms)) {
		/* 系统会调用 section_deactivate() */
		section_deactivate(pfn, PAGES_PER_SECTION, NULL);
		/* 内部会判断并调用 free_map_bootmem() */
	}
}
```

---

## 十、总结

### 10.1 核心差异

| 维度 | depopulate_section_memmap | free_map_bootmem |
|------|--------------------------|------------------|
| **内存来源** | 运行时动态分配（热插拔） | 启动时静态分配（early） |
| **标志检查** | `!section_is_early` | `section_is_early` |
| **支持部分释放** | ✅ 支持 subsection | ❌ 仅支持整个 section |
| **altmap 支持** | ✅ 支持设备内存 | ❌ 不支持 |
| **自引用检查** | ❌ 不需要（独立分配） | ✅ 需要（非 VMEMMAP 配置） |
| **复杂度** | 低（直接释放） | 高（条件检查 + 逐页处理） |
| **常见程度** | 非常常见（热插拔） | 罕见（移除 early section） |

### 10.2 设计原则

1. **depopulate_section_memmap**：
   - 针对运行时动态分配的内存
   - 假设 memmap 独立分配，简化处理逻辑
   - 支持现代特性（subsection、altmap）

2. **free_map_bootmem**：
   - 针对启动时静态分配的内存
   - 需要处理历史遗留问题（自引用）
   - 保持向后兼容性

### 10.3 为什么需要两个函数？

1. **生命周期不同**：
   - 启动时分配：使用 memblock 分配器
   - 运行时分配：使用 buddy 系统或 vmalloc

2. **元数据管理不同**：
   - early section：带有 `SECTION_IS_EARLY` 标志
   - 运行时 section：无此标志

3. **风险模型不同**：
   - early section：可能存在自引用（非 VMEMMAP）
   - 运行时 section：独立分配，无自引用风险

4. **功能需求不同**：
   - early section：整体操作，简单
   - 运行时 section：支持 subsection、altmap 等高级特性

### 10.4 推荐配置

**现代系统推荐使用 `CONFIG_SPARSEMEM_VMEMMAP=y`**：
- 统一的 vmemmap 虚拟地址空间
- 支持大页映射，性能更好
- 两个函数的实现更简单（都调用 `vmemmap_free()`）
- 避免自引用问题

---

## 十一、相关代码位置索引

| 功能 | 文件路径 | 行号 |
|------|---------|------|
| depopulate_section_memmap (VMEMMAP) | mm/sparse.c | 638-645 |
| depopulate_section_memmap (非 VMEMMAP) | mm/sparse.c | 711-715 |
| free_map_bootmem (VMEMMAP) | mm/sparse.c | 646-652 |
| free_map_bootmem (非 VMEMMAP) | mm/sparse.c | 717-745 |
| section_deactivate | mm/sparse.c | 779-825 |
| early_section 定义 | include/linux/mmzone.h | 1910-1913 |
| SECTION_IS_EARLY 宏 | include/linux/mmzone.h | 1871, 1881 |
| populate_section_memmap (VMEMMAP) | mm/sparse.c | 631-636 |
| populate_section_memmap (非 VMEMMAP) | mm/sparse.c | 703-709 |
| sparse_init_one_section | mm/sparse.c | 300-308 |
| sparse_init_nid | mm/sparse.c | 500-552 |
