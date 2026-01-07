/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Kernel page table mapping
 *
 * Copyright (C) 2015 ARM Ltd.
 */

#ifndef __ASM_KERNEL_PGTABLE_H
#define __ASM_KERNEL_PGTABLE_H

#include <asm/boot.h>
#include <asm/pgtable-hwdef.h>
#include <asm/sparsemem.h>

/*
 * The linear mapping and the start of memory are both 2M aligned (per
 * the arm64 booting.txt requirements). Hence we can use section mapping
 * with 4K (section size = 2M) but not with 16K (section size = 32M) or
 * 64K (section size = 512M).
 */

/*
 * The idmap and swapper page tables need some space reserved in the kernel
 * image. Both require pgd, pud (4 levels only) and pmd tables to (section)
 * map the kernel. With the 64K page configuration, swapper and idmap need to
 * map to pte level. The swapper also maps the FDT (see __create_page_tables
 * for more information). Note that the number of ID map translation levels
 * could be increased on the fly if system RAM is out of reach for the default
 * VA range, so pages required to map highest possible PA are reserved in all
 * cases.
 */
#ifdef CONFIG_ARM64_4K_PAGES
#define SWAPPER_PGTABLE_LEVELS	(CONFIG_PGTABLE_LEVELS - 1)
#else
#define SWAPPER_PGTABLE_LEVELS	(CONFIG_PGTABLE_LEVELS)
#endif


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

#ifdef CONFIG_RANDOMIZE_BASE
#define EARLY_KASLR	(1)
#else
#define EARLY_KASLR	(0)
#endif

#define SPAN_NR_ENTRIES(vstart, vend, shift) \
	((((vend) - 1) >> (shift)) - ((vstart) >> (shift)) + 1)

#define EARLY_ENTRIES(vstart, vend, shift, add) \
	(SPAN_NR_ENTRIES(vstart, vend, shift) + (add))

#define EARLY_PGDS(vstart, vend, add) (EARLY_ENTRIES(vstart, vend, PGDIR_SHIFT, add))

#if SWAPPER_PGTABLE_LEVELS > 3
#define EARLY_PUDS(vstart, vend, add) (EARLY_ENTRIES(vstart, vend, PUD_SHIFT, add))
#else
#define EARLY_PUDS(vstart, vend, add) (0)
#endif

#if SWAPPER_PGTABLE_LEVELS > 2
#define EARLY_PMDS(vstart, vend, add) (EARLY_ENTRIES(vstart, vend, SWAPPER_TABLE_SHIFT, add))
#else
#define EARLY_PMDS(vstart, vend, add) (0)
#endif

#define EARLY_PAGES(vstart, vend, add) ( 1 			/* PGDIR page */				\
			+ EARLY_PGDS((vstart), (vend), add) 	/* each PGDIR needs a next level page table */	\
			+ EARLY_PUDS((vstart), (vend), add)	/* each PUD needs a next level page table */	\
			+ EARLY_PMDS((vstart), (vend), add))	/* each PMD needs a next level page table */
#define INIT_DIR_SIZE (PAGE_SIZE * EARLY_PAGES(KIMAGE_VADDR, _end, EARLY_KASLR))

/* the initial ID map may need two extra pages if it needs to be extended */
#if VA_BITS < 48
#define INIT_IDMAP_DIR_SIZE	((INIT_IDMAP_DIR_PAGES + 2) * PAGE_SIZE)
#else
#define INIT_IDMAP_DIR_SIZE	(INIT_IDMAP_DIR_PAGES * PAGE_SIZE)
#endif
#define INIT_IDMAP_DIR_PAGES	EARLY_PAGES(KIMAGE_VADDR, _end + MAX_FDT_SIZE + SWAPPER_BLOCK_SIZE, 1)

/*
 * 早期内核页表映射参数
 *
 * SWAPPER_BLOCK_SHIFT: 最终映射块的地址位移（映射粒度）
 * SWAPPER_BLOCK_SIZE:  最终映射块的大小
 * SWAPPER_TABLE_SHIFT: 中间级别页表的地址位移（用于计算页表索引）
 *
 * 配置差异原因：
 * - ARM64 启动协议要求 2MB 对齐
 * - 4KB 页面：PMD 块映射粒度是 2MB，完美匹配，可使用块映射（Section Mapping）
 *   - SWAPPER_TABLE_SHIFT = PUD_SHIFT (30)：在 PUD 级别创建指向 PMD 表的表项
 *   - SWAPPER_BLOCK_SHIFT = PMD_SHIFT (21)：PMD 表项直接映射 2MB 物理内存
 *   - 页表层级：PGD → PUD → PMD (2MB 块) → 物理内存（3 级）
 *
 * - 16KB/64KB 页面：PMD 块映射粒度（32MB/512MB）远大于 2MB，无法使用块映射
 *   - SWAPPER_TABLE_SHIFT = PMD_SHIFT (25/29)：在 PMD 级别创建指向 PTE 表的表项
 *   - SWAPPER_BLOCK_SHIFT = PAGE_SHIFT (14/16)：PTE 表项映射 16KB/64KB 页面
 *   - 页表层级：PGD → PUD → PMD → PTE (16KB/64KB 页) → 物理内存（4 级）
 *
 * 应用场景：
 * - 内核启动汇编代码（head.S）的 map_memory 宏使用这些宏来：
 *   1. 计算虚拟地址在各级页表中的索引（通过 SWAPPER_TABLE_SHIFT）
 *   2. 确定最终的映射粒度（通过 SWAPPER_BLOCK_SHIFT）
 *   3. 创建 swapper 初始页表（内核启动时使用的页表）
 *
 * 详细分析见：Documentation/arm64_swapper_table_shift_analysis.md
 */
/* Initial memory map size */
#ifdef CONFIG_ARM64_4K_PAGES
#define SWAPPER_BLOCK_SHIFT	PMD_SHIFT
#define SWAPPER_BLOCK_SIZE	PMD_SIZE
#define SWAPPER_TABLE_SHIFT	PUD_SHIFT
#else
#define SWAPPER_BLOCK_SHIFT	PAGE_SHIFT
#define SWAPPER_BLOCK_SIZE	PAGE_SIZE
#define SWAPPER_TABLE_SHIFT	PMD_SHIFT
#endif

/*
 * Initial memory map attributes.
 */
#define SWAPPER_PTE_FLAGS	(PTE_TYPE_PAGE | PTE_AF | PTE_SHARED | PTE_UXN)
#define SWAPPER_PMD_FLAGS	(PMD_TYPE_SECT | PMD_SECT_AF | PMD_SECT_S | PTE_UXN)

#ifdef CONFIG_ARM64_4K_PAGES
#define SWAPPER_RW_MMUFLAGS	(PMD_ATTRINDX(MT_NORMAL) | SWAPPER_PMD_FLAGS | PTE_WRITE)
#define SWAPPER_RX_MMUFLAGS	(SWAPPER_RW_MMUFLAGS | PMD_SECT_RDONLY)
#else
#define SWAPPER_RW_MMUFLAGS	(PTE_ATTRINDX(MT_NORMAL) | SWAPPER_PTE_FLAGS | PTE_WRITE)
#define SWAPPER_RX_MMUFLAGS	(SWAPPER_RW_MMUFLAGS | PTE_RDONLY)
#endif

#endif	/* __ASM_KERNEL_PGTABLE_H */
