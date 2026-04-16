# Active Writer Detection: Latch Strategy & Safety Analysis

## Overview

`lock_table_get_active_writers()` scans `dict_table_t::locks` to enumerate in-flight
write transactions on a specific table. It deliberately **avoids the global
`trx_sys->mutex`** by using only per-table shard latches.

---

## What We Acquire

```
locksys::Shard_latch_guard{*table}
  ├── Global_shared_latch_guard    →  S-latch on lock_sys->latches.global_latch
  └── Shard_naked_latch_guard      →  Mutex on shard(table.id)
```

| Latch | Type | Scope | Contention |
|---|---|---|---|
| `lock_sys->latches.global_latch` | **Shared (S)** | Server-wide, but shared — unlimited concurrent readers | Near-zero. Only blocked by exclusive holders (lock_sys resize — extremely rare) |
| `shard(table.id)` | **Exclusive mutex** | One of ~512 shards, selected by table ID hash | Very low. Only threads locking/unlocking rows on tables in the same shard contend |

**Total hold time**: microseconds (linked list walk, no I/O).

## What We Do NOT Acquire

| Lock | Why not needed |
|---|---|
| **`trx_sys->mutex`** | We iterate `table->locks`, not `trx_sys->rw_trx_list`. The table lock list is protected by the shard latch, not by `trx_sys->mutex`. |
| **`trx->mutex`** | We read two fields: `trx->id` (immutable) and `trx->mysql_thd` (stable for active non-XA transactions). We do not modify or inspect mutable trx state. |
| **`dict_sys->mutex`** | Acquired internally by `dict_table_open_on_name()` during table lookup, released before we scan locks. Not held during the walk. |

---

## Why This Is Safe (Non-XA Transactions)

For a regular (non-XA) transaction:

```
Connection creates trx  →  trx->mysql_thd = this_THD     (set once)
     ...DML executes...  →  trx->mysql_thd unchanged      (stable)
Commit / Rollback        →  IX lock removed from table->locks
                            trx returned to pool
                            trx->mysql_thd cleared AFTER lock release
```

Key observations:
- `trx->id` is assigned in `trx_start_low()` and **never changes** — always safe to read.
- `trx->mysql_thd` is set at transaction creation and remains stable while the transaction
  is active and holds locks. It is only cleared during `trx_free_low()`, which happens
  **after** the IX lock is removed from `table->locks` via `lock_trx_release_locks()`.
- Therefore: if we see a lock in `table->locks`, the owning `trx->mysql_thd` is guaranteed
  to be valid. The lock's presence in the list is proof the transaction is still alive.

**No global mutex needed. No race condition possible for non-XA transactions.**

---

## Where This Does NOT Work: XA Prepared Transactions

XA (distributed transactions) have a unique lifecycle:

```
XA START → DML → XA END → XA PREPARE → [connection disconnects] → XA COMMIT (from another connection)
                                              |
                              trx_disconnect_from_mysql()
                                sets trx->mysql_thd = nullptr
                                under trx_sys->mutex
                                (trx0trx.cc:668)
```

After `XA PREPARE`, the transaction can **outlive its original connection**. When the
connection disconnects, `trx_disconnect_from_mysql()` sets `trx->mysql_thd = nullptr`
under protection of `trx_sys->mutex`. The IX lock remains in `table->locks` because
the transaction hasn't committed yet.

### The race window (XA only):

```
Our thread (scanner):              Disconnecting thread:
─────────────────────              ─────────────────────
read lock->trx->mysql_thd
  → gets 0x7f00001234 (valid)
                                   trx_sys_mutex_enter()
                                   trx->mysql_thd = nullptr
                                   // THD may be freed
                                   trx_sys_mutex_exit()
thd_get_thread_id(0x7f00001234)
  → USE-AFTER-FREE (theoretically)
```

Without `trx_sys->mutex`, there is a narrow window where we read `mysql_thd`, then it
gets nulled and the THD object freed, then we dereference the stale pointer.

### Practical risk assessment:

| Factor | Assessment |
|---|---|
| Window size | Nanoseconds (between our read and our dereference) |
| Trigger condition | XA PREPARE + disconnect happening at the exact moment we scan |
| Frequency in data mover workloads | Near-zero. Data mover tables use regular transactions, not XA. |
| Consequence if hit | Crash (use-after-free) or garbage connection_id |

### Mitigations (choose one):

| Option | Overhead | Safety | Recommendation |
|---|---|---|---|
| **A. Accept the risk** | Zero | 99.99% safe (only XA-prepared disconnect) | OK if workload never uses XA |
| **B. Hold `trx_sys->mutex` only for the `mysql_thd` read** | Low (acquire/release per writer, not for entire walk) | 100% safe | Best balance |
| **C. Read `trx->id` only, map to connection_id via I_S query** | Medium (extra SQL roundtrip) | 100% safe, no mutex | Simpler but slower |
| **D. Use `std::atomic` read on `mysql_thd`** | Zero | Prevents torn read but not use-after-free | Insufficient alone |

**Recommended: Option B** — acquire `trx_sys->mutex` narrowly around each `mysql_thd` read:

```cpp
for each lock in table->locks:
    if not IX/X or is waiting: skip
    trx_id = lock->trx->id            // safe, no mutex needed

    trx_sys_mutex_enter()
    thd = lock->trx->mysql_thd        // safe under mutex
    conn_id = thd ? thd_get_thread_id(thd) : 0
    trx_sys_mutex_exit()

    writers.push_back({trx_id, conn_id})
```

This holds `trx_sys->mutex` for **nanoseconds per writer** (one pointer read + one
function call), not for the entire list walk. The shard latch protects the list
structure; `trx_sys->mutex` protects only the THD pointer dereference.

---

## Other Edge Cases

| Scenario | Behavior | Impact |
|---|---|---|
| **Transaction commits during our scan** | IX lock is removed from `table->locks` by `lock_table_dequeue()`. Requires shard latch, which we hold. So the removal waits until we finish. **No race.** | Safe |
| **New transaction acquires IX during our scan** | `lock_table_create()` appends to `table->locks`. Requires shard latch, which we hold. Insertion waits. We may or may not see this writer depending on our position in the list. | Safe (next poll catches it) |
| **Transaction rolled back by KILL QUERY** | InnoDB rollback releases IX lock via `lock_trx_release_locks()`. Requires shard latch. Waits for our scan to finish, then removes the lock. Next poll won't see this writer. | Safe |
| **Table eviction from dict cache** | `dict_table_open_on_name()` increments `n_ref_count`. Table cannot be evicted while we hold the reference. `dict_table_close()` decrements it after we finish. | Safe |
| **DDL (ALTER TABLE) on the table** | DDL acquires MDL, not InnoDB table locks. Does not affect `table->locks`. Our scan is unaware of DDL. | Not applicable |
| **AUTO_INC lock on the table** | Mode is `LOCK_AUTO_INC`, not `LOCK_IX`/`LOCK_X`. Filtered out by our mode check. | Correct |

---

## Summary

```
┌─────────────────────────────────────────────────────┐
│           lock_table_get_active_writers()            │
├─────────────────────────────────────────────────────┤
│  Acquires:  per-table shard latch (microseconds)    │
│  Does NOT:  hold trx_sys->mutex for list walk       │
│  Reads:     lock->trx->id       (immutable, safe)   │
│             lock->trx->mysql_thd (see below)        │
├─────────────────────────────────────────────────────┤
│  Non-XA transactions:  100% safe, zero race risk    │
│  XA-prepared + disconnect: narrow race on mysql_thd │
│    → Fix: hold trx_sys->mutex per-writer (Option B) │
│    → Cost: nanoseconds per writer, not per scan     │
├─────────────────────────────────────────────────────┤
│  Safe against concurrent commit, rollback, KILL,    │
│  new writer arrival, table eviction.                │
│  Shard latch serializes all lock list mutations.    │
└─────────────────────────────────────────────────────┘
```
