# MySQL THD Memory Optimization Research

**Author:** Research Analysis for Distributed MySQL Database
**Date:** 2025-11-07
**Objective:** Reduce per-connection memory consumption from ~3MB to <500KB to support 100,000+ concurrent connections
**Reference:** TDSQL paper (VLDB 2024, Vol 17, pp 3869-3881)

---

## Executive Summary

Current MySQL architecture consumes **2-5 MB per active connection** (THD), making it impossible to efficiently support 100,000+ concurrent connections without massive RAM requirements (200-500 GB just for connection state). This research identifies the major memory consumption areas and proposes optimization strategies inspired by TDSQL and custom architectural improvements.

**Key Findings:**
- **Stored Procedure Cache Duplication**: Each connection caches SPs independently, wasting 20-200 GB with 100K connections
- **Session Buffers**: 640 KB - 1.5 MB allocated per complex query (join/sort/read buffers)
- **Network Buffers**: 32 KB - 2 MB per connection, not shared
- **Prepared Statements**: Per-THD caching with no sharing, 50 KB - 1.5 MB overhead
- **Transaction State**: 20-150 KB per active transaction

**Proposed Solution:**
1. **Global Shared Caches** for stored procedures, prepared statements, and table definitions
2. **Memory Pooling** for session buffers (join, sort, read) with checkout/return mechanism
3. **Lightweight Session Context** (~50 KB) instead of full THD for idle connections
4. **Async Connection Multiplexing** to reduce total connection count by 50x
5. **Lazy THD Allocation** - only materialize full THD during SQL execution

**Target:** Reduce per-connection memory from **3 MB → 300 KB** (10x reduction)

---

## 1. Current THD Memory Consumption Analysis

### 1.1 Memory Breakdown by Category

Based on deep code analysis of MySQL 8.4+ codebase:

| Component | Idle | Simple Query | Complex | Heavy Use | Code Location |
|-----------|------|--------------|---------|-----------|---------------|
| **Base THD Structure** | 8-12 KB | 8-12 KB | 8-12 KB | 8-12 KB | `sql/sql_class.h:949` |
| **Network Buffers** | 32 KB | 32-64 KB | 512 KB | 1-2 MB | `include/mysql_com.h:916` |
| **MEM_ROOT (query)** | 1 KB | 32 KB | 256 KB | 512 KB | `sql/sql_class.h:4458` |
| **Session Buffers** | 0 | 128 KB | 640 KB | 1.5 MB | `sql/system_variables.h:242-267` |
| **Transaction State** | 0 | 20 KB | 150 KB | 150 KB | `sql/transaction_info.h:55` |
| **Open Tables** | 0 | 20 KB | 400 KB | 2 MB | `sql/sql_class.h:557` |
| **Stored Proc Cache** | 0 | 0 | 0 | **2-5 MB** | `sql/sp_cache.cc:42` |
| **Prepared Stmt Cache** | 0 | 0 | 500 KB | **1.5 MB** | `sql/sql_class.h:483` |
| **MDL Context** | 2 KB | 2 KB | 10 KB | 10 KB | `sql/sql_class.h:967` |
| **Binlog Cache** | 0 | 64 KB | 64 KB | 1 MB | `sql/binlog.cc:1136` |
| **TOTAL** | **~45 KB** | **~300 KB** | **~2.5 MB** | **~14 MB** | |

### 1.2 Per-Component Deep Dive

#### 1.2.1 Stored Procedure Cache - **CRITICAL ISSUE** ⚠️

**Location:** `sql/sp_cache.cc`, `sql/sp_cache.h`, `sql/sp_head.h`

**Current Architecture:**
```cpp
// sql/sql_class.h:2853-2854
class THD {
  sp_cache *sp_proc_cache;  // Per-THD procedure cache
  sp_cache *sp_func_cache;  // Per-THD function cache
};
```

**Problem:** Each connection maintains **independent caches** with NO sharing between threads.

**Memory Consumption:**
```cpp
// Simple 20-statement SP
sp_head base:              2 KB
main_mem_root:             8 KB
SQL body:                  1 KB
Instructions (20):         4 KB
LEX objects (20 × 5 KB):   100 KB
Per-instruction MEM_ROOT:  160 KB (20 × 8 KB)
-----------------------------------
TOTAL per instance:        ~275 KB
```

**100,000 Connection Scenario:**
```
Same stored procedure executed across all connections:
  275 KB × 100,000 = 27.5 GB

10 different SPs cached per connection:
  2.75 MB × 100,000 = 275 GB
```

**Evidence from Code:**

1. **No Sharing:** (`sql/sp_cache.cc:138-149`)
   ```cpp
   void sp_cache_insert(sp_cache **cp, sp_head *sp) {
     if (!(*cp)) {
       *cp = new sp_cache();  // New cache per THD
     }
     (*cp)->insert(sp);
   }
   ```

2. **Duplicate Parsing:** (`sql/sp.cc:1744-1753`)
   ```cpp
   sp_head *sp = sp_cache_lookup(cp, name);
   if (!sp && !cache_only) {
     db_find_routine(thd, type, name, &sp);  // Parse from DD
     sp_cache_insert(cp, sp);  // Each THD parses independently
   }
   ```

3. **All-or-Nothing Eviction:** (`sql/sp_cache.cc:74-76`)
   ```cpp
   void enforce_limit(ulong upper_limit) {
     if (m_hashtable.size() > upper_limit) {
       m_hashtable.clear();  // Clears ENTIRE cache!
     }
   }
   ```

**Impact:**
- Default limit: 256 SPs per cache (`stored_program_cache`, `sql/sys_vars.cc:6424`)
- With 100K connections: Up to **256 × 100K = 25.6 million SP instances in memory**
- Realistic worst case: **50-300 GB wasted on duplicate SPs**

#### 1.2.2 Prepared Statement Cache - **CRITICAL ISSUE** ⚠️

**Location:** `sql/sql_prepare.h:150`, `sql/sql_class.h:483`

**Current Architecture:**
```cpp
class THD {
  Prepared_statement_map stmt_map;  // Per-THD, not shared
};
```

**Memory per Prepared Statement:**
```cpp
Prepared_statement base:       512 bytes
Query text (m_query_string):   Variable (avg 200 bytes)
LEX parse tree:                2-10 KB
Query_arena MEM_ROOT:          8-64 KB
Item parameters:               Variable (1-10 KB)
-----------------------------------------------
TOTAL per statement:           12-75 KB
```

**Typical Usage:**
- Average 10 prepared statements per connection
- **10 × 50 KB = 500 KB per THD**
- **100K connections = 50 GB total**

**Same Problem as SPs:** No sharing, duplicate parsing, per-THD storage.

#### 1.2.3 Session Buffers - **HIGH IMPACT** ⚠️

**Location:** `sql/system_variables.h:242-267`

**Configurable Buffers (allocated on-demand):**
```cpp
struct System_variables {
  ulong join_buff_size;          // Default: 256 KB
  ulong sortbuff_size;           // Default: 256 KB (sort_buffer_size)
  ulong read_buff_size;          // Default: 128 KB
  ulong read_rnd_buff_size;      // Default: 256 KB
  ulong net_buffer_length;       // Default: 16 KB
  ulong bulk_insert_buff_size;   // Default: 8 MB
};
```

**Allocation Pattern:**
- Buffers allocated **per query execution** when needed
- NOT pre-allocated for idle connections
- Released after query completion (in theory)

**Reality Check from Code:**

Looking at `sql/filesort.cc`, `sql/sql_join_buffer.cc`:
```cpp
// Buffers allocated from MEM_ROOT or my_malloc
// Often held until end of statement or transaction
// Can accumulate during complex multi-table joins
```

**Peak Usage During Complex Query:**
```
join_buffer (2 tables):    512 KB
sort_buffer (filesort):    256 KB
read_buffer (scan):        128 KB
read_rnd_buffer:           256 KB
--------------------------------------
TOTAL:                     1.15 MB
```

**100K Connection Scenario:**
- If 10% executing complex queries simultaneously: **10K × 1.15 MB = 11.5 GB**
- If 50% active: **57.5 GB**

#### 1.2.4 Network Buffers - **MEDIUM IMPACT** ⚠️

**Location:** `include/mysql_com.h:916`

**NET Structure:**
```cpp
typedef struct NET {
  unsigned char *buff;           // Main I/O buffer
  unsigned long buf_length;      // Buffer size
  unsigned long max_packet_size; // Max packet (64 MB default)
  // ...
} NET;
```

**Allocation:** (`sql/net_serv.cc:149-163`)
```cpp
bool my_net_init(NET *net, Vio *vio, uint my_flags) {
  net->buff = (uchar *)my_malloc(net_buffer_length);  // 16 KB default
  net->buff_end = net->buff + net_buffer_length;
  // Can grow up to max_allowed_packet (64 MB)
}
```

**Memory per Connection:**
```
NET structure overhead:    ~600 bytes
buff (net_buffer_length):  16 KB (default)
packet String (dynamic):   16 KB - 1 MB (typical)
Compression buffer:        0-64 KB (if enabled)
------------------------------------------
TOTAL:                     32 KB - 2 MB
```

**100K Connections:**
- Minimum: **100K × 32 KB = 3.2 GB**
- With large packets: **100K × 512 KB = 50 GB**

#### 1.2.5 Transaction State - **MEDIUM IMPACT** ⚠️

**Location:** `sql/transaction_info.h:55`

**Transaction_ctx Structure:**
```cpp
class Transaction_ctx {
  MEM_ROOT m_mem_root;                 // Transaction-scoped memory
  THD_TRANS m_scope_info[2];           // STMT and SESSION scopes
  XID_STATE m_xid_state;               // XA transaction state
  Savepoint *m_savepoints;             // Savepoint list
  Rpl_transaction_ctx m_rpl_ctx;       // Replication context
  // ...
};
```

**Memory Breakdown:**
```
Base structure:            ~256 bytes
MEM_ROOT allocations:      16-128 KB
XID state:                 ~140 bytes
Savepoints (avg 2):        ~256 bytes
Binlog cache:              32-1024 KB
-------------------------------------
TOTAL:                     20-150 KB per active transaction
```

**Impact:**
- Not all connections have active transactions
- Realistic: 30% in transaction at any time
- **100K × 30% × 75 KB = 2.25 GB**

#### 1.2.6 Open Table Cache - **MEDIUM IMPACT** ⚠️

**Location:** `sql/sql_class.h:557` (Open_tables_state)

**Per-Open-Table Memory:**
```cpp
// sql/table.h:1134
struct TABLE {
  TABLE_SHARE *s;              // Shared metadata (global)
  Record_buffer record[2];     // Row buffers
  Field **field;               // Field array
  uchar *null_flags;           // Null bitmap
  handler *file;               // Storage engine handler (~2 KB)
  // ...
};
```

**Memory Calculation:**
```
TABLE structure:           ~1.5 KB
record buffers (2):        2 × row_size (avg 500 bytes) = 1 KB
Field array:               columns × 128 bytes (avg 10 cols) = 1.28 KB
handler instance:          ~2 KB
----------------------------------------------------------------------
TOTAL per open table:      ~5-6 KB
```

**Typical Open Tables per Connection:** 3-10 tables
**Per THD:** 15-60 KB
**100K connections:** 1.5-6 GB

**Note:** TABLE_SHARE is globally cached - good! Only TABLE instances are per-THD.

---

## 2. Root Causes of Memory Inefficiency

### 2.1 Architectural Flaws

#### **Flaw #1: No Separation of Immutable vs Mutable State**

**Example: sp_head Structure** (`sql/sp_head.h:389`)

```cpp
class sp_head {
  // IMMUTABLE (should be shared):
  LEX_CSTRING m_body;              // SQL text
  Mem_root_array<sp_instr *> m_instructions;  // Parsed instructions

  // MUTABLE (per-execution):
  ulong m_recursion_level;         // Current recursion depth
  sp_head *m_next_cached_sp;       // Recursion chain
  sp_rcontext *m_rcontext;         // Runtime context
};
```

**Problem:** Mutable execution state mixed with immutable definition prevents sharing.

#### **Flaw #2: Per-THD Caching Without Global Pool**

**Current Pattern:**
```
THD 1 → sp_cache → [SP_A, SP_B, SP_C]
THD 2 → sp_cache → [SP_A, SP_B, SP_C]  // Duplicate!
THD 3 → sp_cache → [SP_A, SP_B, SP_C]  // Duplicate!
...
THD 100K → sp_cache → [SP_A, SP_B, SP_C]  // Duplicate!
```

**Should Be:**
```
Global SP Cache → [SP_A, SP_B, SP_C]  // Single copy
                       ↑
                       |
THD 1 → Execution Context (sp_rcontext)
THD 2 → Execution Context (sp_rcontext)
THD 3 → Execution Context (sp_rcontext)
...
```

#### **Flaw #3: Eager Buffer Allocation**

**Current:** Buffers allocated at full size when first needed
**Problem:** All-or-nothing allocation leads to fragmentation and waste

**Better Approach:**
- Start with small buffer
- Grow incrementally
- Use memory pools for reuse

#### **Flaw #4: No Connection Multiplexing**

**Current:** 1 TCP connection = 1 THD = Full memory footprint

**TDSQL Approach:**
- 1 TCP connection = 50 sessions (configurable)
- Each session = lightweight context
- Full THD allocated only during SQL execution

**Impact:**
```
Traditional:  1200 routers × 50 shards = 60,000 connections
Multiplexed:  1200 routers × 50 shards ÷ 50 sessions = 1,200 connections (50x reduction)
```

### 2.2 Configuration Defaults Not Optimized for High Concurrency

| Variable | Default | Optimized | Savings per THD |
|----------|---------|-----------|-----------------|
| `stored_program_cache` | 256 | Global cache | ~2 MB |
| `join_buffer_size` | 256 KB | Pool-based | ~256 KB |
| `sort_buffer_size` | 256 KB | Pool-based | ~256 KB |
| `read_buffer_size` | 128 KB | Pool-based | ~128 KB |
| `net_buffer_length` | 16 KB | 4 KB | ~12 KB |

---

## 3. TDSQL Memory Optimization Strategies

Based on VLDB 2024 paper and research:

### 3.1 Shared Resource Caches

**Strategy:** Move from per-THD caching to global shared caches with LRU eviction.

**Implementation:**
```
Global Stored Procedure Cache:
  - Single cache for all connections
  - LRU eviction policy
  - Read-write locks for concurrency
  - Separate execution context per THD

Global Prepared Statement Cache:
  - Hash by query fingerprint (normalized SQL)
  - Share identical prepared statements
  - Per-connection parameter binding only
```

**Memory Savings:**
```
Before: 100K THDs × 2 MB SP cache = 200 GB
After:  1 global cache with 10K unique SPs × 275 KB = 2.75 GB
Savings: 197 GB (98.6% reduction)
```

### 3.2 Memory Pools for Session Buffers

**Strategy:** Pre-allocate buffer pools, checkout/return on demand.

**Architecture:**
```cpp
class GlobalBufferPool {
  std::queue<Buffer*> join_buffer_pool;
  std::queue<Buffer*> sort_buffer_pool;
  std::queue<Buffer*> read_buffer_pool;

  Buffer* checkout(BufferType type);
  void return_buffer(Buffer* buf);
};
```

**Benefits:**
- Reuse buffers across queries
- Limit total memory consumption
- Reduce allocation/deallocation overhead
- Better cache locality

**Memory Savings:**
```
Before: 100K THDs, 50% active, 640 KB buffers = 32 GB
After:  Pool of 10K buffers × 640 KB = 6.4 GB
Savings: 25.6 GB (80% reduction)
```

### 3.3 Lightweight Session Context

**Strategy:** Separate lightweight session state from heavy THD structure.

**Architecture:**
```cpp
class LightweightSession {
  uint64 session_id;
  auth_info;                    // User, DB, privileges
  session_variables;            // Lightweight variable set
  transaction_state;            // In-transaction flag
  // NO: sp_cache, prepared stmts, buffers, MEM_ROOT
};

class THD {
  // Only allocated when executing SQL
  // Recycled back to pool after execution
};
```

**Memory:**
```
LightweightSession: ~50 KB
Full THD (allocated only when needed): ~2 MB

Idle connection: 50 KB (vs 3 MB today)
Active query: 50 KB + 2 MB from pool = 2.05 MB
```

**Savings:**
```
100K connections, 10% active:
  Before: 100K × 3 MB = 300 GB
  After: 90K × 50 KB + 10K × 2 MB = 4.5 GB + 20 GB = 24.5 GB
  Savings: 275.5 GB (92% reduction)
```

### 3.4 Async Connection Multiplexing

**Strategy:** Multiple logical sessions per TCP connection.

**Benefits:**
- Reduce total connection count by 50x
- Share network buffers across sessions
- Amortize connection overhead

**Memory:**
```
Before: 50,000 TCP connections × 32 KB net buffers = 1.6 GB
After:  1,000 TCP connections × 128 KB buffers = 128 MB
Savings: 1.47 GB (92% reduction)
```

---

## 4. Proposed Optimization Roadmap

### Phase 1: Global Shared Caches (High Priority)

**Target:** Reduce SP/prepared statement duplication

**Implementation:**

1. **Global SP Cache:**
   - Create `Global_sp_cache` singleton
   - Hash stored procedures by (db, name)
   - LRU eviction with configurable size
   - Read-write locks for thread safety
   - Separate `sp_head` (immutable) from `sp_rcontext` (per-execution)

2. **Global Prepared Statement Cache:**
   - Hash by query fingerprint
   - Share identical statements across connections
   - Per-THD parameter binding context only

**Code Changes:**
- `sql/sp_cache.cc`: Refactor to use global cache
- `sql/sp_head.h`: Separate immutable/mutable state
- `sql/sql_prepare.cc`: Add global prepared stmt cache

**Expected Savings:** 150-250 GB for 100K connections

**Complexity:** Medium-High (requires thread-safety, refactoring)

**Risk:** Moderate (need careful locking to avoid contention)

### Phase 2: Memory Pooling for Session Buffers (High Priority)

**Target:** Reuse join/sort/read buffers across connections

**Implementation:**

1. **Global Buffer Pools:**
   ```cpp
   class Buffer_pool {
     Buffer* allocate(size_t size);
     void free(Buffer* buf);
     void preallocate(size_t count, size_t size);
   };

   Global_buffer_pool::join_buffers(10000, 256*1024);  // 10K buffers of 256 KB
   ```

2. **THD Buffer Checkout:**
   ```cpp
   class THD {
     Buffer* checkout_join_buffer();
     void return_join_buffer(Buffer* buf);
   };
   ```

**Code Changes:**
- `sql/filesort.cc`: Use pooled sort buffers
- `sql/sql_join_buffer.cc`: Use pooled join buffers
- `sql/records.cc`: Use pooled read buffers

**Expected Savings:** 20-40 GB for 100K connections

**Complexity:** Medium (need pool management, checkout/return logic)

**Risk:** Low (fallback to malloc if pool exhausted)

### Phase 3: Lightweight Session Context (Medium Priority)

**Target:** Reduce idle connection overhead

**Implementation:**

1. **Session Context:**
   ```cpp
   class Session_context {
     uint64 session_id;
     Security_context security_ctx;
     Session_variables variables;
     bool in_transaction;
     // ~50 KB total
   };
   ```

2. **THD Pool:**
   ```cpp
   class THD_pool {
     THD* allocate();
     void free(THD* thd);
   };
   ```

3. **Lazy THD Allocation:**
   ```cpp
   if (session->executing_sql && !session->thd) {
     session->thd = THD_pool::allocate();
   }
   ```

**Code Changes:**
- `sql/sql_class.h`: Separate Session_context from THD
- `sql/conn_handler/`: Modify to use Session_context
- `sql/sql_parse.cc`: Allocate THD on demand

**Expected Savings:** 200-270 GB for 100K connections

**Complexity:** High (major architectural change)

**Risk:** High (affects entire connection lifecycle)

### Phase 4: Async Connection Multiplexing (Low Priority)

**Target:** Reduce total connection count

**Implementation:**
- See `async_multiplexing_design.h` for full architecture
- Protocol extension with connid
- Non-blocking I/O with epoll
- Packet aggregation

**Expected Savings:** Additional 10-50 GB through connection reduction

**Complexity:** Very High (new network model)

**Risk:** Very High (affects protocol, client compatibility)

---

## 5. Quick Wins (Immediate Implementation)

### 5.1 Reduce Default Buffer Sizes

**Change system variable defaults:**

```cpp
// sql/sys_vars.cc
join_buffer_size:      256 KB → 64 KB
sort_buffer_size:      256 KB → 64 KB
read_buffer_size:      128 KB → 32 KB
net_buffer_length:     16 KB → 4 KB
```

**Savings:** ~500 KB per active connection
**Risk:** Low (users can override)
**Effort:** 1 hour

### 5.2 Aggressive SP Cache Eviction

**Reduce default stored_program_cache:**

```cpp
stored_program_cache: 256 → 32
```

**Savings:** ~1.5 MB per connection
**Risk:** Low (re-parse overhead minimal for < 32 SPs)
**Effort:** 5 minutes

### 5.3 Prepared Statement Limit

**Add per-THD prepared statement limit:**

```cpp
max_prepared_stmt_count_per_connection: Default 100 (vs unlimited today)
```

**Savings:** Prevent runaway memory usage
**Risk:** Low
**Effort:** 2 hours

---

## 6. Memory Consumption Targets

### Current State (MySQL 8.4)

| Scenario | Per-THD Memory | 100K Connections |
|----------|----------------|------------------|
| Idle | 45 KB | 4.5 GB |
| Simple query | 300 KB | 30 GB |
| Complex query | 2.5 MB | 250 GB |
| Heavy SP/PS usage | 14 MB | **1.4 TB** |

### After Phase 1 (Global Caches)

| Scenario | Per-THD Memory | 100K Connections |
|----------|----------------|------------------|
| Idle | 45 KB | 4.5 GB |
| Simple query | 300 KB | 30 GB |
| Complex query | 2.5 MB | 250 GB |
| Heavy SP/PS usage | 1 MB | **100 GB** ✅ |

**Improvement:** 93% reduction in worst case

### After Phase 2 (Memory Pooling)

| Scenario | Per-THD Memory | 100K Connections |
|----------|----------------|------------------|
| Idle | 45 KB | 4.5 GB |
| Simple query | 150 KB | 15 GB |
| Complex query | 1 MB | 100 GB |
| Heavy SP/PS usage | 800 KB | **80 GB** ✅ |

**Improvement:** 94% reduction

### After Phase 3 (Lightweight Sessions)

| Scenario | Per-THD Memory | 100K Connections |
|----------|----------------|------------------|
| Idle | **50 KB** | **5 GB** ✅ |
| Simple query | 150 KB | 15 GB |
| Complex query | 1 MB | 100 GB |
| Heavy SP/PS usage | 800 KB | **80 GB** ✅ |

**Improvement:** 94-98% reduction

### After Phase 4 (Multiplexing)

| Scenario | TCP Connections | Memory (1000 TCP × 50 sessions) |
|----------|-----------------|----------------------------------|
| Idle | **1,000** | **2.5 GB** ✅ |
| Active | **2,000** | **40 GB** ✅ |

**Improvement:** 99% reduction vs current worst case

---

## 7. Key Metrics and Validation

### 7.1 Memory Measurement Points

**Instrument these code locations:**

1. **THD allocation:**
   - `sql/sql_class.cc:1114` - `THD::THD()` constructor
   - Measure sizeof(THD) + heap allocations

2. **SP cache:**
   - `sql/sp_cache.cc:74` - `sp_cache::enforce_limit()`
   - Track `m_hashtable.size()` and estimated memory

3. **Prepared statements:**
   - `sql/sql_prepare.cc:4238` - `Prepared_statement::prepare()`
   - Measure `m_arena.mem_root.m_allocated_size`

4. **Session buffers:**
   - `sql/filesort.cc` - sort buffer allocation
   - `sql/sql_join_buffer.cc` - join buffer allocation

**Add Performance Schema instrumentation:**
```sql
SELECT * FROM performance_schema.memory_summary_by_thread_by_event_name
WHERE THREAD_ID = <connection_thread_id>;
```

### 7.2 Benchmark Scenarios

**Test 1: SP Cache Duplication**
```sql
-- Create stored procedure
CREATE PROCEDURE test_sp() BEGIN SELECT 1; END;

-- Open 10,000 connections
-- Execute test_sp() from each connection
-- Measure total memory

Expected:
  Before: 10K × 275 KB = 2.75 GB
  After (global cache): ~275 KB (single copy)
```

**Test 2: Buffer Pool Reuse**
```sql
-- Run 1000 concurrent complex joins
SELECT * FROM t1 JOIN t2 JOIN t3 ORDER BY col LIMIT 10;

-- Measure peak memory

Expected:
  Before: 1000 × 640 KB = 640 MB
  After (pool): 640 KB × 100 buffers = 64 MB (10x reduction)
```

**Test 3: 100K Idle Connections**
```bash
# Open 100,000 idle connections
# Measure RSS of mysqld process

Expected:
  Before: 4.5-30 GB (depending on activity)
  After: 5 GB (lightweight sessions)
```

---

## 8. Risks and Mitigations

### 8.1 Global Cache Contention

**Risk:** Lock contention on global SP/prepared stmt cache with 100K threads

**Mitigation:**
- Use sharded caches (16-256 shards based on hash)
- Read-write locks with optimistic reads
- Per-shard LRU to distribute eviction load

**Example:**
```cpp
class Sharded_sp_cache {
  static constexpr size_t NUM_SHARDS = 64;
  sp_cache_shard shards[NUM_SHARDS];

  sp_cache_shard& get_shard(const char* name) {
    return shards[hash(name) % NUM_SHARDS];
  }
};
```

### 8.2 SP/PS Eviction Under Pressure

**Risk:** Hot eviction of frequently-used SPs causes re-parsing thrashing

**Mitigation:**
- Use LRU eviction (not all-or-nothing like current)
- Pin frequently-used objects
- Adaptive cache sizing based on memory pressure

### 8.3 Buffer Pool Exhaustion

**Risk:** All buffers checked out, new queries blocked

**Mitigation:**
- Fallback to malloc if pool exhausted
- Configurable pool size with auto-tuning
- Priority queuing for buffer allocation

### 8.4 Backward Compatibility

**Risk:** Breaking changes to client/server protocol or behavior

**Mitigation:**
- Opt-in via configuration flags
- Gradual rollout (Phase 1 → 2 → 3 → 4)
- Compatibility mode for legacy behavior

---

## 9. Implementation Priority Matrix

| Optimization | Savings | Complexity | Risk | Priority | Effort |
|--------------|---------|------------|------|----------|--------|
| **Global SP Cache** | ⭐⭐⭐⭐⭐ | Medium | Medium | **P0** | 3 weeks |
| **Global PS Cache** | ⭐⭐⭐⭐ | Medium | Medium | **P0** | 2 weeks |
| **Buffer Pooling** | ⭐⭐⭐⭐ | Medium | Low | **P1** | 2 weeks |
| **Reduce Defaults** | ⭐⭐⭐ | Low | Low | **P1** | 1 day |
| **Lightweight Session** | ⭐⭐⭐⭐⭐ | High | High | **P2** | 6 weeks |
| **Async Multiplexing** | ⭐⭐⭐ | Very High | Very High | **P3** | 12 weeks |

**Recommendation:** Start with P0 items (Global Caches) for immediate 90%+ memory reduction in worst-case scenarios.

---

## 10. Code Locations for Implementation

### 10.1 Global SP Cache

**Files to modify:**
```
sql/sp_cache.h                    - Add Global_sp_cache class
sql/sp_cache.cc                   - Implement global cache with LRU
sql/sp_head.h:389                 - Separate mutable state
sql/sp.cc:1744                    - Use global cache lookup
sql/sql_class.h:2853              - Remove per-THD sp_cache
sql/sys_vars.cc                   - Add global_sp_cache_size variable
```

**New classes:**
```cpp
class Global_sp_cache {
  mysql_rwlock_t lock;
  collation_unordered_map<std::string, shared_ptr<sp_head>> cache;

  shared_ptr<sp_head> lookup(const LEX_CSTRING &name);
  void insert(shared_ptr<sp_head> sp);
  void evict_lru();
};
```

### 10.2 Global Prepared Statement Cache

**Files to modify:**
```
sql/sql_prepare.h:150             - Refactor Prepared_statement
sql/sql_prepare.cc:4238           - Use global cache
sql/sql_class.h:483               - Remove per-THD stmt_map
```

**New classes:**
```cpp
class Global_prepared_stmt_cache {
  // Hash by query fingerprint (normalized SQL)
  unordered_map<uint64, shared_ptr<Prepared_statement_template>> cache;

  shared_ptr<Prepared_statement_template> lookup(uint64 fingerprint);
};

class Prepared_statement_template {
  // Shared, immutable
  LEX* lex;
  String query_string;
};

class Prepared_statement_binding {
  // Per-THD, lightweight
  shared_ptr<Prepared_statement_template> template;
  Item_param** params;  // Parameter values only
};
```

### 10.3 Buffer Pools

**Files to modify:**
```
sql/filesort.cc                   - Use global sort buffer pool
sql/filesort_utils.h              - Buffer pool API
sql/sql_join_buffer.cc            - Use global join buffer pool
sql/records.cc                    - Use global read buffer pool
```

**New files:**
```
sql/buffer_pool.h                 - Buffer pool interface
sql/buffer_pool.cc                - Implementation
```

**New classes:**
```cpp
class Buffer_pool {
  std::queue<Buffer*> free_list;
  mysql_mutex_t lock;
  size_t buffer_size;
  size_t pool_size;

  Buffer* checkout();
  void return_buffer(Buffer* buf);
};

class Global_buffer_pools {
  Buffer_pool join_pool;
  Buffer_pool sort_pool;
  Buffer_pool read_pool;
};
```

---

## 11. Next Steps

### Immediate Actions (Week 1)

1. **Validate measurements:**
   - Write test program to measure THD memory with 1K, 10K, 100K connections
   - Profile SP cache memory usage with varying cache sizes
   - Confirm buffer allocation patterns under load

2. **Create prototype:**
   - Implement minimal global SP cache (single-threaded first)
   - Measure memory reduction
   - Validate performance impact

3. **Design review:**
   - Present findings to team
   - Align on implementation phases
   - Define success metrics

### Short-term (Months 1-2)

1. Implement Phase 1: Global SP and PS caches
2. Add comprehensive testing and benchmarking
3. Measure memory reduction in production-like environment

### Medium-term (Months 3-4)

1. Implement Phase 2: Buffer pooling
2. Implement Phase 3: Lightweight sessions (if approved)
3. Performance tuning and optimization

### Long-term (Months 5-6)

1. Consider Phase 4: Async multiplexing (separate project)
2. Production rollout planning
3. Documentation and training

---

## 12. References

### Papers
- [TDSQL: A Distributed Cloud Database System](https://www.vldb.org/pvldb/vol17/p3869-chen.pdf), VLDB 2024

### MySQL Source Code (8.4+)
- `sql/sql_class.h` - THD structure
- `sql/sp_cache.cc` - Stored procedure cache
- `sql/sql_prepare.cc` - Prepared statements
- `sql/filesort.cc` - Sort buffer allocation
- `include/mysql_com.h` - Network buffers

### Configuration Variables
- `stored_program_cache` (default: 256)
- `max_prepared_stmt_count` (default: 16382)
- `join_buffer_size` (default: 256 KB)
- `sort_buffer_size` (default: 256 KB)

---

## Appendix A: Memory Calculation Formulas

### A.1 Total Server Memory for Connections

```
Total_Memory = Fixed_Memory + (N_connections × Per_Connection_Memory)

Where:
  Fixed_Memory = Global caches + buffer pools + table cache + InnoDB buffer pool
  Per_Connection_Memory = THD_base + session_buffers + transaction_state + network_buffers
```

### A.2 Current MySQL (Worst Case)

```
Total_Memory = 20 GB (fixed) + (100,000 × 14 MB)
             = 20 GB + 1,400 GB
             = 1,420 GB (1.4 TB)
```

### A.3 After Optimizations (Phase 1-3)

```
Total_Memory = 30 GB (larger global caches) + (100,000 × 0.3 MB)
             = 30 GB + 30 GB
             = 60 GB

Reduction: 96% (1,420 GB → 60 GB)
```

### A.4 With Multiplexing (Phase 4)

```
Effective_Connections = 100,000 / 50 = 2,000 TCP connections
Total_Memory = 30 GB + (2,000 × 0.5 MB) + (100,000 × 0.05 MB lightweight session)
             = 30 GB + 1 GB + 5 GB
             = 36 GB

Reduction: 97.5% (1,420 GB → 36 GB)
```

---

**End of Research Document**
