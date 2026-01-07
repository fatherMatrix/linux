# ARM64 与 x86 架构 pte_mkwrite 标记对比分析

## 一、概述

本文档详细分析 ARM64 和 x86 两种架构下 `pte_mkwrite` 函数的实现差异，以及各自的硬件特性和软件设计。

---

## 二、ARM64 架构实现

### 2.1 核心实现

**位置**：`arch/arm64/include/asm/pgtable.h:184`

```c
static inline pte_t pte_mkwrite_novma(pte_t pte)
{
	pte = set_pte_bit(pte, __pgprot(PTE_WRITE));
	pte = clear_pte_bit(pte, __pgprot(PTE_RDONLY));
	return pte;
}
```

### 2.2 硬件位定义

**位置**：`arch/arm64/include/asm/pgtable-prot.h` 和 `arch/arm64/include/asm/pgtable-hwdef.h`

```c
/* 软件定义位 */
#define PTE_WRITE       (PTE_DBM)              /* bit 51 - 同 DBM (Dirty Bit Management) */
#define PTE_DIRTY       (_AT(pteval_t, 1) << 55)  /* bit 55 - 软件脏位 */

/* 硬件定义位 */
#define PTE_RDONLY      (_AT(pteval_t, 1) << 7)   /* bit 7 - AP[2] 只读位 */
#define PTE_DBM         (_AT(pteval_t, 1) << 51)  /* bit 51 - 硬件脏位管理 */
```

### 2.3 ARM64 的双重标记机制

ARM64 使用**双重标记**来管理写权限和脏位：

#### (1) 硬件位：PTE_RDONLY (bit 7)
- **硬件强制**：MMU 直接检查此位控制页面是否可写
- `PTE_RDONLY = 1`：页面只读，写操作触发页错误
- `PTE_RDONLY = 0`：页面可写（需配合其他位）

#### (2) 软件位：PTE_WRITE (bit 51)
- **软件标记**：内核用于追踪页面的逻辑可写状态
- 复用硬件的 DBM (Dirty Bit Management) 位
- 用于快速判断页面的写权限状态

#### (3) 关系说明

```
可写页面：PTE_WRITE = 1 && PTE_RDONLY = 0
只读页面：PTE_WRITE = 0 || PTE_RDONLY = 1
```

**判断逻辑**（`arch/arm64/include/asm/pgtable.h:105`）：
```c
#define pte_write(pte)  (!!(pte_val(pte) & PTE_WRITE))
```

### 2.4 脏位管理机制

ARM64 采用**软硬件混合**的脏位管理：

```c
/* arch/arm64/include/asm/pgtable.h:124-126 */
#define pte_hw_dirty(pte)  (pte_write(pte) && !pte_rdonly(pte))
#define pte_sw_dirty(pte)  (!!(pte_val(pte) & PTE_DIRTY))
#define pte_dirty(pte)     (pte_sw_dirty(pte) || pte_hw_dirty(pte))
```

- **硬件脏位**：`pte_write(pte) && !pte_rdonly(pte)` - 页面实际可写
- **软件脏位**：`PTE_DIRTY` (bit 55) - 内核追踪修改状态
- **判断脏页**：两者之一为真即认为是脏页

### 2.5 pte_mkdirty 实现

```c
/* arch/arm64/include/asm/pgtable.h:197-207 */
static inline pte_t pte_mkdirty(pte_t pte)
{
	pte = set_pte_bit(pte, __pgprot(PTE_DIRTY));

	if (pte_write(pte))
		pte = clear_pte_bit(pte, __pgprot(PTE_RDONLY));

	return pte;
}
```

**关键点**：
- 设置软件脏位 `PTE_DIRTY`
- 如果页面可写（`PTE_WRITE`），同时清除 `PTE_RDONLY` 使其硬件可写
- 确保脏页必须是可写的

---

## 三、x86 架构实现

### 3.1 核心实现

**位置**：`arch/x86/include/asm/pgtable.h:454` 和 `arch/x86/mm/pgtable.c:888`

```c
/* 基础版本 - 仅设置 RW 位 */
static inline pte_t pte_mkwrite_novma(pte_t pte)
{
	return pte_set_flags(pte, _PAGE_RW);
}

/* VMA 感知版本 - 支持 shadow stack */
pte_t pte_mkwrite(pte_t pte, struct vm_area_struct *vma)
{
	if (vma->vm_flags & VM_SHADOW_STACK)
		return pte_mkwrite_shstk(pte);

	pte = pte_mkwrite_novma(pte);

	return pte_clear_saveddirty(pte);
}
```

### 3.2 硬件位定义

**位置**：`arch/x86/include/asm/pgtable_types.h`

```c
#define _PAGE_RW        (_AT(pteval_t, 1) << 1)   /* bit 1 - Read/Write */
#define _PAGE_DIRTY     (_AT(pteval_t, 1) << 6)   /* bit 6 - Dirty */
#define _PAGE_SAVED_DIRTY (_AT(pteval_t, 1) << _PAGE_BIT_SOFTW5)  /* 软件保存的脏位 */

#define _PAGE_DIRTY_BITS (_PAGE_DIRTY | _PAGE_SAVED_DIRTY)
```

### 3.3 x86 的简单标记机制

x86 使用**单一硬件位**控制写权限：

- `_PAGE_RW = 1`：页面可读写
- `_PAGE_RW = 0`：页面只读

**判断逻辑**（`arch/x86/include/asm/pgtable.h:172-179`）：
```c
static inline int pte_write(pte_t pte)
{
	/*
	 * Shadow stack 页面逻辑上可写，但不设置 _PAGE_RW。
	 * 需要单独检查 shadow stack 状态。
	 */
	return (pte_flags(pte) & _PAGE_RW) || pte_shstk(pte);
}
```

### 3.4 Shadow Stack 支持（x86 特有）

Intel CET (Control-flow Enforcement Technology) 的 Shadow Stack 特性：

#### (1) Shadow Stack 检测
```c
/* arch/x86/include/asm/pgtable.h:133-137 */
static inline bool pte_shstk(pte_t pte)
{
	return cpu_feature_enabled(X86_FEATURE_SHSTK) &&
	       (pte_flags(pte) & (_PAGE_RW | _PAGE_DIRTY)) == _PAGE_DIRTY;
}
```

**特征**：`_PAGE_RW = 0 && _PAGE_DIRTY = 1`
- 不可写（`_PAGE_RW = 0`）
- 但设置脏位（`_PAGE_DIRTY = 1`）
- 这是 x86 硬件用于标识 shadow stack 的特殊组合

#### (2) Shadow Stack 写标记
```c
/* arch/x86/include/asm/pgtable.h:442-447 */
static inline pte_t pte_mkwrite_shstk(pte_t pte)
{
	pte = pte_clear_flags(pte, _PAGE_RW);
	return pte_set_flags(pte, _PAGE_DIRTY);
}
```

**操作**：
- 清除 `_PAGE_RW`（标记为"不可写"）
- 设置 `_PAGE_DIRTY`（标记为 shadow stack）
- 结果：创建一个特殊的"逻辑可写"页面用于 shadow stack

### 3.5 脏位管理

```c
/* arch/x86/include/asm/pgtable.h:128-131 */
static inline bool pte_dirty(pte_t pte)
{
	return pte_flags(pte) & _PAGE_DIRTY_BITS;
}
```

- `_PAGE_DIRTY_BITS = _PAGE_DIRTY | _PAGE_SAVED_DIRTY`
- 检查硬件脏位和软件保存的脏位

---

## 四、关键差异对比

| 特性 | ARM64 | x86 |
|------|-------|-----|
| **写权限控制位** | `PTE_WRITE` (bit 51) + `PTE_RDONLY` (bit 7) | `_PAGE_RW` (bit 1) |
| **标记机制** | 双重标记（软件 + 硬件） | 单一硬件位 |
| **脏位位置** | bit 55 (软件) + 硬件推断 | bit 6 (硬件) |
| **Shadow Stack** | 不支持 | 支持（CET 特性） |
| **pte_mkwrite 实现** | 设置 WRITE + 清除 RDONLY | 设置 RW（或特殊处理 shadow stack） |
| **VMA 感知** | pte_mkwrite_novma（不感知） | pte_mkwrite（感知，处理 shadow stack） |
| **硬件特性** | DBM (Dirty Bit Management) | 简单的 RW 位 |

---

## 五、设计哲学差异

### 5.1 ARM64：分离关注点

- **硬件控制**：`PTE_RDONLY` - MMU 强制执行
- **软件追踪**：`PTE_WRITE` - 内核快速查询
- **优势**：
  - 软件可以独立管理写权限状态
  - 不影响硬件的实际权限检查
  - 支持更复杂的状态机

### 5.2 x86：直接映射

- **统一标记**：`_PAGE_RW` 同时控制硬件和软件逻辑
- **优势**：
  - 简单直接
  - 硬件和软件视图一致
- **劣势**：
  - 需要特殊技巧支持 shadow stack（复用 dirty 位）

---

## 六、使用场景分析

### 6.1 常规页面标记为可写

**ARM64**：
```c
pte = pte_mkwrite_novma(pte);
// 结果：PTE_WRITE = 1, PTE_RDONLY = 0
```

**x86**：
```c
pte = pte_mkwrite_novma(pte);
// 结果：_PAGE_RW = 1
```

### 6.2 Shadow Stack 页面（仅 x86）

```c
pte = pte_mkwrite(pte, vma);  // vma->vm_flags & VM_SHADOW_STACK
// 结果：_PAGE_RW = 0, _PAGE_DIRTY = 1
// 硬件识别为 shadow stack，特殊处理写操作
```

### 6.3 脏页判断

**ARM64**：
```c
is_dirty = pte_dirty(pte);
// 检查：软件脏位 || (可写 && 非只读)
```

**x86**：
```c
is_dirty = pte_dirty(pte);
// 检查：_PAGE_DIRTY || _PAGE_SAVED_DIRTY
```

---

## 七、关键代码路径

### 7.1 ARM64 写保护/解除保护

```c
/* 写保护 */
pte = pte_wrprotect(pte);
// 操作：clear PTE_WRITE, set PTE_RDONLY

/* 标记可写 */
pte = pte_mkwrite_novma(pte);
// 操作：set PTE_WRITE, clear PTE_RDONLY

/* 标记脏 */
pte = pte_mkdirty(pte);
// 操作：set PTE_DIRTY, 如果 pte_write() 则 clear PTE_RDONLY
```

### 7.2 x86 写保护/解除保护

```c
/* 写保护 */
pte = pte_wrprotect(pte);
// 操作：clear _PAGE_RW

/* 标记可写 */
pte = pte_mkwrite(pte, vma);
// 操作：
//   - 普通页面：set _PAGE_RW
//   - Shadow stack：clear _PAGE_RW, set _PAGE_DIRTY

/* 标记脏 */
pte = pte_mkdirty(pte);
// 操作：set _PAGE_DIRTY
```

---

## 八、硬件特性支持

### 8.1 ARM64 DBM (Dirty Bit Management)

- **硬件自动脏位管理**
- PTE_DBM (bit 51) 启用后，硬件写操作时自动设置 dirty 状态
- 软件复用此位作为 `PTE_WRITE` 标记

### 8.2 x86 CET Shadow Stack

- **Control-flow Enforcement Technology**
- 使用特殊的页表项编码（`RW=0, Dirty=1`）
- 硬件识别此模式并允许特殊的 shadow stack 写操作
- 用于防御 ROP (Return-Oriented Programming) 攻击

---

## 九、总结

### 9.1 ARM64 优势

1. **灵活的权限管理**：软硬件分离，支持更复杂的状态
2. **DBM 硬件加速**：自动脏位管理
3. **清晰的语义**：WRITE 和 RDONLY 分别表达意图

### 9.2 x86 优势

1. **简单直接**：单一 RW 位控制
2. **Shadow Stack 支持**：硬件级安全特性
3. **兼容性好**：长期稳定的设计

### 9.3 架构选择考量

- **ARM64**：适合需要复杂权限管理和状态追踪的场景
- **x86**：适合需要安全特性（如 shadow stack）和简单直接的场景

两种架构都实现了相同的功能目标（标记页面可写），但采用了不同的实现策略，反映了各自的硬件特性和设计哲学。
