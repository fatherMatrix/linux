# ARM64 SWAPPER_TABLE_SHIFT 详解

## 一、概述

`SWAPPER_TABLE_SHIFT` 是 ARM64 架构中用于**早期内核页表初始化**的关键宏定义，它决定了在构建 swapper 页表（内核初始页表）时，某一特定页表级别的地址位移量。这个宏在内核启动的汇编代码中用于计算页表索引，从而正确地建立内核的虚拟地址映射。

**核心作用**：定义 swapper 页表中间层级的地址位移，用于在启动时创建内核页表结构。

---

## 二、定义与配置

### 2.1 宏定义

**位置**：`arch/arm64/include/asm/kernel-pgtable.h:97-105`

```c
/* Initial memory map size */
#ifdef CONFIG_ARM64_4K_PAGES
#define SWAPPER_BLOCK_SHIFT	PMD_SHIFT
#define SWAPPER_BLOCK_SIZE	PMD_SIZE
#define SWAPPER_TABLE_SHIFT	PUD_SHIFT    /* 4KB 页时：使用 PUD 级别 */
#else
#define SWAPPER_BLOCK_SHIFT	PAGE_SHIFT
#define SWAPPER_BLOCK_SIZE	PAGE_SIZE
#define SWAPPER_TABLE_SHIFT	PMD_SHIFT    /* 16KB/64KB 页时：使用 PMD 级别 */
#endif
```

### 2.2 配置依赖

#### (1) 页面大小配置

ARM64 支持三种页面大小：

| 配置选项 | 页面大小 | PAGE_SHIFT | SWAPPER_TABLE_SHIFT |
|---------|---------|------------|---------------------|
| CONFIG_ARM64_4K_PAGES | 4KB | 12 | PUD_SHIFT |
| CONFIG_ARM64_16K_PAGES | 16KB | 14 | PMD_SHIFT |
| CONFIG_ARM64_64K_PAGES | 64KB | 16 | PMD_SHIFT |

#### (2) 页表级别计算

**位置**：`arch/arm64/include/asm/pgtable-hwdef.h:41-69`

```c
/*
 * ARM64 页表级别位移计算公式：
 *   (4 - n) * (PAGE_SHIFT - 3) + 3
 */
#define ARM64_HW_PGTABLE_LEVEL_SHIFT(n)	((PAGE_SHIFT - 3) * (4 - (n)) + 3)

/* PMD_SHIFT 决定了 level 2 页表项能映射的大小 */
#if CONFIG_PGTABLE_LEVELS > 2
#define PMD_SHIFT		ARM64_HW_PGTABLE_LEVEL_SHIFT(2)
#define PMD_SIZE		(_AC(1, UL) << PMD_SHIFT)
#define PMD_MASK		(~(PMD_SIZE-1))
#define PTRS_PER_PMD		(1 << (PAGE_SHIFT - 3))
#endif

/* PUD_SHIFT 决定了 level 1 页表项能映射的大小 */
#if CONFIG_PGTABLE_LEVELS > 3
#define PUD_SHIFT		ARM64_HW_PGTABLE_LEVEL_SHIFT(1)
#define PUD_SIZE		(_AC(1, UL) << PUD_SHIFT)
#define PUD_MASK		(~(PUD_SIZE-1))
#define PTRS_PER_PUD		(1 << (PAGE_SHIFT - 3))
#endif
```

### 2.3 具体数值示例

#### 4KB 页面配置

```
PAGE_SHIFT = 12

PMD_SHIFT = (PAGE_SHIFT - 3) * (4 - 2) + 3
          = (12 - 3) * 2 + 3
          = 9 * 2 + 3
          = 21
PMD_SIZE  = 1 << 21 = 2MB

PUD_SHIFT = (PAGE_SHIFT - 3) * (4 - 1) + 3
          = (12 - 3) * 3 + 3
          = 9 * 3 + 3
          = 30
PUD_SIZE  = 1 << 30 = 1GB

SWAPPER_TABLE_SHIFT = PUD_SHIFT = 30
SWAPPER_BLOCK_SHIFT = PMD_SHIFT = 21
```

#### 16KB 页面配置

```
PAGE_SHIFT = 14

PMD_SHIFT = (PAGE_SHIFT - 3) * (4 - 2) + 3
          = (14 - 3) * 2 + 3
          = 11 * 2 + 3
          = 25
PMD_SIZE  = 1 << 25 = 32MB

PUD_SHIFT = (PAGE_SHIFT - 3) * (4 - 1) + 3
          = (14 - 3) * 3 + 3
          = 11 * 3 + 3
          = 36
PUD_SIZE  = 1 << 36 = 64GB

SWAPPER_TABLE_SHIFT = PMD_SHIFT = 25
SWAPPER_BLOCK_SHIFT = PAGE_SHIFT = 14
```

#### 64KB 页面配置

```
PAGE_SHIFT = 16

PMD_SHIFT = (PAGE_SHIFT - 3) * (4 - 2) + 3
          = (16 - 3) * 2 + 3
          = 13 * 2 + 3
          = 29
PMD_SIZE  = 1 << 29 = 512MB

SWAPPER_TABLE_SHIFT = PMD_SHIFT = 29
SWAPPER_BLOCK_SHIFT = PAGE_SHIFT = 16
```

---

## 三、为什么 4KB 与 16KB/64KB 不同？

### 3.1 核心原因：块映射粒度

**位置**：`arch/arm64/include/asm/kernel-pgtable.h:15-20`

```c
/*
 * The linear mapping and the start of memory are both 2M aligned (per
 * the arm64 booting.txt requirements). Hence we can use section mapping
 * with 4K (section size = 2M) but not with 16K (section size = 32M) or
 * 64K (section size = 512M).
 */
```

#### 关键约束

1. **内存对齐要求**：
   - 线性映射和内存起始地址都是 **2MB 对齐**
   - 这是 ARM64 启动协议（booting.txt）的要求

2. **块映射能力**：
   - **4KB 页**：PMD 映射粒度是 2MB，**正好匹配** 2MB 对齐
   - **16KB 页**：PMD 映射粒度是 32MB，**超过** 2MB 对齐
   - **64KB 页**：PMD 映射粒度是 512MB，**超过** 2MB 对齐

### 3.2 映射策略差异

#### (1) 4KB 页面：使用 PMD 块映射（Section Mapping）

```
页表结构（4 级页表）：
  PGD (Level 0)
   └─> PUD (Level 1)  ← SWAPPER_TABLE_SHIFT = PUD_SHIFT (30)
        └─> PMD (Level 2)  ← SWAPPER_BLOCK_SHIFT = PMD_SHIFT (21)
             └─> [2MB 块映射，直接指向物理内存]

优势：
  - 使用 PMD 块映射，每个 PMD 表项映射 2MB
  - 映射粒度与 2MB 对齐完美匹配
  - 减少页表层级，提高效率
```

#### (2) 16KB/64KB 页面：必须使用 PTE 页映射

```
页表结构（4 级页表）：
  PGD (Level 0)
   └─> PUD (Level 1)
        └─> PMD (Level 2)  ← SWAPPER_TABLE_SHIFT = PMD_SHIFT
             └─> PTE (Level 3)  ← SWAPPER_BLOCK_SHIFT = PAGE_SHIFT
                  └─> [16KB/64KB 页映射]

原因：
  - PMD 块映射粒度（32MB/512MB）远大于 2MB 对齐
  - 无法使用块映射，必须细化到 PTE 级别
  - 使用 16KB/64KB 页面大小进行映射
```

### 3.3 SWAPPER_PGTABLE_LEVELS 的差异

**位置**：`arch/arm64/include/asm/kernel-pgtable.h:32-36`

```c
#ifdef CONFIG_ARM64_4K_PAGES
#define SWAPPER_PGTABLE_LEVELS	(CONFIG_PGTABLE_LEVELS - 1)
#else
#define SWAPPER_PGTABLE_LEVELS	(CONFIG_PGTABLE_LEVELS)
#endif
```

#### 原因

- **4KB 页**：使用 PMD 块映射，**不需要** PTE 级别
  - `SWAPPER_PGTABLE_LEVELS = 3`（PGD + PUD + PMD）

- **16KB/64KB 页**：需要到达 PTE 级别才能映射
  - `SWAPPER_PGTABLE_LEVELS = 4`（PGD + PUD + PMD + PTE）

---

## 四、在启动代码中的使用

### 4.1 map_memory 宏

**位置**：`arch/arm64/kernel/head.S:262-298`

```assembly
/*
 * 创建内存映射的宏
 *
 * 参数：
 *   tbl:   页表基地址
 *   vstart: 虚拟地址起始
 *   vend:   虚拟地址结束
 *   phys:   物理地址起始
 *   flags:  映射标志
 */
.macro map_memory, tbl, rtbl, vstart, vend, flags, phys, order, istart, iend, tmp, count, sv, extra_shift
	/* ... 初始化代码 ... */

	/* Level 0 (PGDIR) */
	compute_indices \vstart, \vend, #PGDIR_SHIFT, #\order, \istart, \iend, \count
	mov \sv, \rtbl
	populate_entries \tbl, \rtbl, \istart, \iend, #PMD_TYPE_TABLE, #PAGE_SIZE, \tmp
	mov \tbl, \sv

#if SWAPPER_PGTABLE_LEVELS > 3
	/* Level 1 (PUD) */
	compute_indices \vstart, \vend, #PUD_SHIFT, #(PAGE_SHIFT - 3), \istart, \iend, \count
	mov \sv, \rtbl
	populate_entries \tbl, \rtbl, \istart, \iend, #PMD_TYPE_TABLE, #PAGE_SIZE, \tmp
	mov \tbl, \sv
#endif

#if SWAPPER_PGTABLE_LEVELS > 2
	/* Level 2 (PMD) - 使用 SWAPPER_TABLE_SHIFT */
	compute_indices \vstart, \vend, #SWAPPER_TABLE_SHIFT, #(PAGE_SHIFT - 3), \istart, \iend, \count
	mov \sv, \rtbl
	populate_entries \tbl, \rtbl, \istart, \iend, #PMD_TYPE_TABLE, #PAGE_SIZE, \tmp
	mov \tbl, \sv
#endif

	/* 最终级别映射 - 使用 SWAPPER_BLOCK_SHIFT */
	compute_indices \vstart, \vend, #SWAPPER_BLOCK_SHIFT, #(PAGE_SHIFT - 3), \istart, \iend, \count
	bic \rtbl, \phys, #SWAPPER_BLOCK_SIZE - 1
	populate_entries \tbl, \rtbl, \istart, \iend, \flags, #SWAPPER_BLOCK_SIZE, \tmp
.endm
```

### 4.2 SWAPPER_TABLE_SHIFT 的作用

#### (1) 计算页表索引

```assembly
compute_indices \vstart, \vend, #SWAPPER_TABLE_SHIFT, #(PAGE_SHIFT - 3), \istart, \iend, \count
```

**功能**：
- 根据 `SWAPPER_TABLE_SHIFT` 计算虚拟地址在该级别页表中的索引范围
- `PAGE_SHIFT - 3`：每个页表项的大小是 8 字节（2^3），一个 PAGE 可以容纳 `2^(PAGE_SHIFT - 3)` 个表项

**计算公式**：
```c
index = (vaddr >> SWAPPER_TABLE_SHIFT) & ((1 << (PAGE_SHIFT - 3)) - 1)
```

#### (2) 填充页表项

```assembly
populate_entries \tbl, \rtbl, \istart, \iend, #PMD_TYPE_TABLE, #PAGE_SIZE, \tmp
```

**功能**：
- 在 `\tbl` 指向的页表中，填充从 `\istart` 到 `\iend` 的表项
- 每个表项指向下一级页表（`#PMD_TYPE_TABLE` 标志）
- 下一级页表的地址存储在 `\rtbl` 中

### 4.3 执行流程示例（4KB 页面）

假设要映射虚拟地址范围 `[0xffff_8000_0000_0000, 0xffff_8000_4000_0000)`：

```
1. PGDIR 级别 (PGDIR_SHIFT = 39)：
   索引范围：[256, 256]（只需要一个 PGD 表项）
   创建指向 PUD 表的表项

2. PUD 级别 (PUD_SHIFT = 30) ← SWAPPER_TABLE_SHIFT：
   索引范围：[0, 0]（只需要一个 PUD 表项）
   创建指向 PMD 表的表项

3. PMD 级别 (PMD_SHIFT = 21) ← SWAPPER_BLOCK_SHIFT：
   索引范围：[0, 31]（需要 32 个 PMD 表项）
   每个表项直接映射 2MB 物理内存（块映射）
   总共映射：32 * 2MB = 64MB
```

### 4.4 执行流程示例（64KB 页面）

假设要映射相同的虚拟地址范围：

```
1. PGDIR 级别：
   索引范围：[256, 256]
   创建指向 PUD 表的表项

2. PUD 级别：
   索引范围：[0, 0]
   创建指向 PMD 表的表项

3. PMD 级别 (PMD_SHIFT = 29) ← SWAPPER_TABLE_SHIFT：
   索引范围：[0, 0]（只需要一个 PMD 表项）
   创建指向 PTE 表的表项

4. PTE 级别 (PAGE_SHIFT = 16) ← SWAPPER_BLOCK_SHIFT：
   索引范围：[0, 1023]
   每个表项映射 64KB 物理内存
   总共映射：1024 * 64KB = 64MB
```

---

## 五、EARLY_PMDS 宏的使用

### 5.1 宏定义

**位置**：`arch/arm64/include/asm/kernel-pgtable.h:76-80`

```c
#if SWAPPER_PGTABLE_LEVELS > 2
#define EARLY_PMDS(vstart, vend, add) (EARLY_ENTRIES(vstart, vend, SWAPPER_TABLE_SHIFT, add))
#else
#define EARLY_PMDS(vstart, vend, add) (0)
#endif
```

### 5.2 作用：计算需要的页表页数

```c
#define SPAN_NR_ENTRIES(vstart, vend, shift) \
	((((vend) - 1) >> (shift)) - ((vstart) >> (shift)) + 1)

#define EARLY_ENTRIES(vstart, vend, shift, add) \
	(SPAN_NR_ENTRIES(vstart, vend, shift) + (add))
```

**功能**：
- 计算从 `vstart` 到 `vend` 的虚拟地址范围需要多少个 PMD 表项
- 每个 PMD 表项对应一个下一级页表页
- `add` 参数用于 KASLR（内核地址空间布局随机化）的额外空间

### 5.3 页表总页数计算

**位置**：`arch/arm64/include/asm/kernel-pgtable.h:82-86`

```c
#define EARLY_PAGES(vstart, vend, add) ( 1 			/* PGDIR page */				\
		+ EARLY_PGDS((vstart), (vend), add) 	/* each PGDIR needs a next level page table */	\
		+ EARLY_PUDS((vstart), (vend), add)	/* each PUD needs a next level page table */	\
		+ EARLY_PMDS((vstart), (vend), add))	/* each PMD needs a next level page table */

#define INIT_DIR_SIZE (PAGE_SIZE * EARLY_PAGES(KIMAGE_VADDR, _end, EARLY_KASLR))
```

**说明**：
- 计算内核启动时需要预留的页表空间总大小
- 包括所有级别的页表页
- `SWAPPER_TABLE_SHIFT` 通过 `EARLY_PMDS` 影响总页表大小

---

## 六、与其他宏的关系

### 6.1 SWAPPER_BLOCK_SHIFT vs SWAPPER_TABLE_SHIFT

| 宏 | 作用 | 4KB 页面值 | 16KB/64KB 页面值 |
|----|------|-----------|----------------|
| **SWAPPER_TABLE_SHIFT** | 中间级别页表的位移 | PUD_SHIFT (30) | PMD_SHIFT (25/29) |
| **SWAPPER_BLOCK_SHIFT** | 最终映射块的位移 | PMD_SHIFT (21) | PAGE_SHIFT (14/16) |

**关系**：
- `SWAPPER_TABLE_SHIFT` > `SWAPPER_BLOCK_SHIFT`
- `SWAPPER_TABLE_SHIFT` 定义"需要下一级页表"的层级
- `SWAPPER_BLOCK_SHIFT` 定义"最终映射粒度"的层级

### 6.2 页表层级结构图

#### 4KB 页面（3 级 swapper 页表）

```
虚拟地址位字段分解：
  [47:39]   [38:30]         [29:21]         [20:12]   [11:0]
  PGDIR     PUD             PMD             [unused]  Offset
            ↑               ↑
            TABLE_SHIFT     BLOCK_SHIFT

结构：
  PGDIR → PUD → PMD (2MB 块) → 物理内存
```

#### 64KB 页面（4 级 swapper 页表）

```
虚拟地址位字段分解：
  [47:42]   [41:29]         [28:16]         [15:0]
  PGDIR     PUD             PMD             PTE & Offset
                            ↑               ↑
                            TABLE_SHIFT     BLOCK_SHIFT

结构：
  PGDIR → PUD → PMD → PTE (64KB 页) → 物理内存
```

---

## 七、KASLR 的影响

### 7.1 KASLR 概述

**位置**：`arch/arm64/include/asm/kernel-pgtable.h:39-54`

```c
/*
 * If KASLR is enabled, then an offset K is added to the kernel address
 * space. The bottom 21 bits of this offset are zero to guarantee 2MB
 * alignment for PA and VA.
 *
 * For each pagetable level of the swapper, we know that the shift will
 * be larger than 21 (for the 4KB granule case we use section maps thus
 * the smallest shift is actually 30) thus there is the possibility that
 * KASLR can increase the number of pagetable entries by 1, so we make
 * room for this extra entry.
 *
 * Note KASLR cannot increase the number of required entries for a level
 * by more than one because it increments both the virtual start and end
 * addresses equally (the extra entry comes from the case where the end
 * address is just pushed over a boundary and the start address isn't).
 */
```

### 7.2 KASLR 对 SWAPPER_TABLE_SHIFT 的影响

**关键点**：
1. KASLR 偏移的低 21 位为 0（保证 2MB 对齐）
2. 所有 swapper 页表级别的 shift 都 > 21
3. KASLR 可能使某一级别的页表项数量 **+1**

**原因**：
```
假设 SWAPPER_TABLE_SHIFT = 30 (PUD_SHIFT)
原始范围：[0xffff_8000_0000_0000, 0xffff_8000_4000_0000)
  - 起始地址 PUD 索引：0
  - 结束地址 PUD 索引：0
  - 需要 1 个 PUD 表项

加上 KASLR 偏移（例如 0x3fe0_0000，约 1GB - 32MB）：
偏移后范围：[0xffff_8000_3fe0_0000, 0xffff_8000_7fe0_0000)
  - 起始地址 PUD 索引：0
  - 结束地址 PUD 索引：1（跨越了 1GB 边界）
  - 需要 2 个 PUD 表项（增加 1 个）
```

### 7.3 预留空间

```c
#ifdef CONFIG_RANDOMIZE_BASE
#define EARLY_KASLR	(1)
#else
#define EARLY_KASLR	(0)
#endif

#define EARLY_PMDS(vstart, vend, add) (EARLY_ENTRIES(vstart, vend, SWAPPER_TABLE_SHIFT, add))
```

- `EARLY_KASLR` 为 1 时，每个级别预留额外 1 个页表项空间
- 确保 KASLR 开启时有足够的页表空间

---

## 八、实际应用场景

### 8.1 内核启动流程

1. **CPU 进入内核入口**（`arch/arm64/kernel/head.S`）：
   - MMU 关闭
   - 物理地址空间

2. **创建恒等映射（Identity Mapping）**：
   - 使用 `map_memory` 宏
   - 映射物理地址到相同的虚拟地址
   - 确保开启 MMU 后代码仍可执行

3. **创建内核虚拟地址映射**：
   - 使用 `map_memory` 宏
   - 映射内核物理地址到 `KIMAGE_VADDR`（通常是 `0xffff_8000_0000_0000`）
   - `SWAPPER_TABLE_SHIFT` 决定中间级别页表的创建

4. **开启 MMU**：
   - 加载 `swapper_pg_dir` 到 TTBR1（内核页表基址寄存器）
   - 启用 MMU
   - 跳转到虚拟地址继续执行

### 8.2 示例代码片段

**位置**：`arch/arm64/kernel/head.S`（`__create_page_tables` 函数）

```assembly
__create_page_tables:
	/* 准备页表基地址 */
	adrp	x0, init_pg_dir
	mov	x1, xzr
	mov	x2, #INIT_DIR_SIZE
	bl	__pi_memset

	/* 创建恒等映射 */
	adrp	x0, init_idmap_pg_dir
	adrp	x3, _text
	adrp	x4, _end
	adrp	x5, __inittext_end
	map_memory x0, x1, x3, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14

	/* 创建内核映射 */
	adrp	x0, init_pg_dir
	mov_q	x5, KIMAGE_VADDR
	add	x5, x5, x23			// add KASLR displacement
	adrp	x6, _end
	adrp	x7, _text
	sub	x6, x6, x7
	add	x6, x5, x6
	map_memory x0, x1, x5, x6, x7, x3, (VA_BITS - PGDIR_SHIFT), x8, x9, x10, x11, x12, x13, (VA_BITS - VABITS_ACTUAL)

	ret
```

---

## 九、调试与验证

### 9.1 编译时检查

```bash
# 查看当前配置的 PAGE_SHIFT 和 SWAPPER_TABLE_SHIFT 值
grep -E "CONFIG_ARM64_.*K_PAGES|PAGE_SHIFT" .config

# 查看生成的汇编代码中的实际值
objdump -d vmlinux | grep -A20 "__create_page_tables"
```

### 9.2 运行时检查

```c
/* 在内核代码中打印相关值 */
pr_info("PAGE_SHIFT: %d\n", PAGE_SHIFT);
pr_info("PMD_SHIFT: %d\n", PMD_SHIFT);
pr_info("PUD_SHIFT: %d\n", PUD_SHIFT);
pr_info("SWAPPER_TABLE_SHIFT: %d\n", SWAPPER_TABLE_SHIFT);
pr_info("SWAPPER_BLOCK_SHIFT: %d\n", SWAPPER_BLOCK_SHIFT);
pr_info("SWAPPER_PGTABLE_LEVELS: %d\n", SWAPPER_PGTABLE_LEVELS);
```

### 9.3 页表结构验证

```c
/* 查看 swapper_pg_dir 的内容 */
void dump_swapper_pgtable(void)
{
	pgd_t *pgd = pgd_offset_k(KIMAGE_VADDR);
	p4d_t *p4d = p4d_offset(pgd, KIMAGE_VADDR);
	pud_t *pud = pud_offset(p4d, KIMAGE_VADDR);
	pmd_t *pmd = pmd_offset(pud, KIMAGE_VADDR);

	pr_info("PGD: %016llx\n", pgd_val(*pgd));
	pr_info("P4D: %016llx\n", p4d_val(*p4d));
	pr_info("PUD: %016llx\n", pud_val(*pud));
	pr_info("PMD: %016llx\n", pmd_val(*pmd));
}
```

---

## 十、总结

### 10.1 核心作用

`SWAPPER_TABLE_SHIFT` 在 ARM64 内核启动时起到以下关键作用：

1. **定义中间级别页表的地址位移**：
   - 4KB 页：使用 PUD_SHIFT (30)，在 PUD 级别创建指向 PMD 表的表项
   - 16KB/64KB 页：使用 PMD_SHIFT (25/29)，在 PMD 级别创建指向 PTE 表的表项

2. **计算页表索引和表项数量**：
   - 在汇编代码中用于计算虚拟地址在页表中的索引
   - 用于计算需要预留的页表空间大小

3. **适应不同页面大小的映射策略**：
   - 4KB 页：利用 PMD 块映射（2MB 粒度），减少页表层级
   - 16KB/64KB 页：必须细化到 PTE 级别，因为块映射粒度太大

### 10.2 设计要点

1. **与 SWAPPER_BLOCK_SHIFT 配合**：
   - `SWAPPER_TABLE_SHIFT` > `SWAPPER_BLOCK_SHIFT`
   - 前者定义"需要下一级页表"，后者定义"最终映射粒度"

2. **2MB 对齐约束**：
   - ARM64 启动协议要求 2MB 对齐
   - 4KB 页的 PMD 块映射（2MB）完美匹配
   - 16KB/64KB 页的块映射粒度太大，必须降级到页映射

3. **KASLR 支持**：
   - 预留额外的页表项空间
   - 确保地址随机化后仍有足够的页表覆盖范围

4. **编译时配置**：
   - 通过宏定义在编译时确定
   - 根据 `CONFIG_ARM64_*K_PAGES` 自动选择合适的值

### 10.3 关键数值总结

| 页面大小 | PAGE_SHIFT | PMD_SHIFT | PUD_SHIFT | SWAPPER_TABLE_SHIFT | SWAPPER_BLOCK_SHIFT | 映射策略 |
|---------|------------|-----------|-----------|---------------------|---------------------|---------|
| 4KB | 12 | 21 (2MB) | 30 (1GB) | 30 (PUD) | 21 (PMD) | PMD 块映射 |
| 16KB | 14 | 25 (32MB) | 36 (64GB) | 25 (PMD) | 14 (PAGE) | PTE 页映射 |
| 64KB | 16 | 29 (512MB) | - | 29 (PMD) | 16 (PAGE) | PTE 页映射 |

### 10.4 理解要点

- **不是运行时变量**：编译时宏定义，在内核镜像中固化
- **仅用于早期启动**：swapper 页表是内核的初始页表，后续会被动态页表替代
- **架构特定**：ARM64 特有，反映了 ARM64 页表硬件的特性
- **性能优化**：通过合理选择映射粒度，减少页表层级和 TLB miss

---

## 十一、相关代码位置索引

| 功能 | 文件路径 | 行号 |
|------|---------|------|
| SWAPPER_TABLE_SHIFT 定义 | arch/arm64/include/asm/kernel-pgtable.h | 100, 104 |
| SWAPPER_BLOCK_SHIFT 定义 | arch/arm64/include/asm/kernel-pgtable.h | 98, 102 |
| SWAPPER_PGTABLE_LEVELS 定义 | arch/arm64/include/asm/kernel-pgtable.h | 33-35 |
| PMD_SHIFT/PUD_SHIFT 定义 | arch/arm64/include/asm/pgtable-hwdef.h | 49, 59 |
| ARM64_HW_PGTABLE_LEVEL_SHIFT | arch/arm64/include/asm/pgtable-hwdef.h | 41 |
| map_memory 宏 | arch/arm64/kernel/head.S | 262-298 |
| EARLY_PMDS 宏 | arch/arm64/include/asm/kernel-pgtable.h | 76-80 |
| EARLY_PAGES 宏 | arch/arm64/include/asm/kernel-pgtable.h | 82-86 |
| KASLR 注释说明 | arch/arm64/include/asm/kernel-pgtable.h | 39-54 |
| 2MB 对齐说明 | arch/arm64/include/asm/kernel-pgtable.h | 15-20 |
