/* Copyright (c) 2025, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef ASYNC_MULTIPLEXING_DESIGN_INCLUDED
#define ASYNC_MULTIPLEXING_DESIGN_INCLUDED

/**
 * @file async_multiplexing_design.h
 *
 * ASYNC MULTIPLEXING NETWORK MODEL FOR DISTRIBUTED MYSQL
 * =======================================================
 *
 * This design addresses scalability challenges in distributed database systems
 * by implementing an asynchronous multiplexing network model similar to TDSQL
 * (as described in VLDB 2024 paper: "TDSQL: A Distributed Cloud Database System").
 *
 * PROBLEM STATEMENT
 * -----------------
 * Traditional thread-per-connection or thread-pool models face:
 *
 * 1. **Port Exhaustion**: Each connection needs access to all data nodes
 *    - Example: 1,200 routers × 50 data nodes = 60,000 connections
 *
 * 2. **Network Overhead**: Numerous small packets cause high latency and CPU strain
 *
 * 3. **Thread Switching**: Performance degrades after 20,000+ concurrent connections
 *
 * 4. **Memory Overhead**: Each connection consumes ~3MB:
 *    - THD structure: ~1MB
 *    - Network buffers: ~512KB
 *    - Join buffer: 256KB (default join_buffer_size)
 *    - Sort buffer: 256KB (default sort_buffer_size)
 *    - Read buffer: 128KB (default read_buffer_size)
 *    - Prepared statement cache, stored procedure cache, etc.
 *
 * 5. **Scalability**: Resource overhead grows exponentially with more nodes
 *
 * SOLUTION: ASYNC MULTIPLEXING
 * -----------------------------
 *
 * 1. **Protocol Transformation**
 *    - Add `connid` field to protocol header (8 bytes)
 *    - Multiplex multiple client sessions over single TCP connection
 *    - Reduce connections per data node from 1,200 to ~24
 *
 * 2. **Network Module Refactoring**
 *    - Non-blocking I/O based on epoll (Linux) / kqueue (BSD/macOS)
 *    - Dedicated read/write threads separate from SQL worker threads
 *    - Lock-free queues for I/O → worker communication
 *    - Packet aggregation: Batch multiple small packets
 *    - Network I/O overhead < 5% of total CPU
 *
 * 3. **Memory Optimization**
 *    - Shared caches for stored procedures, prepared statements
 *    - Memory pools for join/sort buffers
 *    - Lazy THD allocation (only when executing SQL)
 *    - Reduce per-session memory from 3MB to ~300KB
 *
 * ARCHITECTURE OVERVIEW
 * ---------------------
 *
 *                          ┌─────────────────┐
 *                          │  Client Apps    │
 *                          │ (1000s of       │
 *                          │  connections)   │
 *                          └────────┬────────┘
 *                                   │
 *                          ┌────────▼────────┐
 *                          │   Router Node   │
 *                          │  (Async Mux)    │
 *                          └────────┬────────┘
 *                                   │
 *                      ┌────────────┼────────────┐
 *                      │            │            │
 *              ┌───────▼──────┐ ┌──▼──────┐ ┌──▼──────┐
 *              │  Data Node 1 │ │  DN 2   │ │  DN N   │
 *              │  (24 conns)  │ │(24 conn)│ │(24 conn)│
 *              └──────────────┘ └─────────┘ └─────────┘
 *
 *
 * THREAD MODEL
 * ------------
 *
 *   Clients
 *     │
 *     ▼
 *  ┌──────────────────────────────────────────┐
 *  │        Connection Acceptor Thread         │
 *  │  (Traditional listener, poll-based)       │
 *  └────────────────┬─────────────────────────┘
 *                   │ New TCP connection
 *                   ▼
 *  ┌──────────────────────────────────────────┐
 *  │     Async Connection Dispatcher          │
 *  │  - Maps client → multiplexed backend     │
 *  │  - Assigns connid to each session        │
 *  └────────────────┬─────────────────────────┘
 *                   │
 *     ┌─────────────┼─────────────┐
 *     │                           │
 *     ▼                           ▼
 *  ┌──────────────┐       ┌──────────────┐
 *  │ Read Thread  │       │ Write Thread │
 *  │   (epoll)    │       │   (epoll)    │
 *  │              │       │              │
 *  │ - EPOLLIN    │       │ - EPOLLOUT   │
 *  │ - Read pkts  │       │ - Write pkts │
 *  │ - Parse hdr  │       │ - Aggregate  │
 *  │ - Route by   │       │ - Batch      │
 *  │   connid     │       │              │
 *  └──────┬───────┘       └──────▲───────┘
 *         │                      │
 *         │  Lock-free queue     │
 *         │                      │
 *         ▼                      │
 *  ┌──────────────────────────────────────────┐
 *  │       SQL Worker Thread Pool             │
 *  │  (Thread pool, NOT 1-per-connection)     │
 *  │                                           │
 *  │  - Parse SQL                              │
 *  │  - Execute queries                        │
 *  │  - Transaction management                 │
 *  │  - Share memory resources                 │
 *  └───────────────────────────────────────────┘
 *
 *
 * PROTOCOL EXTENSION
 * ------------------
 *
 * Traditional MySQL protocol packet:
 *
 *   +------------------+------------------+
 *   | Header (4 bytes) | Payload          |
 *   +------------------+------------------+
 *   | [0-2] length     | SQL command      |
 *   | [3]   seq_num    | or data          |
 *   +------------------+------------------+
 *
 * Extended async multiplexing protocol:
 *
 *   +------------------+------------------+------------------+
 *   | Header (4 bytes) | ConnID (8 bytes) | Payload          |
 *   +------------------+------------------+------------------+
 *   | [0-2] length     | [0-7] connid     | SQL command      |
 *   | [3]   seq_num    | (session ID)     | or data          |
 *   +------------------+------------------+------------------+
 *
 * ConnID structure (64-bit):
 *   - [0-31]  Session ID (4 billion sessions)
 *   - [32-47] Node ID (65K nodes)
 *   - [48-55] Flags (routing hints, priority, etc.)
 *   - [56-63] Reserved
 *
 *
 * MEMORY OPTIMIZATION STRATEGIES
 * -------------------------------
 *
 * 1. **Shared Resource Caches**
 *    - Global prepared statement cache (LRU)
 *    - Global stored procedure cache (LRU)
 *    - Shared table definition cache
 *
 * 2. **Memory Pools**
 *    - Pre-allocated join buffer pool
 *    - Pre-allocated sort buffer pool
 *    - Buffer checkout/return mechanism
 *
 * 3. **Lazy THD Allocation**
 *    - Lightweight session context (~50KB)
 *    - Full THD only when executing SQL
 *    - THD recycling pool
 *
 * 4. **Connection Reuse Limits**
 *    - Max sessions per TCP connection: configurable (default 50)
 *    - Max memory per TCP connection: configurable (default 150MB)
 *    - Graceful overflow handling
 *
 *
 * KEY DATA STRUCTURES
 * -------------------
 *
 * 1. AsyncMultiplexedConnection
 *    - Represents one TCP connection
 *    - Maintains map of connid → session state
 *    - Handles packet routing
 *
 * 2. SessionContext
 *    - Lightweight session state (~50KB)
 *    - Transaction state, variables, authentication
 *    - Reference to shared resources
 *
 * 3. LockFreeQueue<T>
 *    - SPSC or MPMC queue for I/O thread communication
 *    - Based on ring buffer or linked list
 *    - Wait-free enqueue/dequeue when possible
 *
 * 4. EpollEventLoop
 *    - Epoll wrapper for read/write threads
 *    - Event registration, monitoring, dispatch
 *
 * 5. PacketAggregator
 *    - Batches small packets together
 *    - Adaptive batching based on packet size/frequency
 *    - Flush on timeout or buffer full
 *
 *
 * IMPLEMENTATION PHASES
 * ---------------------
 *
 * Phase 1: Protocol Extension
 *   - Extend NET structure for connid
 *   - Modify read/write packet functions
 *   - Add connid negotiation during handshake
 *
 * Phase 2: Non-blocking VIO
 *   - Add VIO_TYPE_ASYNC_TCP
 *   - Implement vio_epoll.cc with epoll support
 *   - Non-blocking read/write with partial I/O handling
 *
 * Phase 3: Async Connection Handler
 *   - connection_handler_async_multiplexing.cc
 *   - Read/write event loops
 *   - Lock-free queue implementation
 *
 * Phase 4: Session Multiplexing
 *   - AsyncMultiplexedConnection class
 *   - SessionContext management
 *   - Packet routing by connid
 *
 * Phase 5: Memory Optimization
 *   - Shared resource caches
 *   - Memory pools
 *   - THD recycling
 *
 * Phase 6: Configuration & Testing
 *   - System variables for tuning
 *   - Performance benchmarks
 *   - Stress testing
 *
 *
 * CONFIGURATION VARIABLES
 * -----------------------
 *
 * - async_multiplexing_enabled: Enable async multiplexing (default: OFF)
 * - async_multiplexing_read_threads: Number of read threads (default: 4)
 * - async_multiplexing_write_threads: Number of write threads (default: 4)
 * - async_multiplexing_max_sessions_per_conn: Max sessions per TCP (default: 50)
 * - async_multiplexing_packet_batch_size: Packet aggregation size (default: 64KB)
 * - async_multiplexing_packet_batch_timeout: Batch flush timeout ms (default: 1)
 * - async_multiplexing_queue_size: Lock-free queue size (default: 10000)
 * - shared_prepared_stmt_cache_size: Global prepared stmt cache (default: 100MB)
 * - shared_stored_proc_cache_size: Global stored proc cache (default: 100MB)
 * - memory_pool_join_buffer_size: Join buffer pool size (default: 1GB)
 * - memory_pool_sort_buffer_size: Sort buffer pool size (default: 1GB)
 *
 *
 * PERFORMANCE TARGETS
 * -------------------
 *
 * Based on TDSQL benchmarks (VLDB 2024 paper):
 *
 * - Reduce connections per data node: 1,200 → 24 (50x reduction)
 * - Support 100,000+ concurrent sessions per router
 * - Network I/O overhead: < 5% of total CPU
 * - Memory per session: < 500KB (vs 3MB traditional)
 * - Linear scalability: No exponential growth with node count
 * - Latency overhead: < 100μs for multiplexing/demultiplexing
 *
 *
 * COMPATIBILITY
 * -------------
 *
 * - Backward compatible: Traditional connections still supported
 * - Opt-in via configuration or connection attribute
 * - Client driver changes required for full multiplexing
 * - Router-to-shard multiplexing works transparently
 *
 *
 * REFERENCES
 * ----------
 *
 * [1] Chen et al., "TDSQL: A Distributed Cloud Database System",
 *     VLDB 2024, Vol 17, pp 3869-3881
 *     https://www.vldb.org/pvldb/vol17/p3869-chen.pdf
 *
 * [2] Vitess: https://vitess.io/
 *     - Similar connection pooling approach
 *     - Protocol multiplexing via VTGate
 *
 * [3] MySQL Thread Pool Plugin
 *     - Connection pooling but still 1 connection = 1 session
 *     - No protocol-level multiplexing
 */

#endif  // ASYNC_MULTIPLEXING_DESIGN_INCLUDED
