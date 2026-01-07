# ARM64 为什么使用两个写权限标记？

## 核心问题

为什么 ARM64 需要同时使用 `PTE_WRITE` (bit 51) 和 `PTE_RDONLY` (bit 7) 两个标记来管理写权限，而不像 x86 那样只用一个 `_PAGE_RW` 位？

---

## 一、硬件架构限制

### 1.1 ARM64 MMU 的硬件设计

ARM64 的 MMU 硬件**只识别 `PTE_RDONLY` 位** (AP[2] - Access Permission bit 2)：

```
PTE_RDONLY (bit 7):
  - 1: 页面只读 (Read-Only)
  - 0: 页面可读写 (Read-Write，需配合其他权限位)
```

**关键点**：
- 硬件**不提供**一个独立的"可写"位
- 硬件只提供**只读控制位**
- 这是 ARM 架构规范（ARMv8）的设计决定

### 1.2 与 x86 的对比

| 架构 | 硬件写权限控制 | 语义 |
|------|---------------|------|
| **x86** | `_PAGE_RW` (bit 1) | 1=可写, 0=只读（正向逻辑） |
| **ARM64** | `PTE_RDONLY` (bit 7) | 1=只读, 0=可写（反向逻辑） |

x86 的设计更直观（设置位表示可写），而 ARM64 的设计是反向的（设置位表示只读）。

---

## 二、为什么需要 PTE_WRITE 软件位？

### 2.1 问题场景：写时复制 (Copy-on-Write, COW)

#### 场景描述

1. **父进程有一个可写页面**
   - 原始状态：`PTE_RDONLY = 0` (硬件可写)

2. **fork() 创建子进程**
   - 内核需要实现 COW：将页面标记为只读，等待写操作触发复制
   - 操作：设置 `PTE_RDONLY = 1` (硬件只读)

3. **写时复制完成后，恢复写权限**
   - 问题：**如何知道这个页面原本是可写的？**

#### 问题核心

```c
/* 原始状态 */
pte: PTE_RDONLY = 0  // 可写页面

/* fork 后 COW */
pte: PTE_RDONLY = 1  // 暂时只读

/* COW 完成 - 但如何知道应该恢复为可写？ */
pte: PTE_RDONLY = ?  // 丢失了原始权限信息！
```

**如果只有 `PTE_RDONLY` 位**：
- COW 前：`PTE_RDONLY = 0`（可写）
- COW 后：`PTE_RDONLY = 1`（只读）
- **无法区分**：这个页面原本是可写的，还是本来就是只读的？

### 2.2 解决方案：PTE_WRITE 软件位

**引入 `PTE_WRITE` 位来记录逻辑写权限**：

```c
/* 普通可写页面 */
PTE_WRITE = 1, PTE_RDONLY = 0  // 逻辑可写，硬件可写

/* 普通只读页面 */
PTE_WRITE = 0, PTE_RDONLY = 1  // 逻辑只读，硬件只读

/* COW 临时只读页面 */
PTE_WRITE = 1, PTE_RDONLY = 1  // 逻辑可写，硬件暂时只读

/* COW 完成后恢复 */
if (PTE_WRITE == 1)
    PTE_RDONLY = 0  // 恢复硬件可写
```

### 2.3 代码实现

```c
/* arch/arm64/include/asm/pgtable.h:184-189 */
static inline pte_t pte_mkwrite_novma(pte_t pte)
{
    pte = set_pte_bit(pte, __pgprot(PTE_WRITE));      // 标记逻辑可写
    pte = clear_pte_bit(pte, __pgprot(PTE_RDONLY));   // 设置硬件可写
    return pte;
}

/* arch/arm64/include/asm/pgtable.h:209-221 */
static inline pte_t pte_wrprotect(pte_t pte)
{
    /*
     * 如果硬件脏 (PTE_WRITE 设置且 PTE_RDONLY 清除)，
     * 设置 PTE_DIRTY 位保存脏状态
     */
    if (pte_hw_dirty(pte))
        pte = set_pte_bit(pte, __pgprot(PTE_DIRTY));

    pte = clear_pte_bit(pte, __pgprot(PTE_WRITE));    // 清除逻辑可写
    pte = set_pte_bit(pte, __pgprot(PTE_RDONLY));     // 设置硬件只读
    return pte;
}
```

---

## 三、硬件特性：DBM (Dirty Bit Management)

### 3.1 DBM 是什么？

ARMv8.1 引入的硬件特性：**Hardware Access Flag/Dirty Bit Management**

```c
/* arch/arm64/include/asm/pgtable-hwdef.h:153 */
#define PTE_DBM  (_AT(pteval_t, 1) << 51)  /* Dirty Bit Management */

/* arch/arm64/include/asm/pgtable-prot.h:16 */
#define PTE_WRITE  (PTE_DBM)  /* 复用 DBM 位作为软件写标记 */
```

### 3.2 DBM 的工作原理

当启用 DBM 时：

1. **软件设置** `PTE_DBM` (即 `PTE_WRITE`)
2. **硬件监测**页面的写操作
3. **硬件自动**：
   - 清除 `PTE_RDONLY` (使页面实际可写)
   - 或设置其他脏位标志

### 3.3 为什么复用 DBM？

这是**精妙的设计选择**：

```c
/* 场景 1: 没有硬件 DBM 支持 */
PTE_WRITE = 1, PTE_RDONLY = 0  // 软件管理，明确可写

/* 场景 2: 有硬件 DBM 支持 */
PTE_DBM = 1, PTE_RDONLY = 1    // 初始只读，等待硬件第一次写时自动清除 RDONLY
```

通过复用 `PTE_DBM` 位：
- **软件视角**：`PTE_WRITE` 表示逻辑可写
- **硬件视角**（如果支持 DBM）：`PTE_DBM` 启用自动脏位管理
- **完美融合**：一个位满足两个需求

---

## 四、典型场景分析

### 4.1 场景一：COW (写时复制)

```c
/* 1. 原始可写页面 */
pte: PTE_WRITE=1, PTE_RDONLY=0, PTE_DIRTY=0

/* 2. fork() - 实现 COW */
pte = pte_wrprotect(pte);
// 结果: PTE_WRITE=0, PTE_RDONLY=1, PTE_DIRTY=0 (如果原本未脏)

/* 3. 子进程写操作触发缺页异常 */
// 内核复制页面，为子进程创建新页面

/* 4. 检查原始权限并恢复 */
if (vma->vm_flags & VM_WRITE) {
    pte = pte_mkwrite(pte);
    // 结果: PTE_WRITE=1, PTE_RDONLY=0
}
```

**关键**：`PTE_WRITE` 在 COW 过程中被清除，记录了"这个页面不应该恢复为可写"。

### 4.2 场景二：mprotect() 权限变更

```c
/* 1. 原始可写页面 */
pte: PTE_WRITE=1, PTE_RDONLY=0

/* 2. mprotect(addr, len, PROT_READ) - 改为只读 */
pte = pte_wrprotect(pte);
// 结果: PTE_WRITE=0, PTE_RDONLY=1

/* 3. mprotect(addr, len, PROT_READ|PROT_WRITE) - 恢复可写 */
pte = pte_mkwrite(pte);
// 结果: PTE_WRITE=1, PTE_RDONLY=0
```

### 4.3 场景三：硬件并发更新

```c
/* arch/arm64/mm/fault.c:206-239 - ptep_set_access_flags() */

/*
 * 该函数必须处理其他硬件代理并发更新 accessed/dirty 状态的情况。
 *
 * 硬件可能在软件操作的同时：
 * - 清除 PTE_RDONLY (标记为可写/脏)
 * - 设置 PTE_AF (访问标志)
 *
 * 必须使用原子操作保证一致性。
 */

/* 只保留访问标志和写权限 */
pte_val(entry) &= PTE_RDONLY | PTE_AF | PTE_WRITE | PTE_DIRTY;

/*
 * 使用原子 cmpxchg 设置标志，避免与硬件更新竞争。
 * PTE_RDONLY 必须设置为 *ptep 和 entry 中更宽松的值。
 */
do {
    old_pteval = pteval;
    pteval ^= PTE_RDONLY;
    pteval |= pte_val(entry);
    pteval ^= PTE_RDONLY;
    pteval = cmpxchg_relaxed(&pte_val(*ptep), old_pteval, pteval);
} while (pteval != old_pteval);
```

**问题**：如果没有 `PTE_WRITE` 软件位：
- 硬件可能并发清除 `PTE_RDONLY`
- 软件无法区分：是硬件自动设置可写，还是软件应该保持只读？
- `PTE_WRITE` 提供了**软件意图的明确记录**

---

## 五、与脏位 (Dirty) 的关系

### 5.1 ARM64 的脏位判断

```c
/* arch/arm64/include/asm/pgtable.h:124-126 */
#define pte_hw_dirty(pte)  (pte_write(pte) && !pte_rdonly(pte))
#define pte_sw_dirty(pte)  (!!(pte_val(pte) & PTE_DIRTY))
#define pte_dirty(pte)     (pte_sw_dirty(pte) || pte_hw_dirty(pte))
```

**双重脏位机制**：

1. **硬件脏** (`pte_hw_dirty`)
   - 定义：`PTE_WRITE=1 && PTE_RDONLY=0`
   - 意义：页面**当前**硬件可写，可能已被修改

2. **软件脏** (`pte_sw_dirty`)
   - 定义：`PTE_DIRTY` 位 (bit 55) 设置
   - 意义：软件记录的脏状态

### 5.2 为什么需要两种脏位？

#### 场景：页面从可写变为只读

```c
/* 1. 初始状态：可写页面，已被修改 */
pte: PTE_WRITE=1, PTE_RDONLY=0, PTE_DIRTY=0
// pte_hw_dirty() = true (硬件脏)

/* 2. 写保护页面 (例如 COW) */
pte = pte_wrprotect(pte);

/* pte_wrprotect() 实现 */
if (pte_hw_dirty(pte))
    pte = set_pte_bit(pte, __pgprot(PTE_DIRTY));  // 保存硬件脏状态到软件位

pte = clear_pte_bit(pte, __pgprot(PTE_WRITE));
pte = set_pte_bit(pte, __pgprot(PTE_RDONLY));

/* 3. 结果 */
pte: PTE_WRITE=0, PTE_RDONLY=1, PTE_DIRTY=1
// pte_hw_dirty() = false (硬件不可写)
// pte_sw_dirty() = true (软件记录了之前的脏状态)
// pte_dirty() = true (整体判断为脏)
```

**关键**：`PTE_DIRTY` 软件位保存了"页面曾经可写并可能被修改"的历史信息。

### 5.3 pte_modify 的处理

```c
/* arch/arm64/include/asm/pgtable.h:817-830 */
static inline pte_t pte_modify(pte_t pte, pgprot_t newprot)
{
    const pteval_t mask = PTE_USER | PTE_PXN | PTE_UXN | PTE_RDONLY |
                          PTE_PROT_NONE | PTE_VALID | PTE_WRITE | PTE_GP |
                          PTE_ATTRINDX_MASK;

    /* preserve the hardware dirty information */
    if (pte_hw_dirty(pte))
        pte = set_pte_bit(pte, __pgprot(PTE_DIRTY));

    pte_val(pte) = (pte_val(pte) & ~mask) | (pgprot_val(newprot) & mask);
    return pte;
}
```

**保护硬件脏信息**：在修改页面保护属性时，必须保存当前的硬件脏状态。

---

## 六、为什么 x86 不需要两个位？

### 6.1 x86 的优势：正向逻辑

x86 的 `_PAGE_RW` 是**正向语义**：

```
_PAGE_RW = 1: 可写
_PAGE_RW = 0: 只读
```

在 COW 场景：
```c
/* 原始可写页面 */
pte: _PAGE_RW = 1

/* COW - 设为只读 */
pte: _PAGE_RW = 0

/* 恢复可写时 */
// 可以从 VMA 的 vm_flags 判断原始权限
if (vma->vm_flags & VM_WRITE)
    pte: _PAGE_RW = 1
```

**关键差异**：
- x86 清除 `_PAGE_RW` 后，值为 0（明确只读）
- ARM64 设置 `PTE_RDONLY` 后，值为 1（只读），但**无法区分**是临时只读还是永久只读

### 6.2 x86 的劣势：Shadow Stack 需要特殊处理

正是因为 x86 的简单设计，Shadow Stack 功能需要"滥用" dirty 位：

```c
/* x86 Shadow Stack 的特殊编码 */
_PAGE_RW = 0, _PAGE_DIRTY = 1  // 表示 shadow stack (逻辑可写，但不是常规写)
```

ARM64 的双位设计天然支持更多状态组合，不需要这种技巧。

---

## 七、总结

### 7.1 ARM64 双位设计的根本原因

| 原因 | 说明 |
|------|------|
| **硬件限制** | ARM64 MMU 只提供反向的 `PTE_RDONLY` 位，不提供正向的"可写"位 |
| **状态保存** | 需要记录页面的**逻辑写权限**（软件意图），与**硬件实际权限**分离 |
| **COW 支持** | COW 期间需要临时禁用写权限，但保留"原本可写"的信息 |
| **并发安全** | 硬件可能并发更新页表项，软件需要独立的位来记录意图 |
| **DBM 集成** | 复用硬件 DBM 位作为软件 `PTE_WRITE` 标记，一举两得 |

### 7.2 四种页面状态

| PTE_WRITE | PTE_RDONLY | 含义 |
|-----------|-----------|------|
| 0 | 1 | **永久只读页面** (如只读映射、文本段) |
| 1 | 0 | **正常可写页面** (硬件可写，软件可写) |
| 1 | 1 | **临时只读页面** (COW、mprotect 等，逻辑可写但硬件暂时只读) |
| 0 | 0 | **无效组合** (理论上不应出现) |

### 7.3 设计权衡

**ARM64 的选择**：
- ✅ 灵活的状态管理
- ✅ 支持复杂的内存管理场景
- ✅ 硬件 DBM 特性的完美集成
- ❌ 实现稍复杂，需要管理两个位

**x86 的选择**：
- ✅ 简单直接
- ✅ 硬件软件语义统一
- ❌ Shadow Stack 需要特殊技巧
- ❌ 状态表达能力有限

### 7.4 核心洞察

ARM64 的双位设计本质上是**分离了关注点**：
- **PTE_RDONLY**：硬件控制位 - MMU 实际检查的权限
- **PTE_WRITE**：软件标记位 - 内核意图和逻辑状态

这种分离使得内核可以：
1. 独立管理逻辑权限和硬件权限
2. 在需要时临时改变硬件权限（COW、mprotect）
3. 保留完整的权限历史信息
4. 安全地处理硬件并发更新

这是**架构设计与软件需求的完美结合**。
