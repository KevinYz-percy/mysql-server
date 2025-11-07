# Global Cache Implementations in MySQL: Reference Patterns for SP Cache

**Analysis Date:** 2025-11-07
**Purpose:** Evaluate existing global cache patterns in MySQL to guide Global SP Cache implementation
**Related:** THD_MEMORY_LIFECYCLE_ANALYSIS.md, MEMORY_OPTIMIZATION_RESEARCH.md

---

## Executive Summary

MySQL contains **7 major global cache implementations** that can serve as reference patterns for implementing a Global SP Cache. The most relevant patterns are:

1. **Table Cache** - Multi-instance sharding for reduced contention
2. **DD Cache** - Reference counting with LRU eviction
3. **Hostname Cache** - Simple LRU list + hash map pattern

**Recommended Approach:** Hybrid model combining Table Cache sharding (for concurrency) with DD Cache reference counting (for safety) and Hostname Cache LRU management (for eviction).

**Key Insight:** MySQL already has proven patterns for:
- Thread-safe global caching
- LRU eviction policies
- Reference counting for safe deletion
- Cache miss coordination
- Version-based invalidation

---

## Table of Contents

1. [Table Cache / TABLE_SHARE Cache](#1-table-cache--table_share-cache)
2. [Data Dictionary (DD) Cache](#2-data-dictionary-dd-cache)
3. [Hostname Cache](#3-hostname-cache)
4. [ACL/User Cache](#4-acluser-cache)
5. [Plugin Cache](#5-plugin-cache)
6. [InnoDB Buffer Pool LRU](#6-innodb-buffer-pool-lru)
7. [Current SP Cache (Thread-Local)](#7-current-sp-cache-thread-local)
8. [Comparison Matrix](#8-comparison-matrix)
9. [Recommended Architecture for Global SP Cache](#9-recommended-architecture-for-global-sp-cache)
10. [Implementation Roadmap](#10-implementation-roadmap)

---

## 1. Table Cache / TABLE_SHARE Cache

### Location
- **Files:** `/home/user/mysql-server/sql/table_cache.h`, `/home/user/mysql-server/sql/table_cache.cc`
- **Manager:** `Table_cache_manager` class
- **Instance:** `Table_cache` class

### Architecture: Multi-Instance Sharding

**Key Innovation:** Uses multiple cache instances to reduce lock contention

```cpp
class Table_cache_manager {
  Table_cache **m_table_cache;  // Array of cache instances
  uint m_table_cache_instances; // Default: 16, Max: 64
};
```

**Thread Assignment:**
```cpp
// sql/table_cache.cc:184-186
uint Table_cache_manager::cache_id(uint64 id) const {
  return (uint)(id % m_table_cache_instances);
}
```

### Data Structures

**Per-Instance Storage:**
```cpp
class Table_cache {
  // Primary storage: Hash map
  std::unordered_map<std::string, std::unique_ptr<Table_cache_element>> m_cache;

  // LRU tracking
  TABLE *m_unused_tables;         // Head of unused list
  TABLE *m_unused_tables_triggers;  // Separate list for trigger tables

  // Limits
  uint m_table_cache_size;
  uint m_table_cache_triggers_size;

  // Lock
  mysql_mutex_t m_lock;
};
```

**Cache Element Structure:**
```cpp
class Table_cache_element {
  TABLE_SHARE *share;           // Shared metadata (global)
  TABLE *used_tables;           // In-use tables (linked list)
  TABLE *free_tables_slim;      // Free tables without triggers
  TABLE *free_tables_full_triggers;  // Free tables with triggers
};
```

### Locking Strategy

**Per-Instance Mutex:**
- Each `Table_cache` has its own `mysql_mutex_t m_lock`
- Threads only contend on their assigned cache instance
- **Benefit:** N instances = N-way parallelism

**Global LOCK_open:**
- Required only for TABLE creation/destruction
- Not for cache lookups/releases

**Lock Scope:**
```cpp
// sql/table_cache.cc:431-433
mysql_mutex_lock(&m_lock);
// ... cache operation ...
mysql_mutex_unlock(&m_lock);
```

### Eviction Policy

**Size-Based LRU:**
```cpp
// sql/table_cache.cc:261-282
void Table_cache::free_unused_tables_if_necessary(THD *thd) {
  while (m_cache.size() > m_table_cache_size && m_unused_tables) {
    TABLE *table = m_unused_tables;  // LRU head
    remove_table(table);
    intern_close_table(table);
  }
}
```

**Two-Tier Limits:**
- `table_cache_size_per_instance`: Main cache limit
- `table_cache_triggers_per_instance`: Separate for trigger tables

**LRU Management:**
- Unused tables linked in LRU order
- New unused table added to tail
- Eviction from head (oldest)

### Key Operations

```cpp
// Get table from cache
TABLE *Table_cache::get_table(THD *thd, const char *key,
                               size_t key_length, TABLE_SHARE **share);

// Release table back to cache
void Table_cache::release_table(THD *thd, TABLE *table);

// Add newly-opened table
bool Table_cache::add_used_table(THD *thd, TABLE *table);
```

### Applicability to SP Cache

**✅ Highly Applicable:**
- Multi-instance sharding would work perfectly for SPs
- Per-instance mutex reduces contention
- LRU eviction pattern directly usable

**Adaptation:**
- Replace `Table_cache_element` with `SP_cache_element`
- Store `sp_head*` instead of `TABLE*`
- Keep same sharding and locking strategy

---

## 2. Data Dictionary (DD) Cache

### Location
- **Files:** `/home/user/mysql-server/sql/dd/impl/cache/`
- **Main Class:** `Shared_dictionary_cache`
- **Element:** `Cache_element<T>`
- **Free List:** `Free_list<Cache_element<T>>`

### Architecture: Shared Global + Local Registry

**Two-Level Caching:**
```cpp
// Per-THD local registry
class Dictionary_client {
  Object_registry m_registry_uncommitted;  // Local cache
  Object_registry m_registry_committed;    // Local cache
  // References to global cache
};

// Global shared cache
class Shared_dictionary_cache {
  Shared_multi_map<Abstract_table> m_table_map;
  Shared_multi_map<Schema> m_schema_map;
  Shared_multi_map<Tablespace> m_tablespace_map;
  // ... one map per DD object type
};
```

### Data Structures

**Cache Element with Reference Counting:**
```cpp
template <typename T>
class Cache_element {
  const T *m_object;             // Cached object (immutable!)
  std::atomic<uint> m_ref_counter;  // Reference count
  Cache_element<T> *m_next;      // Free list pointer
  Cache_element<T> *m_prev;      // Free list pointer
};
```

**Multi-Key Maps:**
```cpp
template <typename T>
class Shared_multi_map {
  // Multiple indexes for fast lookup
  Element_map<Id_key, T> m_map_id;
  Element_map<Name_key, T> m_map_name;
  Element_map<Aux_key, T> m_map_aux;
  Element_map<const T*, T> m_rev_map;

  // LRU management
  Free_list<Cache_element<T>> m_free_list;

  // Element pool (reuse pattern)
  std::vector<Cache_element<T>*> m_element_pool;

  // Synchronization
  mysql_mutex_t m_lock;
  mysql_cond_t m_miss_handled;
};
```

### Locking Strategy

**Per-Map Mutex:**
```cpp
// sql/dd/impl/cache/shared_multi_map.cc:85-87
mysql_mutex_lock(&m_lock);
// ... operation ...
mysql_mutex_unlock(&m_lock);
```

**Autolocker Pattern (RAII):**
```cpp
class Autolocker {
  mysql_mutex_t *m_lock;
public:
  Autolocker(mysql_mutex_t *lock) : m_lock(lock) {
    mysql_mutex_lock(m_lock);
  }
  ~Autolocker() {
    mysql_mutex_unlock(m_lock);
  }
};
```

**Cache Miss Coordination:**
```cpp
// sql/dd/impl/cache/shared_multi_map.cc:184-207
template <typename K, typename T>
bool Shared_multi_map<T>::get(const K &key, Cache_element<T> **element) {
  Autolocker lock(&m_lock);

  // Check if already in cache
  if (m_map.get(key, element)) {
    (*element)->m_ref_counter++;
    return false;  // Hit
  }

  // Check if another thread is loading
  if (m_map.is_missed(key)) {
    // Wait for other thread
    while (m_map.is_missed(key)) {
      mysql_cond_wait(&m_miss_handled, &m_lock);
    }
    // Retry lookup
    return get(key, element);
  }

  // Mark as missed (we'll load it)
  m_map.set_missed(key);
  return true;  // Miss - caller should load
}
```

### Eviction Policy

**Capacity-Based LRU:**
```cpp
template <typename T>
void Shared_multi_map<T>::rectify_free_list(THD *thd) {
  // Evict if over capacity
  while (m_map.size() > m_capacity && m_free_list.length() > 0) {
    Cache_element<T> *e = m_free_list.get_lru();

    // Only evict if not in use
    if (e->m_ref_counter == 0) {
      remove(e->object());
      delete e->object();
    }
  }
}
```

**Reference Counting:**
- Cannot evict if `ref_counter > 0`
- Elements in use are protected
- Safe deletion after release

**Element Pool:**
```cpp
// Reuse cache elements (memory efficiency)
Cache_element<T> *Shared_multi_map<T>::create_new_element() {
  if (!m_element_pool.empty()) {
    Cache_element<T> *e = m_element_pool.back();
    m_element_pool.pop_back();
    return e;  // Reuse
  }
  return new Cache_element<T>();  // Allocate new
}
```

### Key Operations

```cpp
// Get from cache
bool get(const K &key, Cache_element<T> **element);

// Put new object
void put(const T *object, Cache_element<T> **element);

// Release (decrement ref count, add to free list)
void release(Cache_element<T> *element);

// Remove (evict from cache)
void remove(const T *object);
```

### Applicability to SP Cache

**✅ Extremely Applicable:**
- Reference counting critical for safe SP deletion
- Cache miss coordination prevents duplicate parsing
- Element pool reduces allocation overhead
- LRU with capacity limit is exactly what we need

**Direct Reuse:**
- `Cache_element<sp_head>` with reference counting
- `Free_list<Cache_element<sp_head>>` for LRU
- Same put/get/release pattern

---

## 3. Hostname Cache

### Location
- **File:** `/home/user/mysql-server/sql/hostname_cache.cc`
- **Variables:** `hostname_cache_lru`, `hostname_cache_hash`

### Architecture: Simple LRU List + Hash Map

**Data Structures:**
```cpp
// Global LRU list
std::list<std::unique_ptr<Host_entry>> *hostname_cache_lru;

// Hash map for O(1) lookup (stores iterator to list)
std::unordered_map<std::string,
                   std::list<std::unique_ptr<Host_entry>>::iterator>
  *hostname_cache_hash;

// Single global mutex
mysql_mutex_t hostname_cache_mutex;
```

**Host Entry:**
```cpp
class Host_entry {
  std::string m_hostname;
  std::string m_ip_key;
  uint m_errors;
  time_t m_first_error_seen;
  // ... statistics
};
```

### Locking Strategy

**Global Mutex:**
```cpp
mysql_mutex_lock(&hostname_cache_mutex);
// ... operation ...
mysql_mutex_unlock(&hostname_cache_mutex);
```

**Simple and Safe:**
- One mutex protects both data structures
- No deadlock risk
- Easy to reason about

### LRU Management

**Move to Front on Access:**
```cpp
// hostname_cache.cc:248-249
hostname_cache_lru->splice(hostname_cache_lru->begin(),
                           *hostname_cache_lru, it->second);
```

**Evict from Back:**
```cpp
// hostname_cache.cc:205-212
void evict_oldest() {
  if (hostname_cache_lru->empty()) return;

  Host_entry *e = hostname_cache_lru->back().get();
  hostname_cache_hash->erase(e->ip_key());
  hostname_cache_lru->pop_back();
}
```

**Add to Front:**
```cpp
hostname_cache_lru->push_front(std::make_unique<Host_entry>(...));
hostname_cache_hash->insert({key, hostname_cache_lru->begin()});
```

### Size-Based Eviction

```cpp
// hostname_cache.cc:217-222
if (hostname_cache_lru->size() >= hostname_cache_max_size) {
  evict_oldest();
}
```

### Applicability to SP Cache

**✅ Good for Simple Implementation:**
- Easy to understand and implement
- Proven LRU pattern
- Minimal code complexity

**⚠️ Limitations for SP Cache:**
- Single global mutex may have contention at scale
- Better for smaller caches (hostname cache is typically small)

**Use Case:**
- Good for **Phase 1 prototype**
- Consider multi-instance if contention observed

---

## 4. ACL/User Cache

### Location
- **File:** `/home/user/mysql-server/sql/auth/sql_auth_cache.h`
- **Classes:** `Acl_cache`, `ACL_USER`, `ACL_DB`

### Architecture: Lock-Free Hash with Versioning

**Data Structures:**
```cpp
class Acl_cache {
  std::atomic<uint64> m_role_graph_version;  // Atomic version
  Acl_cache_internal m_cache;                // LF_HASH
  mysql_mutex_t m_cache_flush_mutex;
};

// Lock-free hash table
typedef struct lf_hash LF_HASH;
```

### Locking Strategy

**Lock-Free Reads:**
- Uses `LF_HASH` (Lock-Free Hash) for concurrent reads
- No mutex needed for read operations
- Atomic operations for updates

**RW Lock Guard:**
```cpp
class Acl_cache_lock_guard {
  enum class Acl_cache_lock_mode {
    READ_MODE,   // Shared lock
    WRITE_MODE   // Exclusive lock
  };
};
```

### Version-Based Invalidation

**Atomic Version Counter:**
```cpp
std::atomic<uint64> m_role_graph_version;

// Increment on change
m_role_graph_version.fetch_add(1, std::memory_order_release);

// Check if stale
uint64 cached_version = ...;
uint64 current_version = m_role_graph_version.load(std::memory_order_acquire);
if (cached_version != current_version) {
  // Stale, reload
}
```

### Applicability to SP Cache

**⚠️ Advanced Pattern:**
- Lock-free structures are complex to implement correctly
- Requires careful memory ordering
- Overkill for SP cache (SPs are large objects, not tiny ACL entries)

**Use Case:**
- Version-based invalidation pattern is useful (similar to current SP cache)
- Full lock-free probably not worth complexity

---

## 5. Plugin Cache

### Location
- **File:** `/home/user/mysql-server/sql/sql_plugin.h`

### Architecture: Global Static with Reference Counting

**Data Structures:**
```cpp
// Global plugin array
static st_plugin_int *plugin_array[MYSQL_MAX_PLUGIN_TYPE_NUM];

// Reference counted handle
typedef struct st_plugin_int *plugin_ref;
```

### Locking Strategy

**Global Mutexes:**
```cpp
mysql_mutex_t LOCK_plugin;
mysql_mutex_t LOCK_plugin_delete;
```

**Lock/Unlock Pattern:**
```cpp
plugin_ref plugin_lock(THD *thd, plugin_ref *ptr) {
  mysql_mutex_lock(&LOCK_plugin);
  // ... increment ref count ...
  mysql_mutex_unlock(&LOCK_plugin);
}

void plugin_unlock(THD *thd, plugin_ref plugin) {
  mysql_mutex_lock(&LOCK_plugin);
  // ... decrement ref count ...
  mysql_mutex_unlock(&LOCK_plugin);
}
```

### Reference Counting

**Usage Tracking:**
- Each plugin has reference count
- Cannot unload plugin while `ref_count > 0`
- Safe deletion after all refs released

### Applicability to SP Cache

**✅ Reference Counting Pattern:**
- Similar need: Cannot delete SP while in use
- Simple increment/decrement on lock/unlock
- Proven safe deletion mechanism

**Not Applicable:**
- Static array structure (plugins are few, SPs are many)
- Plugin-specific state machine

---

## 6. InnoDB Buffer Pool LRU

### Location
- **File:** `/home/user/mysql-server/storage/innobase/include/buf0lru.h`

### Architecture: Two-Segment LRU

**Key Concepts:**
```cpp
// LRU list with young and old segments
buf_pool->LRU;          // Main LRU list
buf_pool->LRU_old;      // Pointer to old segment start
buf_pool->LRU_old_len;  // Length of old segment
```

**Midpoint Insertion:**
- New pages inserted at midpoint (not head)
- Protects hot pages from one-time scans
- Adaptive based on access patterns

**Time-Based Promotion:**
```cpp
// Only promote to young if accessed after threshold
if (current_time - block->access_time > threshold) {
  move_to_young(block);
}
```

### Applicability to SP Cache

**⚠️ Advanced LRU:**
- Two-segment LRU is sophisticated
- Useful for very large caches (GB-scale buffer pool)
- Probably overkill for SP cache (MB-scale)

**Simpler Alternative:**
- Standard LRU sufficient for SP cache
- Consider if cache becomes very large (1000s of SPs)

---

## 7. Current SP Cache (Thread-Local)

### Location
- **Files:** `/home/user/mysql-server/sql/sp_cache.h`, `/home/user/mysql-server/sql/sp_cache.cc`

### Current Architecture

**Thread-Local Design:**
```cpp
// sql/sql_class.h:2853-2854
class THD {
  sp_cache *sp_proc_cache;  // Per-THD
  sp_cache *sp_func_cache;  // Per-THD
};
```

**Simple Hash Map:**
```cpp
class sp_cache {
  collation_unordered_map<std::string, std::unique_ptr<sp_head>> m_hashtable;
};
```

### Version-Based Invalidation

**Global Atomic Version:**
```cpp
// sql/sp_cache.cc:54-59
static std::atomic<int64> Cversion{0};

int64 sp_cache_version() {
  return Cversion.load(std::memory_order_relaxed);
}

void sp_cache_invalidate() {
  Cversion.fetch_add(1, std::memory_order_relaxed);
}
```

**Per-SP Version Check:**
```cpp
// sp_head.h:529-530
uint64 sp_cache_version() const { return m_sp_cache_version; }
void set_sp_cache_version(uint64 version_arg) { m_sp_cache_version = version_arg; }
```

### Size Limit Enforcement

**All-or-Nothing Clear:**
```cpp
// sql/sp_cache.cc:74-76
void enforce_limit(ulong upper_limit_for_elements) {
  if (m_hashtable.size() > upper_limit_for_elements)
    m_hashtable.clear();  // Nuclear option!
}
```

**Problem:** No LRU, just clears entire cache

### Applicability

**✅ Keep These Patterns:**
- Version-based invalidation (works well)
- String key (qualified name)
- `sp_head` storage

**❌ Replace These:**
- Thread-local design → Global
- All-or-nothing eviction → LRU
- No concurrency control → Add locking
- No reference counting → Add ref counts

---

## 8. Comparison Matrix

| Feature | Table Cache | DD Cache | Hostname Cache | ACL Cache | Plugin Cache | Current SP Cache |
|---------|-------------|----------|----------------|-----------|--------------|------------------|
| **Scope** | Global, Multi-Instance | Global | Global | Global | Global | Thread-Local |
| **Sharding** | ✅ Yes (16-64 instances) | ❌ No | ❌ No | ❌ No | ❌ No | ❌ No |
| **Locking** | Per-instance mutex | Per-map mutex | Global mutex | Lock-free read, RW write | Global mutex | None (thread-local) |
| **Eviction** | LRU, size-based | LRU, capacity-based | LRU, size-based | No eviction | No eviction | All-or-nothing |
| **Reference Counting** | ✅ (in-use list) | ✅ Atomic ref counter | ❌ No | ✅ Plugin ref | ✅ Plugin ref | ❌ No |
| **Cache Miss Handling** | ❌ No coordination | ✅ Cond var coordination | ❌ No coordination | ❌ No | ❌ No | ❌ No |
| **Memory Reuse** | ❌ No | ✅ Element pool | ❌ No | ❌ No | ❌ No | ❌ No |
| **Version Tracking** | ✅ TABLE_SHARE version | ✅ Global version | ❌ No | ✅ Role graph version | ✅ Plugin state | ✅ Global atomic version |
| **Complexity** | Medium | High | Low | High | Low | Low |
| **Scalability** | Excellent (sharded) | Good | Fair (global lock) | Excellent (lock-free) | Fair | N/A (thread-local) |

---

## 9. Recommended Architecture for Global SP Cache

Based on the analysis, here's the optimal hybrid architecture:

### 9.1 Multi-Instance Sharding (from Table Cache)

**Rationale:**
- Reduces lock contention
- N-way parallelism (N = number of instances)
- Proven at MySQL scale

**Implementation:**
```cpp
class Global_SP_cache_manager {
  SP_cache **m_cache_instances;     // Array of caches
  uint m_num_instances;              // Default: 16

  uint cache_id(uint64 thread_id) const {
    return thread_id % m_num_instances;
  }

  SP_cache *get_cache_for_thread(uint64 thread_id) {
    return m_cache_instances[cache_id(thread_id)];
  }
};
```

### 9.2 Cache Element with Reference Counting (from DD Cache)

**Rationale:**
- Safe deletion (only when ref_count == 0)
- Protects in-use SPs from eviction
- Memory-efficient reuse

**Implementation:**
```cpp
class SP_cache_element {
  sp_head *m_sp;                      // Stored procedure
  std::atomic<uint32_t> m_ref_counter;  // Thread-safe ref count
  uint64_t m_version;                 // For invalidation
  uint64_t m_last_used;               // For LRU (timestamp)

  // LRU list pointers
  SP_cache_element *m_next;
  SP_cache_element *m_prev;
};
```

### 9.3 LRU Free List (from DD Cache + Hostname Cache)

**Rationale:**
- Efficient LRU tracking
- O(1) move to front/back
- Standard pattern

**Implementation:**
```cpp
class SP_free_list {
  SP_cache_element *m_head;  // Most recently used
  SP_cache_element *m_tail;  // Least recently used
  uint m_length;

  void add_to_head(SP_cache_element *e);
  SP_cache_element *remove_from_tail();
  void move_to_head(SP_cache_element *e);
};
```

### 9.4 Per-Instance Storage

**Implementation:**
```cpp
class SP_cache {
  // Primary storage
  std::unordered_map<std::string, SP_cache_element*> m_hash_map;

  // LRU tracking
  SP_free_list m_free_list;

  // Element pool (reuse)
  std::vector<SP_cache_element*> m_element_pool;

  // Limits
  size_t m_capacity;       // Max SPs in this instance
  size_t m_memory_limit;   // Max memory in bytes

  // Synchronization
  mysql_mutex_t m_lock;
  mysql_cond_t m_miss_handled;  // For cache miss coordination

  // Statistics
  uint64_t m_hits;
  uint64_t m_misses;
};
```

### 9.5 Locking Strategy

**Per-Instance Mutex:**
```cpp
void SP_cache::get(const std::string &key, SP_cache_element **element) {
  mysql_mutex_lock(&m_lock);

  auto it = m_hash_map.find(key);
  if (it != m_hash_map.end()) {
    // Cache hit
    *element = it->second;
    (*element)->m_ref_counter++;
    m_free_list.move_to_head(*element);
    m_hits++;
    mysql_mutex_unlock(&m_lock);
    return;
  }

  // Cache miss - coordinate with other threads
  if (is_being_loaded(key)) {
    // Wait for other thread to load
    while (is_being_loaded(key)) {
      mysql_cond_wait(&m_miss_handled, &m_lock);
    }
    mysql_mutex_unlock(&m_lock);
    return get(key, element);  // Retry
  }

  // Mark as being loaded
  mark_loading(key);
  m_misses++;
  mysql_mutex_unlock(&m_lock);

  // Caller loads SP (outside lock)
  *element = nullptr;
}
```

### 9.6 Cache Miss Coordination (from DD Cache)

**Prevents Duplicate Parsing:**
```cpp
// Thread A: Cache miss on SP "proc1"
// Thread A: Marks "proc1" as being loaded
// Thread A: Unlocks, parses SP from DD

// Thread B: Cache miss on SP "proc1"
// Thread B: Sees "proc1" is being loaded
// Thread B: Waits on condition variable

// Thread A: Finishes parsing, inserts to cache
// Thread A: Broadcasts m_miss_handled

// Thread B: Wakes up, retries lookup
// Thread B: Finds SP in cache (hit!)
```

### 9.7 Eviction Policy

**Capacity-Based LRU:**
```cpp
void SP_cache::evict_if_necessary() {
  mysql_mutex_lock(&m_lock);

  while (m_hash_map.size() > m_capacity && m_free_list.length() > 0) {
    SP_cache_element *e = m_free_list.tail();  // LRU

    // Only evict if not in use
    if (e->m_ref_counter == 0) {
      m_hash_map.erase(e->m_sp->m_qname);
      m_free_list.remove(e);

      // Delete SP
      sp_head::destroy(e->m_sp);

      // Return element to pool or delete
      return_to_pool_or_delete(e);
    } else {
      break;  // All remaining elements are in use
    }
  }

  mysql_mutex_unlock(&m_lock);
}
```

### 9.8 Version-Based Invalidation (keep current pattern)

**Global Atomic Version:**
```cpp
static std::atomic<uint64_t> g_sp_cache_version{0};

void sp_cache_invalidate() {
  g_sp_cache_version.fetch_add(1, std::memory_order_release);
}

uint64_t sp_cache_version() {
  return g_sp_cache_version.load(std::memory_order_acquire);
}
```

**Check on Access:**
```cpp
void SP_cache::get(const std::string &key, SP_cache_element **element) {
  // ... find element ...

  if ((*element)->m_version < sp_cache_version()) {
    // Stale - remove and reload
    remove(*element);
    // ... trigger reload ...
  }
}
```

### 9.9 Memory Management

**Element Pool (from DD Cache):**
```cpp
SP_cache_element *SP_cache::create_element() {
  if (!m_element_pool.empty()) {
    SP_cache_element *e = m_element_pool.back();
    m_element_pool.pop_back();
    return e;  // Reuse
  }
  return new SP_cache_element();  // Allocate new
}

void SP_cache::return_element(SP_cache_element *e) {
  e->m_sp = nullptr;
  e->m_ref_counter = 0;
  m_element_pool.push_back(e);  // Reuse later
}
```

### 9.10 Statistics and Monitoring

```cpp
class SP_cache {
  std::atomic<uint64_t> m_hits;
  std::atomic<uint64_t> m_misses;
  std::atomic<uint64_t> m_evictions;
  std::atomic<uint64_t> m_invalidations;

  double hit_rate() const {
    uint64_t h = m_hits.load();
    uint64_t m = m_misses.load();
    return (h + m > 0) ? (double)h / (h + m) : 0.0;
  }
};
```

---

## 10. Implementation Roadmap

### Phase 1: Basic Global Cache (2 weeks)

**Goals:**
- Single global instance (no sharding yet)
- Simple mutex locking
- No reference counting initially
- Basic LRU eviction

**Implementation:**
```cpp
class SP_cache {
  std::unordered_map<std::string, sp_head*> m_map;
  std::list<sp_head*> m_lru;
  mysql_mutex_t m_lock;
  size_t m_capacity;
};
```

**Integration:**
- Replace `THD::sp_proc_cache` with global cache lookup
- Keep version-based invalidation

**Validation:**
- Test with 100 connections, 10 SPs
- Measure memory savings
- Performance benchmarks

### Phase 2: Reference Counting (1 week)

**Goals:**
- Add `SP_cache_element` wrapper
- Implement atomic ref counting
- Safe deletion when `ref_count == 0`

**Integration:**
- Increment ref count on checkout
- Decrement on release
- Only evict when `ref_count == 0`

**Validation:**
- Concurrent SP execution tests
- Ensure no use-after-free

### Phase 3: Cache Miss Coordination (1 week)

**Goals:**
- Prevent duplicate parsing
- Add condition variable for waiting threads

**Integration:**
- Mark SP as "being loaded"
- Other threads wait on condition variable
- Broadcast when load completes

**Validation:**
- Test with 1000 threads racing to load same SP
- Verify only one parse occurs

### Phase 4: Multi-Instance Sharding (1 week)

**Goals:**
- Split into 16 cache instances
- Per-instance locking
- Thread assignment by `thread_id % 16`

**Integration:**
- Create `Global_SP_cache_manager`
- Route cache operations to correct instance

**Validation:**
- Lock contention testing with 10K threads
- Compare throughput vs single-instance

### Phase 5: Element Pool & Memory Limit (1 week)

**Goals:**
- Reuse cache elements
- Add memory limit (in addition to capacity limit)

**Integration:**
- Track memory usage per instance
- Evict based on memory pressure

**Validation:**
- Memory leak testing
- Verify element reuse

### Phase 6: Monitoring & Tuning (1 week)

**Goals:**
- Add statistics (hits, misses, evictions)
- Performance Schema integration
- Tuning parameters

**Configuration:**
```sql
SET GLOBAL global_sp_cache_size = 100;           -- Per-instance capacity
SET GLOBAL global_sp_cache_memory_limit = 100M;  -- Per-instance memory
SET GLOBAL global_sp_cache_instances = 16;       -- Number of instances
```

**Monitoring:**
```sql
SHOW STATUS LIKE 'Sp_cache_%';
-- Sp_cache_hits
-- Sp_cache_misses
-- Sp_cache_evictions
-- Sp_cache_hit_rate
```

---

## 11. File Paths for Reference Implementation

**Must-Read Files:**
1. `/home/user/mysql-server/sql/table_cache.h` - Multi-instance sharding pattern
2. `/home/user/mysql-server/sql/table_cache.cc` - LRU eviction, locking
3. `/home/user/mysql-server/sql/dd/impl/cache/cache_element.h` - Reference counting
4. `/home/user/mysql-server/sql/dd/impl/cache/free_list.h` - LRU list implementation
5. `/home/user/mysql-server/sql/dd/impl/cache/shared_multi_map.h` - Cache miss coordination
6. `/home/user/mysql-server/sql/hostname_cache.cc` - Simple LRU pattern
7. `/home/user/mysql-server/sql/sp_cache.cc` - Current SP cache (to replace)

**Key Functions to Study:**
```cpp
// Table cache sharding
Table_cache_manager::get_cache(THD *thd)
Table_cache::get_table(...)
Table_cache::release_table(...)
Table_cache::free_unused_tables_if_necessary()

// DD cache reference counting
Cache_element<T>::use()
Cache_element<T>::release()
Shared_multi_map<T>::put(...)
Shared_multi_map<T>::get(...)
Free_list<T>::add_last(...)
Free_list<T>::get_lru()

// Hostname cache LRU
hostname_cache_get_host_entry(...)
hostname_cache_add_or_update(...)
evict_oldest()
```

---

## 12. Expected Results

### Memory Savings (100K Connections)

**Scenario: 5 Unique SPs, Average 100 KB each**

**Current (Thread-Local):**
```
60,000 connections with SPs × 200 KB average = 12 GB
Actual unique data: 5 × 100 KB = 500 KB
Waste: 11.99 GB (99.996%)
```

**After Global Cache (16 instances):**
```
Global cache (16 instances):
  5 SPs × 100 KB = 500 KB per instance
  500 KB × 16 instances = 8 MB

Per-THD overhead:
  SP references: 100K × 40 bytes = 4 MB

Total: 12 MB (vs 12 GB)
Savings: 99.9%
```

### Performance Impact

**Expected:**
- Cache hit latency: +5-10 μs (mutex lock/unlock overhead)
- Cache miss (first load): Same as current (parse from DD)
- Throughput: 10-20% improvement (better cache hit rate)

**Acceptable Trade-offs:**
- Slight latency increase per lookup (microseconds)
- Massive memory savings (gigabytes)
- Better overall system performance (less memory pressure)

---

## Summary

MySQL provides excellent reference implementations for building a Global SP Cache:

1. **Table Cache** - Multi-instance sharding for scalability
2. **DD Cache** - Reference counting for safety, cache miss coordination
3. **Hostname Cache** - Simple LRU pattern for eviction

**Recommended Approach:**
- Start with simple global cache (Phase 1)
- Add reference counting (Phase 2)
- Add cache miss coordination (Phase 3)
- Scale with multi-instance sharding (Phase 4)

**Total Effort:** ~7 weeks for full implementation
**Expected Savings:** 99.9% memory reduction for SP workloads

This hybrid architecture leverages proven MySQL patterns while addressing the specific needs of stored procedure caching.

---

**End of Document**
