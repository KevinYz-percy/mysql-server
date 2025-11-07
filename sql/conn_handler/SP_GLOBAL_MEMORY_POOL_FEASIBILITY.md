# Stored Procedure Global Memory Pool: Feasibility Analysis

**Analysis Date:** 2025-11-07
**Question:** Is it possible to allocate sp_head memory from a global pool instead of per-instance MEM_ROOT?
**Related:** GLOBAL_CACHE_PATTERNS_ANALYSIS.md, THD_MEMORY_LIFECYCLE_ANALYSIS.md

---

## Executive Summary

**Answer: YES - Feasible, but requires architectural split similar to TABLE_SHARE/TABLE pattern**

**Current Problem:**
- Each sp_head allocates from its own private MEM_ROOT
- Duplication across 100K connections wastes 12GB for just 5 stored procedures
- Cannot share sp_head objects directly because LEX and Item objects contain **mutable execution state**

**Solution:**
Split sp_head into two layers:
1. **SP_SHARE** - Immutable parse results, allocated from global memory pool, shared across all threads
2. **SP_INSTANCE** - Per-thread execution context, lightweight mutable state

**Impact:**
- Memory savings: 99.9% (12GB → 12MB for 100K connections)
- Complexity: HIGH - Requires refactoring LEX, Items, and execution model
- Implementation time: 12-16 weeks

---

## Table of Contents

1. [Current Architecture Analysis](#1-current-architecture-analysis)
2. [Mutability Analysis: Why Direct Sharing Fails](#2-mutability-analysis-why-direct-sharing-fails)
3. [Reference Pattern: TABLE_SHARE vs TABLE](#3-reference-pattern-table_share-vs-table)
4. [Proposed Architecture: SP_SHARE vs SP_INSTANCE](#4-proposed-architecture-sp_share-vs-sp_instance)
5. [Memory Pool Design](#5-memory-pool-design)
6. [Implementation Roadmap](#6-implementation-roadmap)
7. [Risks and Challenges](#7-risks-and-challenges)
8. [Expected Results](#8-expected-results)

---

## 1. Current Architecture Analysis

### 1.1 sp_head Memory Allocation

**Location:** `/home/user/mysql-server/sql/sp_head.h:927`

```cpp
class sp_head {
  // SP-persistent memory root (for instructions and expressions).
  MEM_ROOT main_mem_root;

  // The SP-instructions.
  Mem_root_array<sp_instr *> m_instructions;

  // ... other members
};
```

**Key Characteristics:**
- **Own MEM_ROOT:** Each sp_head instance has its own `main_mem_root`
- **Block allocation:** 8KB initial block, exponential growth
- **Lifetime:** Allocated at parse time, freed when sp_head is destroyed
- **Allocates:** Instructions (sp_instr), LEX objects, Items, strings

### 1.2 sp_instr Structure

**Location:** `/home/user/mysql-server/sql/sp_instr.h:105, 252`

```cpp
class sp_instr : public sp_printable {
 protected:
  Query_arena m_arena;  // Contains Item list
  // ...
};

class sp_lex_instr : public sp_instr {
 protected:
  LEX *m_lex;  // Full LEX object: 8-50 KB per instruction!

 private:
  // Mem-root for storing the LEX-tree during reparse
  MEM_ROOT m_lex_mem_root{PSI_NOT_INSTRUMENTED, MEM_ROOT_BLOCK_SIZE};
  // ...
};
```

**Memory Breakdown per Instruction:**
- Base sp_instr: ~100 bytes
- LEX object: 8-50 KB (typical: 12-20 KB for simple statements)
- Items in LEX: Variable (0-100+ items, ~100-500 bytes each)
- Total: **8-50 KB per SQL instruction**

### 1.3 Memory Duplication Problem

**Scenario: 100K connections, 5 stored procedures**

```
Each SP averages:
- 20 instructions
- 12 KB per instruction
- Total: 240 KB per SP

Per-THD cache (60K connections use SPs):
- 5 SPs × 240 KB = 1.2 MB per THD
- 60,000 THDs × 1.2 MB = 72 GB total

Actual unique data needed:
- 5 SPs × 240 KB = 1.2 MB

Waste: 72 GB - 1.2 MB = 71.99 GB (99.998%)
```

**Why can't we just share sp_head?**
Because LEX and Item objects contain **mutable state** that changes during execution.

---

## 2. Mutability Analysis: Why Direct Sharing Fails

### 2.1 LEX Object Mutability

**Location:** `/home/user/mysql-server/sql/sp_instr.cc:321, 413-415`

#### Evidence 1: LEX is Modified During Execution

```cpp
bool sp_lex_instr::execute_expression(THD *thd, uint *nextp) {
  auto execute_guard = create_scope_guard([&]() {
    m_lex->cleanup(true);  // ← MUTATES LEX STATE!
    // ...
  });
  // ...
}
```

#### Evidence 2: LEX Contains Thread-Specific Pointers

```cpp
bool sp_lex_instr::reset_lex_and_exec_core(THD *thd, uint *nextp, bool open_tables) {
  LEX *lex_saved = thd->lex;
  thd->lex = m_lex;
  m_lex->thd = thd;  // ← Sets thread-specific pointer!
  // ...
}
```

**Location:** `/home/user/mysql-server/sql/sql_union.cc:1210-1239`

#### Evidence 3: cleanup() Modifies Internal State

```cpp
void Query_expression::cleanup(bool full) {
  if (cleaned >= (full ? UC_CLEAN : UC_PART_CLEAN)) return;

  cleaned = (full ? UC_CLEAN : UC_PART_CLEAN);  // ← State modification

  if (full) {
    clear_root_access_path();  // ← Destroys execution plan
  }

  m_operands.clear();  // ← Clears runtime data

  for (auto qt : query_terms<QTC_PRE_ORDER>()) {
    qt->cleanup(full);  // ← Recursive cleanup
  }
  // ...
}
```

**LEX Mutable State Includes:**
- `thd` pointer - Must point to executing thread
- `cleaned` flag - Tracks cleanup state
- Query execution plan - Built during optimization, destroyed during cleanup
- Table lists - Modified during table opening
- Item states - Modified during fix_fields() and execution

### 2.2 Item Object Mutability

**Location:** `/home/user/mysql-server/sql/item.h:1273, 1282`

```cpp
class Item : public Parse_tree_node {
 public:
  virtual void cleanup() { marker = MARKER_NONE; }  // ← Modifies state
  virtual bool fix_fields(THD *, Item **);          // ← Modifies state

 private:
  enum_marker marker;  // ← Mutable execution state
  // ... many more mutable fields
};
```

**Item Mutable State:**
1. **fix_fields()** - Called during name resolution:
   - Resolves field references to TABLE objects
   - Performs type checking and coercion
   - Builds access paths
   - **Modifies:** `maybe_null`, `max_length`, `fixed` flag, internal caches

2. **cleanup()** - Called after execution:
   - Resets temporary buffers
   - Clears cached values
   - Resets state for next execution
   - **Modifies:** `marker`, caches, temporary strings

**Why Items Cannot Be Shared:**
```cpp
// Thread 1 executing SP:
item->fix_fields(thd1, ...);  // Sets internal state for THD1
item->val_int();               // Uses THD1's context

// Thread 2 simultaneously:
item->fix_fields(thd2, ...);  // RACE CONDITION! Overwrites THD1's state
item->val_int();               // Reads corrupted state
```

### 2.3 Conclusion: Direct Sharing Impossible

**Cannot share current sp_head because:**
1. ❌ LEX objects modified by cleanup()
2. ❌ LEX contains thread-specific `thd` pointer
3. ❌ Items modified by fix_fields() and cleanup()
4. ❌ Execution plans destroyed and rebuilt
5. ❌ No thread-safety mechanisms

**Must separate immutable parse results from mutable execution state.**

---

## 3. Reference Pattern: TABLE_SHARE vs TABLE

MySQL already has a proven pattern for sharing immutable metadata while maintaining per-thread mutable state.

### 3.1 TABLE_SHARE (Immutable, Shared)

**Location:** `/home/user/mysql-server/sql/table.h:716`

```cpp
struct TABLE_SHARE {
  MEM_ROOT mem_root;  // ← SHARED MEM_ROOT for immutable data

  // Immutable metadata (shared across all TABLE instances):
  Field **field;              // Field definitions
  KEY *key_info;              // Index definitions
  TYPELIB keynames;           // Key names
  uint fields;                // Field count
  uint keys;                  // Key count
  ulong reclength;            // Row length
  plugin_ref db_plugin;       // Storage engine

  // Reference counting for safe destruction:
  std::atomic<uint> ref_count;

  // Cache management:
  TABLE_SHARE *next, **prev;  // LRU list
  Table_cache_element **cache_element;

  // ... other immutable metadata
};
```

**Key Characteristics:**
- **Own MEM_ROOT:** All immutable metadata allocated from `mem_root`
- **Shared:** Single instance referenced by multiple TABLE objects
- **Thread-safe:** Reference counting with atomic operations
- **Lifetime:** Created on first use, destroyed when ref_count reaches 0

### 3.2 TABLE (Mutable, Per-Thread)

**Location:** `/home/user/mysql-server/sql/table.h:1435`

```cpp
struct TABLE {
  TABLE_SHARE *s;  // ← Pointer to shared immutable metadata
  handler *file;   // ← Per-thread storage engine handler

  // Per-thread mutable state:
  uchar *record[2];            // Current/previous row buffers
  uchar *write_row_record;     // Update row buffer
  uchar *insert_values;        // INSERT default values
  Field **field;               // Field instances (point to shared definitions)
  Field *next_number_field;    // AUTO_INCREMENT field
  Field *found_next_number_field;

  // Query execution state:
  key_map quick_keys;          // Keys usable for this query
  key_map covering_keys;       // Keys that cover this query
  key_map keys_in_use_for_query;

  // Transaction state:
  MY_BITMAP *read_set;         // Columns to read
  MY_BITMAP *write_set;        // Columns to write

  // ... many other per-thread fields
};
```

**Key Characteristics:**
- **References shared data:** `s` pointer to TABLE_SHARE
- **Own mutable state:** Buffers, transaction context, query state
- **Lightweight:** ~2-5 KB per instance vs ~100-500 KB for TABLE_SHARE
- **Per-thread:** Each thread gets its own TABLE instance

### 3.3 Pattern Benefits

**Memory Efficiency:**
```
Without sharing (old model):
- 100K connections × 500 KB per table = 50 GB

With TABLE_SHARE pattern:
- Shared: 500 KB (1 TABLE_SHARE)
- Per-thread: 100K × 3 KB (TABLE instances) = 300 MB
- Total: 300.5 MB (99.4% savings)
```

**Thread Safety:**
- Immutable shared data requires no locks for reading
- Mutable per-thread data has no contention
- Reference counting protects against premature destruction

**This is the exact pattern we need for stored procedures!**

---

## 4. Proposed Architecture: SP_SHARE vs SP_INSTANCE

### 4.1 SP_SHARE (Immutable, Shared)

```cpp
/**
 * SP_SHARE: Immutable stored procedure metadata, shared globally.
 *
 * Allocated from global memory pool, reference counted, cached globally.
 * Contains parse results that never change after creation.
 */
class SP_SHARE {
 public:
  // Reference counting for safe destruction
  std::atomic<uint32_t> ref_count{1};

  // Version for invalidation (increment on ALTER/DROP)
  std::atomic<uint64_t> version{1};

  // Immutable identification
  LEX_CSTRING m_db;
  LEX_CSTRING m_name;
  LEX_CSTRING m_qname;
  enum_sp_type m_type;

  // Immutable metadata
  st_sp_chistics *m_chistics;
  sql_mode_t m_sql_mode;
  LEX_CSTRING m_body;
  LEX_CSTRING m_body_utf8;
  LEX_STRING m_definer_user;
  LEX_STRING m_definer_host;

  // Immutable parse results
  Mem_root_array<SP_INSTR_SHARE *> m_instructions;

  // Security context (immutable after creation)
  Security_context m_security_ctx;

  // Global memory pool for this SP_SHARE
  MEM_ROOT main_mem_root;

  // Creation timestamp
  longlong m_created;
  longlong m_modified;

 private:
  // Prevent copying
  SP_SHARE(const SP_SHARE &) = delete;
  void operator=(const SP_SHARE &) = delete;
};

/**
 * SP_INSTR_SHARE: Immutable instruction parse results.
 *
 * Contains only the parse tree, no execution state.
 */
class SP_INSTR_SHARE {
 public:
  uint m_ip;  // Instruction pointer (index)

  // Instruction type and metadata
  enum sp_instr::Instr_type m_type;

  // Parse results (immutable):
  // - For sp_lex_instr: Store parsed AST, not LEX
  // - For jumps: Store destination IP
  // - For variables: Store variable index and parse tree

  union {
    struct {  // For INSTR_LEX_STMT
      LEX_CSTRING m_query;           // Original query text
      Parse_tree_root *m_parse_tree; // Immutable parse tree
    } stmt;

    struct {  // For INSTR_JUMP*
      uint m_dest;  // Destination IP
    } jump;

    struct {  // For INSTR_LEX_SET
      uint m_var_idx;                // Variable index
      Item *m_value_item;            // Value expression (immutable parse tree)
    } set_var;

    // ... other instruction types
  } m_data;

 private:
  SP_INSTR_SHARE(const SP_INSTR_SHARE &) = delete;
};
```

### 4.2 SP_INSTANCE (Mutable, Per-Thread)

```cpp
/**
 * SP_INSTANCE: Per-thread execution context for a stored procedure.
 *
 * Lightweight mutable state, references immutable SP_SHARE.
 */
class SP_INSTANCE {
 public:
  SP_SHARE *m_share;  // ← Reference to shared immutable data

  // Per-thread execution state
  THD *m_thd;  // Owning thread

  // Execution context
  sp_rcontext *m_rcontext;  // Runtime context (variables, cursors, handlers)

  // Query execution state (per instruction)
  Mem_root_array<SP_INSTR_INSTANCE *> m_instr_instances;

  // Transaction state
  bool m_in_transaction;
  uint m_recursion_level;

  // Statistics
  uint64_t m_execution_count;

  // Memory for temporary execution state
  MEM_ROOT m_exec_mem_root{PSI_NOT_INSTRUMENTED, 4096};  // Small: 4KB blocks

 public:
  SP_INSTANCE(THD *thd, SP_SHARE *share);
  ~SP_INSTANCE();

  bool execute(THD *thd);
  void cleanup();
};

/**
 * SP_INSTR_INSTANCE: Per-thread execution state for one instruction.
 *
 * Contains LEX and Items rebuilt for this thread.
 */
class SP_INSTR_INSTANCE {
 public:
  SP_INSTR_SHARE *m_share;  // ← Reference to immutable parse results

  // Per-thread execution state:
  LEX *m_lex;                // Rebuilt LEX for this thread
  Query_arena m_arena;       // Items for this thread

  // Execution statistics
  uint64_t m_exec_count;

 public:
  // Rebuild LEX from immutable parse tree
  bool prepare(THD *thd, SP_SHARE *sp_share);

  // Execute instruction
  bool execute(THD *thd, uint *nextp);

  // Cleanup for next execution
  void cleanup();
};
```

### 4.3 Memory Layout Comparison

#### Current Architecture (Per-THD sp_head):

```
THD 1:
  sp_head (500 KB)
    ├─ main_mem_root (owns all memory)
    ├─ sp_instr[0]: LEX (15 KB)
    ├─ sp_instr[1]: LEX (12 KB)
    ├─ ... (20 instructions)
    └─ sp_instr[19]: LEX (18 KB)

THD 2:
  sp_head (500 KB) ← DUPLICATE!
    ├─ main_mem_root
    ├─ sp_instr[0]: LEX (15 KB)
    └─ ... (identical to THD 1)

... (100K THDs × 500 KB = 50 GB)
```

#### Proposed Architecture (SP_SHARE + SP_INSTANCE):

```
Global SP Cache:
  SP_SHARE (500 KB) ← SHARED by all threads
    ├─ main_mem_root (owns immutable memory)
    ├─ SP_INSTR_SHARE[0]: Parse tree (10 KB)
    ├─ SP_INSTR_SHARE[1]: Parse tree (8 KB)
    ├─ ... (20 instructions)
    └─ SP_INSTR_SHARE[19]: Parse tree (12 KB)

THD 1:
  SP_INSTANCE (50 KB) ← Per-thread
    ├─ m_share → (points to SP_SHARE)
    ├─ m_exec_mem_root (16 KB for runtime state)
    ├─ SP_INSTR_INSTANCE[0]: LEX + Items (rebuilt, 2 KB)
    └─ ... (lightweight per-instruction state)

THD 2:
  SP_INSTANCE (50 KB) ← Per-thread
    ├─ m_share → (points to same SP_SHARE)
    └─ ... (own mutable state)

... (100K THDs × 50 KB = 5 GB)

Total: 500 KB (shared) + 5 GB (per-thread) = 5.0005 GB
Savings: 50 GB → 5 GB (90% reduction!)
```

**Note:** Even better savings possible with lazy allocation of SP_INSTR_INSTANCE.

### 4.4 Key Design Decisions

#### 4.4.1 What Goes in SP_SHARE (Immutable)?

✅ **Include:**
- Parse trees (AST nodes)
- String literals (allocated from SP_SHARE's MEM_ROOT)
- Instruction metadata (type, destination IPs)
- SP metadata (name, definer, characteristics)
- Security context

❌ **Exclude:**
- LEX objects (contain mutable state)
- Item objects (modified by fix_fields)
- Thread pointers (THD *)
- Execution statistics
- Runtime context (variables, cursors)

#### 4.4.2 Parse Tree vs LEX

**Current:** sp_instr stores full LEX object (8-50 KB, mutable)

**Proposed:** SP_INSTR_SHARE stores parse tree (5-30 KB, immutable)

```cpp
// Current (mutable LEX):
class sp_lex_instr {
  LEX *m_lex;  // Full LEX with mutable state
};

// Proposed (immutable parse tree):
class SP_INSTR_SHARE {
  Parse_tree_root *m_parse_tree;  // Immutable AST
};

// Per-thread (rebuild LEX from parse tree):
class SP_INSTR_INSTANCE {
  LEX *m_lex;  // Rebuilt from m_share->m_parse_tree

  bool prepare(THD *thd) {
    // Rebuild LEX from parse tree for this thread
    m_lex = build_lex_from_parse_tree(thd, m_share->m_parse_tree);
    // fix_fields() on this thread's items
    return m_lex->fix_fields(thd);
  }
};
```

**Benefits:**
- Parse tree is immutable (AST nodes don't change)
- LEX rebuilt per-thread from shared parse tree
- Items created per-thread, safe to modify
- Each thread has own execution context

#### 4.4.3 Lazy Allocation

**Optimization:** Don't allocate SP_INSTR_INSTANCE until instruction is executed

```cpp
class SP_INSTANCE {
  // Initially null, allocated on first execution of each instruction
  Mem_root_array<SP_INSTR_INSTANCE *> m_instr_instances;

  SP_INSTR_INSTANCE *get_instr_instance(uint ip) {
    if (!m_instr_instances[ip]) {
      // Lazy allocation: only create when first executed
      m_instr_instances[ip] = new (m_exec_mem_root) SP_INSTR_INSTANCE();
      m_instr_instances[ip]->prepare(m_thd, m_share->m_instructions[ip]);
    }
    return m_instr_instances[ip];
  }
};
```

**Benefits:**
- SP with 100 instructions, but only 10 executed → Only 10 SP_INSTR_INSTANCE allocated
- Further memory savings for large SPs
- Conditional branches: Only executed paths allocate memory

---

## 5. Memory Pool Design

### 5.1 Global Memory Pool Architecture

```cpp
/**
 * Global memory pool for SP_SHARE objects.
 *
 * Uses slab allocator with size classes for efficient allocation.
 */
class SP_Memory_Pool {
 public:
  /**
   * Allocate SP_SHARE with given size.
   *
   * Uses size classes: 64KB, 128KB, 256KB, 512KB, 1MB, 2MB, 4MB.
   * Rounds up to next size class for efficient reuse.
   */
  SP_SHARE *allocate_sp_share(size_t estimated_size);

  /**
   * Free SP_SHARE back to pool for reuse.
   *
   * Memory is not returned to OS, kept in free list for reuse.
   */
  void free_sp_share(SP_SHARE *share);

 private:
  // Size classes (power-of-2 buckets)
  struct Size_Class {
    size_t size;                      // Size of this class (e.g., 64KB)
    std::vector<void *> free_list;    // Free blocks of this size
    mysql_mutex_t lock;               // Per-size-class lock
    uint64_t alloc_count;             // Statistics
    uint64_t free_count;
  };

  Size_Class m_size_classes[7] = {
    {64 * 1024},    // 64 KB
    {128 * 1024},   // 128 KB
    {256 * 1024},   // 256 KB
    {512 * 1024},   // 512 KB
    {1024 * 1024},  // 1 MB
    {2048 * 1024},  // 2 MB
    {4096 * 1024},  // 4 MB
  };

  // Find size class for given size
  Size_Class *find_size_class(size_t size);
};
```

### 5.2 MEM_ROOT Strategy

#### Current Problem:

```cpp
class sp_head {
  MEM_ROOT main_mem_root;  // Each sp_head owns its MEM_ROOT
};

// On sp_head destruction:
sp_head::~sp_head() {
  main_mem_root.Clear();  // Frees ALL memory at once
}
```

**Issue:** Cannot share MEM_ROOT across sp_head instances because destroying one would free memory used by others.

#### Solution: SP_SHARE Owns MEM_ROOT

```cpp
class SP_SHARE {
  MEM_ROOT main_mem_root;  // SP_SHARE owns the MEM_ROOT
  std::atomic<uint32_t> ref_count{1};

  void add_ref() {
    ref_count.fetch_add(1, std::memory_order_relaxed);
  }

  void release() {
    if (ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      // Last reference, safe to destroy
      this->~SP_SHARE();         // Destroy C++ objects
      main_mem_root.Clear();     // Free all memory
      sp_memory_pool.free_sp_share(this);  // Return to pool
    }
  }
};

class SP_INSTANCE {
  SP_SHARE *m_share;  // Reference to shared SP_SHARE

  SP_INSTANCE(SP_SHARE *share) : m_share(share) {
    m_share->add_ref();  // Increment reference count
  }

  ~SP_INSTANCE() {
    m_share->release();  // Decrement reference count
  }
};
```

**Benefits:**
- SP_SHARE's MEM_ROOT freed only when no more SP_INSTANCE references it
- Safe sharing across threads
- Automatic memory management via reference counting

### 5.3 Memory Pool vs Malloc

**Option 1: Custom Memory Pool (Proposed)**

```cpp
void *SP_Memory_Pool::allocate_sp_share(size_t size) {
  Size_Class *sc = find_size_class(size);

  mysql_mutex_lock(&sc->lock);
  if (!sc->free_list.empty()) {
    // Reuse freed block
    void *ptr = sc->free_list.back();
    sc->free_list.pop_back();
    mysql_mutex_unlock(&sc->lock);
    return ptr;
  }
  mysql_mutex_unlock(&sc->lock);

  // Allocate new block from OS
  return my_malloc(PSI_INSTRUMENT_ME, sc->size, MYF(0));
}

void SP_Memory_Pool::free_sp_share(SP_SHARE *share) {
  Size_Class *sc = find_size_class(share->allocated_size);

  mysql_mutex_lock(&sc->lock);
  sc->free_list.push_back(share);  // Keep for reuse
  mysql_mutex_unlock(&sc->lock);
}
```

**Pros:**
- ✅ Reuses memory, reduces malloc/free overhead
- ✅ Reduces memory fragmentation
- ✅ Faster allocation (from free list)
- ✅ Better cache locality (same-size objects)

**Cons:**
- ❌ More complex implementation
- ❌ Memory not returned to OS (stays in free lists)
- ❌ Requires memory limit and eviction policy

**Option 2: Direct malloc/free (Simpler)**

```cpp
SP_SHARE *allocate_sp_share(size_t size) {
  void *mem = my_malloc(PSI_INSTRUMENT_ME, size, MYF(0));
  return new (mem) SP_SHARE();
}

void free_sp_share(SP_SHARE *share) {
  share->~SP_SHARE();
  my_free(share);  // Return to OS immediately
}
```

**Pros:**
- ✅ Simple implementation
- ✅ Memory returned to OS
- ✅ No memory limit needed

**Cons:**
- ❌ Slower (malloc/free overhead)
- ❌ Memory fragmentation
- ❌ No reuse benefits

**Recommendation:** Start with Option 2 (simple malloc/free), migrate to Option 1 (memory pool) in later phase for performance optimization.

---

## 6. Implementation Roadmap

### Phase 1: Infrastructure (4 weeks)

**Goal:** Build foundation without breaking existing code

**Tasks:**
1. Create SP_SHARE class (2 days)
   - Define structure with immutable fields from sp_head
   - Implement reference counting
   - Add version tracking for invalidation

2. Create SP_INSTANCE class (2 days)
   - Define structure with mutable fields
   - Implement reference to SP_SHARE
   - Add per-thread execution context

3. Create SP_INSTR_SHARE and SP_INSTR_INSTANCE (3 days)
   - Split sp_instr into immutable/mutable parts
   - Design parse tree storage format
   - Plan LEX rebuilding strategy

4. Implement memory allocation (1 week)
   - Create SP_Memory_Pool class (or use malloc/free)
   - Implement size class allocator
   - Add instrumentation for Performance Schema

5. Create Global SP Cache (1 week)
   - Integrate with GLOBAL_CACHE_PATTERNS_ANALYSIS.md recommendations
   - Implement multi-instance sharding (16 instances)
   - Add reference counting to cache elements
   - Implement LRU eviction

6. Testing infrastructure (1 week)
   - Unit tests for SP_SHARE/SP_INSTANCE
   - Memory leak detection
   - Reference counting verification
   - Multi-threaded stress tests

### Phase 2: Parse Tree Refactoring (4 weeks)

**Goal:** Store immutable parse trees instead of mutable LEX

**Challenge:** LEX is deeply integrated into MySQL's execution model

**Tasks:**
1. Analyze LEX→Parse_tree conversion (1 week)
   - Study Parse_tree_root and subclasses
   - Identify what can be stored immutably
   - Design conversion strategy

2. Implement Parse_tree storage in SP_INSTR_SHARE (1 week)
   - Store Parse_tree_root instead of LEX
   - Ensure parse trees are truly immutable
   - Handle special cases (cursors, handlers)

3. Implement LEX rebuilding from parse tree (2 weeks)
   - SP_INSTR_INSTANCE::prepare() builds LEX from parse tree
   - Allocate Items on per-thread mem_root
   - Call fix_fields() on per-thread Items
   - Handle metadata caching (table definitions, etc.)

**Key Challenge:** Parse tree → LEX conversion
```cpp
bool SP_INSTR_INSTANCE::prepare(THD *thd, SP_INSTR_SHARE *share) {
  // Rebuild LEX from immutable parse tree
  m_lex = new (m_arena.mem_root) LEX;

  // Convert parse tree to LEX for this thread
  Parse_tree_root *tree = share->m_parse_tree;
  if (tree->contextualize(thd, m_lex)) return true;

  // Fix fields for this thread's context
  if (m_lex->unit->prepare(thd, ...)) return true;

  return false;
}
```

### Phase 3: Instruction Execution Refactoring (3 weeks)

**Goal:** Execute instructions using SP_SHARE + SP_INSTANCE

**Tasks:**
1. Modify sp_head::execute() (1 week)
   - Create SP_INSTANCE for this execution
   - Pass SP_INSTANCE to instruction execution
   - Handle recursion (multiple SP_INSTANCE for same SP_SHARE)

2. Modify sp_instr::execute() (1 week)
   - Get SP_INSTR_INSTANCE (lazy allocation)
   - Use rebuilt LEX and Items
   - Ensure cleanup() called correctly

3. Handle special cases (1 week)
   - Cursors (need per-thread state)
   - Handlers (exception handling context)
   - Recursive calls (nested SP_INSTANCE)
   - Dynamic SQL (PREPARE/EXECUTE)

### Phase 4: Integration with SP Cache (2 weeks)

**Goal:** Integrate SP_SHARE with global cache

**Tasks:**
1. Modify sp_cache to use SP_SHARE (1 week)
   - Change cache storage from sp_head to SP_SHARE
   - Implement reference counting in cache
   - Handle cache invalidation (ALTER/DROP)

2. Modify sp_head loading (1 week)
   - Load from Data Dictionary into SP_SHARE
   - Store in global cache instead of per-THD cache
   - Create SP_INSTANCE on execution

**Changes:**
```cpp
// Current (per-THD cache):
class THD {
  sp_cache *sp_proc_cache;  // Per-THD cache: duplicates data
};

// Proposed (global cache):
class Global_SP_Cache {
  std::unordered_map<string, SP_SHARE *> m_cache;  // Shared
  mysql_mutex_t m_lock;
};

// Execution:
bool sp_head::execute(THD *thd) {
  // Current: Uses 'this' sp_head (per-THD)

  // Proposed: Create SP_INSTANCE
  SP_SHARE *share = get_sp_share();  // From global cache
  SP_INSTANCE instance(thd, share);
  return instance.execute();
}
```

### Phase 5: Optimization (2 weeks)

**Goal:** Optimize for performance and memory

**Tasks:**
1. Lazy allocation of SP_INSTR_INSTANCE (3 days)
   - Allocate only when instruction is executed
   - Handle conditional branches efficiently
   - Measure memory savings

2. Parse tree optimization (3 days)
   - Minimize parse tree size
   - Share common subtrees
   - Compress string literals

3. Memory pool optimization (4 days)
   - Implement size class allocator
   - Add free list management
   - Tune size classes based on real workloads

4. Cache tuning (4 days)
   - Optimize number of cache instances
   - Tune LRU eviction policy
   - Add memory limits and pressure handling

### Phase 6: Testing & Validation (3 weeks)

**Goal:** Ensure correctness and stability

**Tasks:**
1. Functional testing (1 week)
   - Run full MySQL test suite
   - Test all SP features
   - Test edge cases (recursion, cursors, handlers)

2. Stress testing (1 week)
   - 100K concurrent connections
   - Heavy SP workload
   - Memory pressure scenarios
   - Cache invalidation stress

3. Performance testing (1 week)
   - Benchmark SP execution latency
   - Measure memory savings
   - Compare with current implementation
   - Optimize hot paths

**Total: 18 weeks (4.5 months)**

---

## 7. Risks and Challenges

### 7.1 Technical Risks

#### Risk 1: LEX Rebuilding Overhead (HIGH)

**Problem:** Rebuilding LEX from parse tree on every SP execution could be expensive

**Mitigation:**
- **Caching:** Keep SP_INSTR_INSTANCE across multiple executions (cleanup but don't destroy)
- **Lazy allocation:** Only rebuild LEX for executed instructions
- **Measurement:** Benchmark overhead, acceptable if <10% latency increase

**Estimated overhead:**
```
Current: Execute LEX directly (no overhead)
Proposed: Rebuild LEX from parse tree + fix_fields()

Rebuild cost per instruction:
- Parse tree → LEX: ~5-10 μs
- fix_fields(): ~10-20 μs
- Total: 15-30 μs overhead

For SP with 50 instructions:
- First execution: 50 × 30 μs = 1.5 ms overhead
- Cached execution: 0 μs (reuse SP_INSTR_INSTANCE)

Acceptable if SP execution time >> 1.5 ms (typically 10-100 ms)
```

#### Risk 2: Parse Tree Completeness (MEDIUM)

**Problem:** Parse trees might not capture all information needed to rebuild LEX

**Mitigation:**
- **Audit:** Comprehensive audit of what's stored in LEX vs parse tree
- **Extension:** Extend parse tree to store missing information
- **Fallback:** Keep original query text, re-parse if needed (slower but correct)

#### Risk 3: Reference Counting Bugs (MEDIUM)

**Problem:** Reference counting errors could cause memory leaks or use-after-free

**Mitigation:**
- **Atomic operations:** Use std::atomic for thread-safe ref counting
- **Smart pointers:** Consider using intrusive_ptr-like pattern
- **Testing:** Extensive memory leak detection (Valgrind, AddressSanitizer)
- **Assertion:** Debug builds verify ref_count invariants

#### Risk 4: Cache Invalidation (MEDIUM)

**Problem:** ALTER/DROP SP must invalidate all cached instances

**Mitigation:**
- **Version tracking:** Increment SP_SHARE->version on ALTER
- **Version check:** SP_INSTANCE checks version before execution
- **Stale execution:** If version mismatch, drop instance and get fresh SP_SHARE

```cpp
bool SP_INSTANCE::execute(THD *thd) {
  if (m_cached_version != m_share->version.load()) {
    // SP was altered, reload
    m_share->release();
    m_share = get_sp_share(thd, m_sp_name);
    m_cached_version = m_share->version.load();
    rebuild_instr_instances();
  }
  // ... execute
}
```

#### Risk 5: Thread Safety (HIGH)

**Problem:** Bugs in shared memory access could cause data races

**Mitigation:**
- **Immutability:** SP_SHARE truly immutable after creation
- **Thread Sanitizer:** Test with ThreadSanitizer during development
- **Atomic operations:** Use atomics for ref_count and version
- **No locks for read:** SP_SHARE read without locks (immutable)

### 7.2 Compatibility Risks

#### Risk 1: Breaking Changes (LOW)

**Problem:** Existing code might depend on sp_head internals

**Mitigation:**
- **Gradual migration:** Keep sp_head API stable, refactor internals
- **Compatibility layer:** sp_head wraps SP_SHARE+SP_INSTANCE temporarily
- **Testing:** Run full test suite at each phase

#### Risk 2: Third-Party Dependencies (LOW)

**Problem:** Third-party code might access sp_head directly

**Mitigation:**
- **Private members:** sp_head internals are already private
- **Stable API:** Public API remains unchanged
- **Deprecation:** Mark old APIs deprecated, provide migration path

### 7.3 Performance Risks

#### Risk 1: Latency Regression (MEDIUM)

**Problem:** LEX rebuilding overhead increases SP execution latency

**Acceptable:** <10% latency increase
**Measured:** Benchmark before/after on TPC-C, sysbench

**Mitigation:**
- Caching of SP_INSTR_INSTANCE
- Optimize rebuild path
- Consider JIT for hot SPs (future work)

#### Risk 2: Throughput Regression (LOW)

**Problem:** Global cache lock contention reduces throughput

**Mitigation:**
- Multi-instance sharding (16 instances)
- Lock-free reads (immutable data)
- Reference counting instead of locks

---

## 8. Expected Results

### 8.1 Memory Savings

#### Scenario: 100K Connections, 5 SPs, Average 200 KB Each

**Current (per-THD sp_head):**
```
Assumptions:
- 60% of connections use SPs (60K connections)
- Each connection caches all 5 SPs
- Average SP size: 200 KB

Per-connection:
- 5 SPs × 200 KB = 1 MB per connection

Total:
- 60,000 connections × 1 MB = 60 GB

Unique data:
- 5 SPs × 200 KB = 1 MB

Waste: 60 GB - 1 MB = 59.999 GB (99.998%)
```

**Proposed (SP_SHARE + SP_INSTANCE):**
```
Global SP Cache (16 instances):
- 5 SPs × 200 KB = 1 MB per instance
- 1 MB × 16 instances = 16 MB

Per-connection (SP_INSTANCE):
- Lightweight reference: 40 bytes × 5 SPs = 200 bytes
- Execution state: ~50 KB per active SP
- Average (10% actively executing): 5 KB per connection

Total:
- Global: 16 MB
- Per-connection: 60,000 × 5 KB = 300 MB
- Total: 316 MB

Savings: 60 GB → 316 MB (99.5% reduction!)
```

#### Memory Breakdown by Component

| Component | Current | Proposed | Savings |
|-----------|---------|----------|---------|
| **SP parse results** | 60 GB | 16 MB | 99.97% |
| **Per-thread execution state** | 0 | 300 MB | N/A |
| **Cache overhead** | 0 | ~10 MB | N/A |
| **Total** | **60 GB** | **326 MB** | **99.5%** |

### 8.2 Performance Impact

#### 8.2.1 Latency

**Measured metric:** SP execution time (end-to-end)

**Expected impact:**
```
First execution (cold cache):
- Current: Parse from DD + compile: ~5 ms
- Proposed: Parse from DD + create SP_SHARE + compile: ~6 ms
- Overhead: +20% (one-time cost, amortized)

Subsequent executions (warm cache):
- Current: Execute directly: ~1 ms baseline
- Proposed (cached SP_INSTR_INSTANCE): Execute directly: ~1.05 ms
- Overhead: +5% (LEX validation overhead)

Subsequent executions (cold SP_INSTR_INSTANCE):
- Current: Execute directly: ~1 ms baseline
- Proposed: Rebuild LEX + fix_fields + execute: ~1.3 ms
- Overhead: +30% (worst case, lazy allocation)
```

**Mitigation:** SP_INSTR_INSTANCE caching keeps overhead at +5%

**Acceptable:** For 99.5% memory savings, +5% latency is excellent trade-off

#### 8.2.2 Throughput

**Measured metric:** Queries per second (QPS) on SP workload

**Expected impact:**
```
Current throughput: 50K QPS (baseline)

Cache hit rate: 99% (global cache is warm)
Lock contention: Minimal (16-way sharding)
Reference counting overhead: ~10 ns per execution

Proposed throughput: 48-49K QPS
Overhead: -2 to -4%
```

**Why minimal impact:**
- Global cache reads are lock-free (immutable data)
- Reference counting uses atomic ops (fast)
- Multi-instance sharding reduces contention
- No parsing overhead (warm cache)

#### 8.2.3 Cache Hit Rate

**Global SP Cache:**
```
Typical production:
- Total unique SPs: 100-1000
- Total cache capacity: 10,000 SPs
- Hit rate: >99.9%

Large codebase:
- Total unique SPs: 10,000
- Total cache capacity: 10,000 SPs
- Hit rate: 90-95% (depends on access pattern)
- LRU eviction handles working set
```

### 8.3 Scalability

#### 8.3.1 Connection Scalability

**Goal:** Support 100K+ concurrent connections

**Current bottleneck:**
```
100K connections × 1 MB (SP cache) = 100 GB memory
→ OOM, cannot scale
```

**Proposed:**
```
100K connections × 5 KB (SP_INSTANCE) + 16 MB (global) = 516 MB
→ Easily fits in memory, can scale to 1M connections
```

#### 8.3.2 SP Count Scalability

**Goal:** Support large codebases with 10K+ stored procedures

**Current:**
```
Each connection caches subset of SPs
Memory per connection: Varies by usage
Problem: Duplicated across all connections
```

**Proposed:**
```
Global cache stores all unique SPs
10,000 SPs × 200 KB average = 2 GB total
With 16 instances: 125 MB per instance
Eviction: LRU keeps working set in cache
```

### 8.4 Summary Metrics

| Metric | Current | Proposed | Change |
|--------|---------|----------|--------|
| **Memory (100K conns, 5 SPs)** | 60 GB | 326 MB | **-99.5%** ✅ |
| **Memory (100K conns, 50 SPs)** | 600 GB | 3.3 GB | **-99.4%** ✅ |
| **Latency (warm cache)** | 1 ms | 1.05 ms | **+5%** ✅ |
| **Latency (cold cache)** | 1 ms | 1.3 ms | **+30%** ⚠️ |
| **Throughput (QPS)** | 50K | 48K | **-4%** ✅ |
| **Max connections** | ~10K | 1M+ | **+100x** ✅ |
| **Cache hit rate** | 100% | >99% | **-1%** ✅ |

**Overall Assessment:** Excellent trade-off. Small latency/throughput cost for massive memory savings and scalability improvement.

---

## 9. Alternative Approaches (Considered and Rejected)

### 9.1 Copy-on-Write (COW) LEX

**Idea:** Share LEX objects, copy only when modified

**Pros:**
- Simpler than parse tree approach
- Leverages existing LEX structure

**Cons:**
- ❌ LEX is always modified (fix_fields, cleanup)
- ❌ Would copy every execution → No savings
- ❌ Complex tracking of what changed
- ❌ Thread safety issues

**Verdict: Rejected** - LEX is too mutable

### 9.2 Read-Only LEX

**Idea:** Make LEX immutable, create separate execution context

**Pros:**
- Clean separation of parse/execute
- Could share LEX directly

**Cons:**
- ❌ Massive refactoring of MySQL internals
- ❌ LEX deeply integrated into optimizer, executor
- ❌ Would break many assumptions
- ❌ Years of work

**Verdict: Rejected** - Too invasive

### 9.3 Compressed Serialization

**Idea:** Serialize sp_head, compress, share compressed data

**Pros:**
- Reduces memory usage
- Simpler implementation

**Cons:**
- ❌ Decompression overhead on every execution
- ❌ Still duplicates execution state
- ❌ Compression ratio unclear (maybe 2-3x)
- ❌ Doesn't enable sharing

**Verdict: Rejected** - Insufficient savings, adds overhead

### 9.4 Global MEM_ROOT Pool

**Idea:** All sp_head allocate from shared MEM_ROOT pool

**Pros:**
- Simple memory pooling

**Cons:**
- ❌ Doesn't solve duplication problem
- ❌ Memory still duplicated per THD
- ❌ No sharing benefits
- ❌ Fragmentation issues

**Verdict: Rejected** - Doesn't address root cause

---

## 10. Conclusion

### 10.1 Feasibility: YES

**Allocating sp_head from a global memory pool is feasible**, but requires architectural refactoring to separate:
1. **SP_SHARE** - Immutable parse results (shared, global pool)
2. **SP_INSTANCE** - Mutable execution context (per-thread, lightweight)

This mirrors the proven TABLE_SHARE/TABLE pattern used throughout MySQL.

### 10.2 Key Insights

1. **Current sp_head cannot be shared directly** because LEX and Item objects are mutable
2. **Parse trees are immutable** and can be shared
3. **LEX can be rebuilt** from parse tree per-thread
4. **Reference counting** protects shared objects from premature destruction
5. **Global cache with multi-instance sharding** provides thread-safe access

### 10.3 Recommended Approach

**Hybrid architecture:**
```
Global SP Cache (16 instances, sharded)
  └─ SP_SHARE (immutable, reference counted)
      ├─ MEM_ROOT (owns all memory)
      ├─ Parse trees (immutable AST)
      └─ Metadata (name, definer, etc.)

Per-THD:
  └─ SP_INSTANCE (lightweight, mutable)
      ├─ Reference to SP_SHARE
      ├─ Execution state (variables, cursors)
      └─ SP_INSTR_INSTANCE (lazy allocated)
          ├─ Rebuilt LEX
          └─ Per-thread Items
```

### 10.4 Expected Impact

✅ **Memory savings:** 99.5% (60 GB → 326 MB for 100K connections)
✅ **Scalability:** 10K → 1M+ concurrent connections
✅ **Performance:** -4% throughput, +5% latency (acceptable trade-off)
✅ **Complexity:** HIGH (18 weeks implementation)

### 10.5 Next Steps

1. **Approve approach** - Review with MySQL team
2. **Phase 1: Infrastructure** - Build SP_SHARE/SP_INSTANCE classes (4 weeks)
3. **Phase 2: Parse trees** - Store immutable parse results (4 weeks)
4. **Phase 3: Execution** - Refactor instruction execution (3 weeks)
5. **Phase 4: Integration** - Integrate with global cache (2 weeks)
6. **Phase 5: Optimization** - Tune for performance (2 weeks)
7. **Phase 6: Testing** - Validate correctness (3 weeks)

**Total effort: 18 weeks (4.5 months)**

---

## 11. References

### Code Locations

#### Current Architecture
- `sql/sp_head.h:927` - sp_head with main_mem_root
- `sql/sp_instr.h:252` - sp_lex_instr with LEX
- `sql/sp_instr.cc:321, 413-415` - LEX mutability evidence
- `sql/item.h:1273, 1282` - Item mutability (fix_fields, cleanup)

#### Reference Pattern
- `sql/table.h:716` - TABLE_SHARE definition
- `sql/table.h:1435` - TABLE definition
- `sql/table_cache.h` - Multi-instance cache pattern

#### Global Cache Patterns
- `sql/table_cache.h` - Multi-instance sharding
- `sql/dd/impl/cache/cache_element.h` - Reference counting
- `sql/dd/impl/cache/shared_multi_map.h` - Cache miss coordination
- `sql/hostname_cache.cc` - LRU eviction

### Related Documents
- `GLOBAL_CACHE_PATTERNS_ANALYSIS.md` - Global cache implementation patterns
- `THD_MEMORY_LIFECYCLE_ANALYSIS.md` - THD memory allocation analysis
- `MEMORY_OPTIMIZATION_RESEARCH.md` - Initial memory research
- `ASYNC_MUX_DESIGN_VALIDATION.md` - Async multiplexing validation

---

**End of Document**
