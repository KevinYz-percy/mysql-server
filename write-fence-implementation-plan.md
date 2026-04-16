# Plan: `component_write_fence` — Table-Level Write Fencing + Active Writer Detection

## Context

We are building a **data mover** that migrates tables from MySQL Server A to Server B
(Seeding -> CDC -> Cutover). During **cutover**, the mover must:

1. **Fence** watched tables on Server A — block all new writes instantly
2. **Drain** in-flight writers — monitor and wait for existing transactions to finish
3. **Kill** lingering writers that exceed a time budget via `KILL QUERY <connection_id>`
4. **Proceed** with cutover once the source is quiescent

This plan covers two pieces:
- **Part 1: `component_write_fence`** — a MySQL component implementing fencing (step 1)
  using the `event_tracking_table_access` service
- **Part 2: Active writer detection via InnoDB Path B** — walks `dict_table_t::locks`
  filtered by LOCK_IX, avoiding `trx_sys->mutex`. Exposed as a UDF registered by the
  component, implemented via a new public function in `lock0lock.cc`.

### Why a component?
- Dynamic load/unload via `INSTALL COMPONENT` / `UNINSTALL COMPONENT` — no server rebuild
- Follows MySQL 8.0 component architecture patterns (`connection_control` as reference)

### Why Path B for active writer detection?
- `trx_t::mod_tables` is only populated at commit time — **useless for in-flight writers**
- P_S `data_locks` query works but is heavyweight (full SQL parse, joins, LIKE matching)
- Path B walks `dict_table_t::locks` per table under the **per-table shard latch** only —
  no global `trx_sys->mutex` needed for the iteration
- `lock->trx->id` (trx_id_t) is immutable once assigned — safe to read without any mutex
- Maps trx_id → connection_id via `trx->mysql_thd` + `thd_get_thread_id()` (null-check
  for XA PREPARED; accept the negligible race for non-XA workloads)

---

## Architecture

```
                        Data Mover Control Plane
                               |
          +--------------------+---------------------+
          |                    |                     |
   write_fence_set()    write_fence_active_     KILL QUERY <id>
   write_fence_clear()   writers('db','t')
          |                    |
   component_write_fence       |
     |                         |
     |  event_tracking         |  UDF calls into InnoDB
     |  callback               |
     |                         v
     v                    lock_table_get_active_writers()  [new, lock0lock.cc]
   open_tables()               |
   sql/sql_base.cc:6031        v
                          dict_table_t::locks  (per-table shard latch only)
                          filter LOCK_TABLE | LOCK_IX/LOCK_X, !LOCK_WAIT
                          read lock->trx->id, lock->trx->mysql_thd
```

### Fencing mechanism
- The `event_tracking_table_access` callback fires in `open_tables()` at
  `sql/sql_base.cc:6031` — BEFORE IX lock acquisition and BEFORE any rows are touched.
- Returning `true` from the callback causes `event_tracking_dispatch_error()`
  (`sql/sql_audit.cc:345`) to raise `ER_AUDIT_API_ABORT`, aborting the statement.
- READ events are filtered out via `filtered_sub_events`; only INSERT/UPDATE/DELETE
  reach the callback.
- Every DML path goes through `open_tables()` and fires table access events.

### Active writer detection (Path B — `dict_table_t::locks` walk)
- New function `lock_table_get_active_writers()` in `lock0lock.cc`
- Acquires `locksys::Shard_latch_guard{*table}` (per-table shard latch + shared global latch)
- Iterates `table->locks` (UT_LIST), filters by `LOCK_TABLE | LOCK_IX` or `LOCK_X`,
  skips `LOCK_WAIT` entries
- Reads `lock->trx->id` (safe — immutable) and `lock->trx->mysql_thd` → `thd_get_thread_id()`
- **No `trx_sys->mutex` needed** — we accept the negligible XA-prepared race
- Deduplicates by trx_id (a trx may hold multiple table locks)
- Returns vector of `{trx_id, connection_id}` pairs
- Exposed via UDF `write_fence_active_writers(schema, table)` registered by the component

---

## File Structure

### Component (under `components/write_fence/`)

```
components/write_fence/
  CMakeLists.txt                  # Build config (MYSQL_ADD_COMPONENT)
  write_fence.cc                  # Component init/deinit, event callback, service wiring
  write_fence.h                   # Shared declarations, extern globals
  write_fence_data.cc             # FenceSet class (rwlock-protected unordered_set)
  write_fence_data.h              # FenceSet header
  write_fence_udf.cc              # UDF implementations (fence/unfence/check/list/clear_all/active_writers)
  write_fence_udf.h               # UDF declarations
  write_fence_pfs_table.cc        # P_S table: performance_schema.write_fence_status
  write_fence_pfs_table.h         # P_S table header
```

### InnoDB changes (minimal, in existing files)

```
storage/innobase/include/lock0lock.h   # Add lock_table_get_active_writers() declaration
storage/innobase/lock/lock0lock.cc     # Add lock_table_get_active_writers() implementation
storage/innobase/handler/ha_innodb.cc  # Register innodb_table_active_writers() UDF (called by component)
```

---

## Implementation Steps

### Step 1: FenceSet data structure (`write_fence_data.h` / `.cc`)

A rwlock-protected `std::unordered_set<std::string>` holding fenced table keys.

- Key format: `schema + '\0' + table` (null-separated to avoid "a.b" vs "a" + "b" ambiguity)
- `is_fenced(schema, schema_len, table, table_len)` — acquires **read lock**, O(1) hash lookup.
  This is the hot path called from the event callback on every DML.
- `fence(schema, table)` / `unfence(schema, table)` — acquire **write lock**
- `list_all()` — returns a snapshot `std::vector<FencedTableInfo>` under read lock
- `clear_all()` — acquires write lock, clears set, returns count
- `count()` — read lock, returns size

The rwlock is PSI-instrumented using `mysql_rwlock_register()` / `mysql_rwlock_init()`,
following the pattern at `components/connection_control/connection_control.cc:62-72`.

**Reference files:**
- `components/connection_control/connection_control_data.h` (state management pattern)
- `include/mysql/components/services/mysql_rwlock.h` (rwlock service)

### Step 2: Component skeleton (`write_fence.cc` / `.h`)

**Required services:**
```cpp
REQUIRES_MYSQL_RWLOCK_SERVICE_PLACEHOLDER;
REQUIRES_MYSQL_MUTEX_SERVICE_PLACEHOLDER;
REQUIRES_PSI_MEMORY_SERVICE_PLACEHOLDER;
REQUIRES_SERVICE_PLACEHOLDER(log_builtins);
REQUIRES_SERVICE_PLACEHOLDER(log_builtins_string);
REQUIRES_SERVICE_PLACEHOLDER(udf_registration);
REQUIRES_SERVICE_PLACEHOLDER(mysql_current_thread_reader);
REQUIRES_SERVICE_PLACEHOLDER(mysql_thd_security_context);
REQUIRES_SERVICE_PLACEHOLDER(mysql_security_context_options);
REQUIRES_SERVICE_PLACEHOLDER(global_grants_check);
REQUIRES_SERVICE_PLACEHOLDER(dynamic_privilege_register);
REQUIRES_SERVICE_PLACEHOLDER(pfs_plugin_column_string_v2);
REQUIRES_SERVICE_PLACEHOLDER(pfs_plugin_column_integer_v1);
REQUIRES_SERVICE_PLACEHOLDER(pfs_plugin_table_v1);
REQUIRES_SERVICE_PLACEHOLDER(component_sys_variable_register);
REQUIRES_SERVICE_PLACEHOLDER(component_sys_variable_unregister);
REQUIRES_SERVICE_PLACEHOLDER(status_variable_registration);
```

**Provided services:**
```cpp
IMPLEMENTS_SERVICE_EVENT_TRACKING_TABLE_ACCESS(write_fence);

BEGIN_COMPONENT_PROVIDES(write_fence)
PROVIDES_SERVICE_EVENT_TRACKING_TABLE_ACCESS(write_fence),
END_COMPONENT_PROVIDES();
```

**Event tracking callback (core fencing logic):**
```cpp
namespace Event_tracking_implementation {

// Filter out READ events — only INSERT/UPDATE/DELETE reach callback
mysql_event_tracking_table_access_subclass_t
    Event_tracking_table_access_implementation::filtered_sub_events =
        EVENT_TRACKING_TABLE_ACCESS_READ;

bool Event_tracking_table_access_implementation::callback(
    const mysql_event_tracking_table_access_data *data) {
  if (!g_enabled.load(std::memory_order_relaxed)) return false;

  if (g_fence_set.is_fenced(data->table_database.str,
                             data->table_database.length,
                             data->table_name.str,
                             data->table_name.length)) {
    g_blocked_writes.fetch_add(1, std::memory_order_relaxed);
    return true;   // Block the DML — raises ER_AUDIT_API_ABORT
  }
  return false;    // Allow the DML
}

}  // namespace Event_tracking_implementation
```

**init() sequence:**
1. Initialize logging
2. Register PSI instruments (rwlock)
3. Initialize FenceSet (creates rwlock)
4. Register P_S table
5. Register system variables
6. Register status variables
7. Register dynamic privilege `WRITE_FENCE_ADMIN`
8. Register UDFs

**deinit():** Reverse order. Log warning if tables still fenced.

**System variables:**
| Variable | Type | Default | Purpose |
|---|---|---|---|
| `component_write_fence.enabled` | BOOL | true | Master kill-switch for the callback |

**Status variables:**
| Variable | Type | Purpose |
|---|---|---|
| `Component_write_fence_blocked_writes` | LONGLONG | Count of blocked DML statements |
| `Component_write_fence_fenced_tables` | LONGLONG | Current number of fenced tables |

**Reference file:** `components/connection_control/connection_control.cc:590-648`

### Step 3: UDFs (`write_fence_udf.cc` / `.h`)

| UDF | Return | Args | Privilege | Behavior |
|---|---|---|---|---|
| `write_fence_set(schema, table)` | INT | 2 strings | WRITE_FENCE_ADMIN | Fence a table. Returns 1=success, 0=already fenced |
| `write_fence_clear(schema, table)` | INT | 2 strings | WRITE_FENCE_ADMIN | Unfence. Returns 1=success, 0=not fenced |
| `write_fence_clear_all()` | INT | 0 | WRITE_FENCE_ADMIN | Clear all fences. Returns count |
| `write_fence_check(schema, table)` | INT | 2 strings | None (read-only) | Returns 1=fenced, 0=not |
| `write_fence_list()` | STRING | 0 | None (read-only) | JSON array of fenced tables |
| `write_fence_active_writers(schema, table)` | STRING | 2 strings | WRITE_FENCE_ADMIN | JSON array of active writers (see Step 7) |

Privilege check pattern:
```cpp
THD *thd = nullptr;
mysql_service_mysql_current_thread_reader->get(&thd);
Security_context_handle ctx = nullptr;
mysql_service_mysql_thd_security_context->get(thd, &ctx);
if (!mysql_service_global_grants_check->has_global_grant(
        ctx, "WRITE_FENCE_ADMIN", strlen("WRITE_FENCE_ADMIN"))) {
  strcpy(message, "Access denied; you need WRITE_FENCE_ADMIN privilege");
  return true;
}
```

**Reference files:**
- `include/mysql/components/services/udf_registration.h` (service interface)
- `components/test/test_udf_registration.cc` (registration pattern)
- `include/mysql/components/services/dynamic_privilege.h` (privilege APIs)

### Step 4: P_S table (`write_fence_pfs_table.cc` / `.h`)

Table: `performance_schema.write_fence_status`

| Column | Type | Description |
|---|---|---|
| TABLE_SCHEMA | VARCHAR(64) | Database name |
| TABLE_NAME | VARCHAR(64) | Table name |
| FENCED_AT | BIGINT UNSIGNED | Microseconds since epoch |

Implementation uses `PFS_engine_table_share_proxy` with READONLY access:
- `open_table()`: snapshot `g_fence_set.list_all()` into `std::vector`
- `rnd_next()`: iterate snapshot
- `read_column_value()`: use `pfs_plugin_column_string_v2->set_varchar_utf8mb4()` and
  `pfs_plugin_column_integer_v1->set_unsigned()`
- `close_table()`: delete snapshot

**Reference file:** `components/connection_control/connection_control_pfs_table.cc`

### Step 5: CMakeLists.txt

```cmake
MYSQL_ADD_COMPONENT(write_fence
  write_fence.cc
  write_fence_data.cc
  write_fence_udf.cc
  write_fence_pfs_table.cc
  MODULE_ONLY
  LINK_LIBRARIES
    library_mysys
)
ADD_DEFINITIONS(-DLOG_COMPONENT_TAG="WRITE_FENCE")
```

**Reference file:** `components/connection_control/CMakeLists.txt`

### Step 6: InnoDB-side — `lock_table_get_active_writers()` (Path B)

This is the core of active writer detection. A new public function in InnoDB's lock module.

**Declaration** — add to `storage/innobase/include/lock0lock.h` (after line ~859,
near existing `lock_get_*` accessor functions):

```cpp
/** Result entry for lock_table_get_active_writers(). */
struct lock_table_writer_t {
  trx_id_t trx_id;              /**< transaction ID */
  unsigned long connection_id;   /**< MySQL connection ID (0 if THD is NULL) */
};

/** Get active write-transaction connection IDs for a table.
Walks dict_table_t::locks under the per-table shard latch, filters by
LOCK_TABLE with LOCK_IX or LOCK_X mode (granted, not waiting).
Does NOT acquire trx_sys->mutex — reads trx->mysql_thd without global lock
(safe for non-XA; XA-prepared disconnect may yield connection_id=0).
@param[in]  table    table to inspect (must be open, n_ref_count > 0)
@param[out] writers  output vector, cleared before use
@return number of distinct active writers found */
size_t lock_table_get_active_writers(
    dict_table_t *table,
    std::vector<lock_table_writer_t> *writers);
```

**Implementation** — add to `storage/innobase/lock/lock0lock.cc` (after line ~5815,
near existing `lock_get_*` functions):

```cpp
size_t lock_table_get_active_writers(
    dict_table_t *table,
    std::vector<lock_table_writer_t> *writers) {
  ut_ad(table != nullptr);
  ut_ad(writers != nullptr);
  writers->clear();

  // Per-table shard latch + shared global latch. No trx_sys->mutex.
  locksys::Shard_latch_guard guard{UT_LOCATION_HERE, *table};

  std::unordered_set<trx_id_t> seen_trx;

  for (auto lock = UT_LIST_GET_FIRST(table->locks);
       lock != nullptr;
       lock = UT_LIST_GET_NEXT(tab_lock.locks, lock)) {

    // Only table-level locks
    if (lock_get_type_low(lock) != LOCK_TABLE) continue;

    // Only granted (not waiting)
    if (lock->type_mode & LOCK_WAIT) continue;

    // Only IX or X mode (writers)
    const auto mode = lock_get_mode(lock);
    if (mode != LOCK_IX && mode != LOCK_X) continue;

    const trx_t *trx = lock->trx;
    const trx_id_t trx_id = trx->id;

    // Deduplicate by trx_id
    if (!seen_trx.insert(trx_id).second) continue;

    // Read connection_id. trx->mysql_thd can be NULL for XA-prepared
    // disconnected transactions. We read without trx_sys->mutex —
    // acceptable for non-XA workloads (see design doc for rationale).
    unsigned long conn_id = 0;
    THD *thd = trx->mysql_thd;
    if (thd != nullptr) {
      conn_id = thd_get_thread_id(thd);
    }

    writers->push_back({trx_id, conn_id});
  }

  return writers->size();
}
```

**Latch analysis:**
- `locksys::Shard_latch_guard` (`lock0guards.h:118-130`) acquires:
  1. `Global_shared_latch_guard` — shared s-latch on `lock_sys->latches.global_latch`
  2. `Shard_naked_latch_guard` — mutex on the shard for `table->id`
- This is the same latch discipline used by P_S `Innodb_data_lock_iterator` (`p_s.cc:252-260`)
- **No `trx_sys->mutex` acquired** — avoids global contention
- `lock_get_type_low()` and `lock_get_mode()` are inline functions that only read
  `lock->type_mode` — safe under shard latch
- `lock->trx->id` is assigned once during `trx_start_low()` and never changes — safe
- `lock->trx->mysql_thd` is read without mutex. For non-XA: stable while trx is active.
  For XA-prepared: could be nulled by `trx_disconnect_from_mysql()` under `trx_sys->mutex`.
  We accept `conn_id=0` in this rare case.

**Reference files:**
- `lock0lock.cc:5651-5815` — existing public `lock_get_*` accessor functions (precedent)
- `lock0lock.cc:3246-3296` — `lock_table_create()` showing how locks are added to table->locks
- `lock0guards.h:118-130` — `Shard_latch_guard` RAII class
- `lock0priv.h:137-187` — `struct lock_t` definition
- `lock0priv.h:54-64` — `struct lock_table_t` (the `tab_lock` union member)
- `lock0priv.ic:94-99` — `lock_get_mode()` inline
- `lock0priv.ic:45-48` — `lock_get_type_low()` inline
- `handler/p_s.cc:252-260` — existing P_S pattern for table lock scanning

### Step 7: `write_fence_active_writers` UDF (bridges component → InnoDB)

The component registers this UDF. It calls into InnoDB via `ha_innodb.cc` wrapper.

**Bridge function** — add to `storage/innobase/handler/ha_innodb.cc` (near existing
UDF or status functions):

```cpp
/** Called from component UDF. Opens table by name, calls
lock_table_get_active_writers(), closes table.
@param[in]  schema       schema name (null-terminated)
@param[in]  table_name   table name (null-terminated)
@param[out] writers      output vector
@return 0 on success, 1 if table not found */
int innodb_get_active_table_writers(
    const char *schema,
    const char *table_name,
    std::vector<lock_table_writer_t> *writers) {
  char full_name[MAX_FULL_NAME_LEN + 1];
  snprintf(full_name, sizeof(full_name), "%s/%s", schema, table_name);

  dict_table_t *table = dict_table_open_on_name(
      full_name, false /* dict_locked */, false /* try_drop */,
      DICT_ERR_IGNORE_NONE);
  if (table == nullptr) return 1;

  lock_table_get_active_writers(table, writers);

  dict_table_close(table, false /* dict_locked */, false /* thd */);
  return 0;
}
```

**Note on `dict_table_open_on_name()` (`dict0dict.h:346`):**
- Opens the table and increments `n_ref_count`, preventing eviction
- `dict_table_close()` decrements `n_ref_count`
- Table name format for InnoDB: `"schema/table"` (forward slash, not dot)

**UDF implementation** in `write_fence_udf.cc`:

```cpp
static char *write_fence_active_writers_func(
    UDF_INIT *initid, UDF_ARGS *args,
    char *result, unsigned long *length,
    unsigned char *is_null, unsigned char *error) {

  std::string schema(args->args[0], args->lengths[0]);
  std::string table(args->args[1], args->lengths[1]);

  std::vector<lock_table_writer_t> writers;
  int rc = innodb_get_active_table_writers(
      schema.c_str(), table.c_str(), &writers);

  if (rc != 0) {
    *is_null = 1;
    return nullptr;
  }

  // Build JSON: [{"trx_id":123,"connection_id":456}, ...]
  std::string json = "[";
  for (size_t i = 0; i < writers.size(); i++) {
    if (i > 0) json += ",";
    json += "{\"trx_id\":" + std::to_string(writers[i].trx_id) +
            ",\"connection_id\":" + std::to_string(writers[i].connection_id) + "}";
  }
  json += "]";

  // Copy to UDF result buffer (allocated in init)
  if (json.size() > initid->max_length) {
    initid->max_length = json.size();
  }
  char *buf = (char *)malloc(json.size() + 1);
  memcpy(buf, json.c_str(), json.size() + 1);
  initid->ptr = buf;
  *length = json.size();
  return buf;
}
```

**Linkage concern:** The component is a separate `.so` that cannot directly call
InnoDB functions. Two approaches:

**Option A (Recommended): Declare `innodb_get_active_table_writers` as an exported
symbol from mysqld.** Since `ha_innodb.cc` is compiled into the server (not a plugin),
its symbols are available to dynamically loaded components if declared with default
visibility. Add to a shared header:

```cpp
// include/mysql/components/services/write_fence_innodb_bridge.h
extern "C" int innodb_get_active_table_writers(
    const char *schema, const char *table_name,
    void *writers_vec /* std::vector<lock_table_writer_t>* */);
```

The component `dlsym()`s this function at init time, or we expose it through the
component registry as a minimal service.

**Option B: Register as a component service.** Define a new service interface:

```cpp
// include/mysql/components/services/innodb_write_fence_service.h
BEGIN_SERVICE_DEFINITION(innodb_write_fence)
DECLARE_METHOD(int, get_active_writers,
    (const char *schema, const char *table_name,
     unsigned long *conn_ids, size_t max_ids, size_t *num_found));
END_SERVICE_DEFINITION(innodb_write_fence)
```

Implement in `ha_innodb.cc`, register via `mysql_service_registry->register_service()`.
The component REQUIRES this service. This is cleaner but requires InnoDB to register
a component service (no existing precedent — InnoDB currently only uses PSI).

**Recommendation**: Go with Option B (component service). It's cleaner, type-safe,
and follows the component framework's design intent. The service uses a flat C
interface (array of `unsigned long` + count) to avoid crossing ABI boundaries with
`std::vector`. Registration happens in `innobase_init()` alongside the existing
`mysql_data_lock_register()` call.

### Step 8: MTR tests

Under `mysql-test/suite/component_write_fence/`:

- `t/basic_fence.test` — fence table, verify INSERT fails (ER_AUDIT_API_ABORT),
  SELECT succeeds, unfence, INSERT succeeds again
- `t/multi_table.test` — fence one table, multi-table UPDATE including it fails
- `t/privilege.test` — UDFs require WRITE_FENCE_ADMIN, write_fence_check doesn't
- `t/pfs_table.test` — P_S table reflects fence state
- `t/concurrent.test` — multiple connections fence/unfence simultaneously
- `t/clear_all.test` — fence multiple tables, clear_all, verify all unblocked
- `t/active_writers.test` — start trx with INSERT (no commit), query active_writers,
  verify connection ID appears; commit, verify it disappears
- `t/cutover_e2e.test` — full cutover simulation: fence, detect writer, kill, drain, unfence

---

## Data Mover Integration Protocol

```sql
-- Phase 1: Load component (once per server)
INSTALL COMPONENT 'file://component_write_fence';

-- Phase 2: Fence tables for cutover
SELECT write_fence_set('mydb', 'orders');
SELECT write_fence_set('mydb', 'order_items');
-- New writes to these tables now get ER_AUDIT_API_ABORT

-- Phase 3: Drain active writers (control plane polls via Path B UDF)
SELECT write_fence_active_writers('mydb', 'orders');
-- Returns: [{"trx_id":12345,"connection_id":42},{"trx_id":12346,"connection_id":57}]
-- Or: [] when fully drained

-- Drain loop (pseudocode in control plane):
--   deadline = now() + X_seconds
--   while now() < deadline:
--     writers = parse_json(write_fence_active_writers('mydb', 'orders'))
--     if writers is empty: break
--     sleep(poll_interval)

-- Phase 4: Kill lingering writers after deadline
-- For each remaining writer from write_fence_active_writers():
KILL QUERY 42;
KILL QUERY 57;

-- Phase 5: Verify quiescence
SELECT write_fence_active_writers('mydb', 'orders');  -- expect []
SELECT write_fence_check('mydb', 'orders');           -- expect 1 (still fenced)

-- Phase 6: Finalize cutover (flip traffic to Server B), then cleanup
SELECT write_fence_clear_all();
-- Or: UNINSTALL COMPONENT 'file://component_write_fence';
```

### Performance characteristics of the drain query
- **No `trx_sys->mutex`** — only per-table shard latch
- **O(table_locks)** per call — typically small (equal to number of active writers)
- **Lock held for microseconds** — shard latch is released immediately after scan
- Can be called at high frequency (100ms poll interval) without contention issues
- Compare to P_S `data_locks` query: requires full SQL parse, plan, join, LIKE matching

---

## Known Limitations

| Limitation | Impact | Mitigation |
|---|---|---|
| **FK CASCADE writes bypass fence** | InnoDB CASCADE ops don't go through `open_tables()` / audit events | Data mover must fence ALL tables in the FK graph |
| **DDL not blocked** | ALTER/DROP TABLE don't generate table access events | Use MDL-based DDL protection separately if needed |
| **Error message is generic** | Client sees `ER_AUDIT_API_ABORT` not a custom "table fenced" message | Cutover is brief; data mover can interpret the error |
| **Temp tables invisible** | Temp tables don't fire audit events | Not migration candidates — acceptable |
| **XA prepared transactions** | `KILL QUERY` doesn't work on XA PREPARED state | Separate XA handling needed if workload uses XA |
| **LOCK TABLES ... WRITE** | Uses LOCK_X not LOCK_IX; event subclass is READ not INSERT | Blocked if table opened for write in audit event |

---

## Edge Cases Verified

| Scenario | Behavior | Status |
|---|---|---|
| Multi-table DML (UPDATE t1 JOIN t2) | Each table gets separate callback; if any fenced, entire stmt blocked | Correct |
| Triggers writing to fenced table | Trigger SQL goes through open_tables(), fires own events | Correct |
| TRUNCATE TABLE | Fires DELETE event via sql_truncate.cc:550 | Correct |
| LOAD DATA INFILE | Fires INSERT event (SQLCOM_LOAD) | Correct |
| Prepared statements | Event fires at execution, not preparation | Correct |
| SELECT on fenced table | READ events filtered out; not blocked | Correct |
| Component unload while fenced | deinit() clears FenceSet, lifts all fences, logs warning | Correct |

---

## Key Reference Files

### Component infrastructure
| Purpose | File |
|---|---|
| Event tracking service definition | `include/mysql/components/services/event_tracking_table_access_service.h` |
| Event data struct + subclass defs | `include/mysql/components/services/defs/event_tracking_table_access_defs.h` |
| Consumer helper macros | `include/mysql/components/util/event_tracking/event_tracking_table_access_consumer_helper.h` |
| Example consumer component | `components/test/event_tracking_test/event_tracking_consumer_a.cc:381-449` |
| Model component (structure) | `components/connection_control/connection_control.cc:590-648` |
| Model P_S table | `components/connection_control/connection_control_pfs_table.cc` |
| Model CMakeLists | `components/connection_control/CMakeLists.txt` |
| Audit dispatch + error handling | `sql/sql_audit.cc:345-372` (event_tracking_dispatch_error) |
| Where table access event fires | `sql/sql_base.cc:6031` (open_tables) |
| UDF registration service | `include/mysql/components/services/udf_registration.h` |
| Dynamic privilege service | `include/mysql/components/services/dynamic_privilege.h` |
| P_S plugin table service | `include/mysql/components/services/pfs_plugin_table_service.h` |
| RWlock service | `include/mysql/components/services/mysql_rwlock.h` |
| Component build system | `cmake/component.cmake` |

### InnoDB / Path B (active writer detection)
| Purpose | File |
|---|---|
| `struct lock_t` definition | `storage/innobase/include/lock0priv.h:137-187` |
| `struct lock_table_t` (tab_lock union member) | `storage/innobase/include/lock0priv.h:54-64` |
| `lock_get_mode()` inline | `storage/innobase/include/lock0priv.ic:94-99` |
| `lock_get_type_low()` inline | `storage/innobase/include/lock0priv.ic:45-48` |
| Existing public `lock_get_*` accessors | `storage/innobase/lock/lock0lock.cc:5651-5815` |
| `lock_table_create()` (how locks added to table) | `storage/innobase/lock/lock0lock.cc:3246-3296` |
| `Shard_latch_guard` RAII class | `storage/innobase/include/lock0guards.h:118-130` |
| `dict_table_t::locks` (per-table lock list) | `storage/innobase/include/dict0mem.h:2424` |
| `count_by_mode[]` (per-mode counter) | `storage/innobase/include/dict0mem.h:2437` |
| `dict_table_open_on_name()` | `storage/innobase/include/dict0dict.h:346-348` |
| P_S lock iterator pattern | `storage/innobase/handler/p_s.cc:252-260` |
| `Innodb_data_lock_inspector` registration | `storage/innobase/handler/ha_innodb.cc:354, 5618` |
| `thd_get_thread_id()` | `include/mysql/plugin.h:786` |
| `trx_t::mysql_thd` | `storage/innobase/include/trx0trx.h:931` |
| `trx_disconnect_from_mysql()` (XA null-out) | `storage/innobase/trx/trx0trx.cc:668` |

---

## Verification

### Fencing (component)
1. **Build**: `cmake --build . --target component_write_fence` — verify .so is produced
2. **Load**: `INSTALL COMPONENT 'file://component_write_fence'` — verify no errors
3. **Privilege**: `SELECT write_fence_set('db','t')` without WRITE_FENCE_ADMIN — expect error
4. **Grant + Fence**: `GRANT WRITE_FENCE_ADMIN ON *.* TO user; SELECT write_fence_set('db','t')` — returns 1
5. **Write blocked**: `INSERT INTO db.t VALUES(...)` from another connection — expect ER_AUDIT_API_ABORT (error 1789)
6. **Read allowed**: `SELECT * FROM db.t` — expect success
7. **P_S visible**: `SELECT * FROM performance_schema.write_fence_status` — shows fenced table
8. **Status counters**: `SHOW STATUS LIKE 'Component_write_fence%'` — blocked_writes > 0
9. **Unfence**: `SELECT write_fence_clear('db','t')` — returns 1, INSERT now succeeds
10. **Multi-table**: Fence t1, run `UPDATE t1 JOIN t2 ...` — expect blocked
11. **Unload**: `UNINSTALL COMPONENT 'file://component_write_fence'` — clean shutdown

### Active writer detection (Path B)
12. **Baseline**: Connection A starts trx, `INSERT INTO db.t VALUES(...)` (no commit).
    Connection B: `SELECT write_fence_active_writers('db','t')` — expect JSON with conn A's ID
13. **Commit clears**: Connection A commits. Connection B re-queries — expect `[]`
14. **Multiple writers**: Connections A, C, D each insert (no commit). Query returns 3 entries.
15. **Non-writer invisible**: Connection E does `SELECT * FROM db.t` (read-only).
    `write_fence_active_writers` does NOT include E (IX filter excludes IS/S locks).
16. **Table not found**: `write_fence_active_writers('nonexistent','t')` — returns NULL
17. **Kill integration**: Fence table, writer A is in-flight, `KILL QUERY <A's conn_id>`,
    re-query active_writers — A disappears.

### End-to-end cutover simulation
18. Set up table with data, start a long-running INSERT in connection A.
    Fence the table. Verify new INSERTs from connection B are blocked.
    Query active_writers — see connection A. Kill connection A. Re-query — empty.
    Unfence. Verify writes resume.

### MTR
19. `mysql-test-run --suite=component_write_fence` — all tests pass
