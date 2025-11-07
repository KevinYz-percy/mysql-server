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

#ifndef MYSQL_COM_ASYNC_MUX_INCLUDED
#define MYSQL_COM_ASYNC_MUX_INCLUDED

/**
 * @file mysql_com_async_mux.h
 *
 * MySQL Protocol Extension for Async Multiplexing
 *
 * This header defines the protocol extension for asynchronous connection
 * multiplexing, allowing multiple client sessions to share a single TCP
 * connection.
 */

#include <stdint.h>
#include "my_inttypes.h"

/**
 * Extended Protocol Header Size
 *
 * Traditional MySQL: 4 bytes (3 length + 1 seq)
 * Async Multiplexing: 4 + 8 = 12 bytes (traditional + connid)
 */
#define NET_ASYNC_MUX_HEADER_SIZE 12
#define NET_ASYNC_MUX_CONNID_SIZE 8
#define NET_ASYNC_MUX_CONNID_OFFSET 4

/**
 * Connection ID Structure (64-bit)
 *
 * Layout:
 *   Bits [0-31]:   Session ID (32-bit)
 *   Bits [32-47]:  Node ID (16-bit)
 *   Bits [48-55]:  Flags (8-bit)
 *   Bits [56-63]:  Reserved (8-bit)
 */
typedef uint64 async_mux_connid_t;

/** Invalid/uninitialized connection ID */
#define ASYNC_MUX_CONNID_INVALID 0ULL

/** Extract session ID from connid */
#define ASYNC_MUX_GET_SESSION_ID(connid) ((uint32)((connid) & 0xFFFFFFFFULL))

/** Extract node ID from connid */
#define ASYNC_MUX_GET_NODE_ID(connid) ((uint16)(((connid) >> 32) & 0xFFFFULL))

/** Extract flags from connid */
#define ASYNC_MUX_GET_FLAGS(connid) ((uint8)(((connid) >> 48) & 0xFFULL))

/** Extract reserved bits from connid */
#define ASYNC_MUX_GET_RESERVED(connid) ((uint8)(((connid) >> 56) & 0xFFULL))

/** Create connid from components */
#define ASYNC_MUX_MAKE_CONNID(session_id, node_id, flags) \
  (((uint64)(session_id) & 0xFFFFFFFFULL) |                \
   (((uint64)(node_id) & 0xFFFFULL) << 32) |               \
   (((uint64)(flags) & 0xFFULL) << 48))

/**
 * Connection ID Flags (8-bit field)
 */
enum async_mux_flags {
  /** Normal packet */
  ASYNC_MUX_FLAG_NORMAL = 0x00,

  /** High priority packet (should be processed first) */
  ASYNC_MUX_FLAG_PRIORITY = 0x01,

  /** Session initialization packet */
  ASYNC_MUX_FLAG_SESSION_INIT = 0x02,

  /** Session termination packet */
  ASYNC_MUX_FLAG_SESSION_CLOSE = 0x04,

  /** Keepalive/heartbeat packet */
  ASYNC_MUX_FLAG_KEEPALIVE = 0x08,

  /** Packet requires ordered processing */
  ASYNC_MUX_FLAG_ORDERED = 0x10,

  /** Last packet in aggregated batch */
  ASYNC_MUX_FLAG_BATCH_END = 0x20,

  /** Compressed payload */
  ASYNC_MUX_FLAG_COMPRESSED = 0x40,

  /** Reserved for future use */
  ASYNC_MUX_FLAG_RESERVED = 0x80
};

/**
 * Async Multiplexing Packet Header
 *
 * Wire format (little-endian):
 *
 *   Offset  Size  Field
 *   ------  ----  -----
 *   0       3     Packet length (excludes header)
 *   3       1     Sequence number
 *   4       8     Connection ID (connid)
 *   12      N     Payload
 */
struct AsyncMuxPacketHeader {
  /** Packet length (3 bytes in wire format) */
  uint32 length;

  /** Sequence number (1 byte) */
  uint8 seq_num;

  /** Connection/Session ID (8 bytes) */
  async_mux_connid_t connid;
};

/**
 * Parse async multiplexing packet header from buffer
 *
 * @param buffer  Input buffer (at least NET_ASYNC_MUX_HEADER_SIZE bytes)
 * @param header  Output header structure
 *
 * @return true on success, false on invalid data
 */
inline bool async_mux_parse_header(const unsigned char *buffer,
                                     AsyncMuxPacketHeader *header) {
  if (!buffer || !header) return false;

  // Parse traditional MySQL header (little-endian)
  header->length = (uint32)(buffer[0]) |
                   ((uint32)(buffer[1]) << 8) |
                   ((uint32)(buffer[2]) << 16);
  header->seq_num = buffer[3];

  // Parse connid (little-endian)
  header->connid = (uint64)(buffer[4]) |
                   ((uint64)(buffer[5]) << 8) |
                   ((uint64)(buffer[6]) << 16) |
                   ((uint64)(buffer[7]) << 24) |
                   ((uint64)(buffer[8]) << 32) |
                   ((uint64)(buffer[9]) << 40) |
                   ((uint64)(buffer[10]) << 48) |
                   ((uint64)(buffer[11]) << 56);

  return true;
}

/**
 * Write async multiplexing packet header to buffer
 *
 * @param buffer  Output buffer (at least NET_ASYNC_MUX_HEADER_SIZE bytes)
 * @param header  Input header structure
 *
 * @return Number of bytes written (NET_ASYNC_MUX_HEADER_SIZE)
 */
inline size_t async_mux_write_header(unsigned char *buffer,
                                       const AsyncMuxPacketHeader *header) {
  if (!buffer || !header) return 0;

  // Write traditional MySQL header (little-endian)
  buffer[0] = (unsigned char)(header->length & 0xFF);
  buffer[1] = (unsigned char)((header->length >> 8) & 0xFF);
  buffer[2] = (unsigned char)((header->length >> 16) & 0xFF);
  buffer[3] = header->seq_num;

  // Write connid (little-endian)
  buffer[4] = (unsigned char)(header->connid & 0xFF);
  buffer[5] = (unsigned char)((header->connid >> 8) & 0xFF);
  buffer[6] = (unsigned char)((header->connid >> 16) & 0xFF);
  buffer[7] = (unsigned char)((header->connid >> 24) & 0xFF);
  buffer[8] = (unsigned char)((header->connid >> 32) & 0xFF);
  buffer[9] = (unsigned char)((header->connid >> 40) & 0xFF);
  buffer[10] = (unsigned char)((header->connid >> 48) & 0xFF);
  buffer[11] = (unsigned char)((header->connid >> 56) & 0xFF);

  return NET_ASYNC_MUX_HEADER_SIZE;
}

/**
 * Session Context for Multiplexed Connection
 *
 * Lightweight structure representing one logical session within a
 * multiplexed TCP connection. This is much smaller than a full THD.
 */
struct AsyncMuxSessionContext {
  /** Unique session ID within this connection */
  async_mux_connid_t connid;

  /** Sequence number for this session */
  uint8 sequence_num;

  /** Authentication state */
  bool authenticated;

  /** Transaction active flag */
  bool in_transaction;

  /** Last activity timestamp (for timeout) */
  uint64 last_activity_time;

  /** Current database name */
  char current_db[256];

  /** User name */
  char user[256];

  /** Session variables (pointer to shared or private structure) */
  void *session_variables;

  /** Full THD pointer (allocated lazily during SQL execution) */
  void *thd;

  /** Read buffer for partial packets */
  unsigned char *read_buffer;
  size_t read_buffer_size;
  size_t read_buffer_used;

  /** Write buffer for outgoing packets */
  unsigned char *write_buffer;
  size_t write_buffer_size;
  size_t write_buffer_used;

  /** Reference count for safe deletion */
  uint32 ref_count;
};

/**
 * Multiplexed Connection Statistics
 */
struct AsyncMuxConnectionStats {
  /** Total sessions created */
  uint64 sessions_created;

  /** Active sessions */
  uint32 sessions_active;

  /** Total packets read */
  uint64 packets_read;

  /** Total packets written */
  uint64 packets_written;

  /** Total bytes read */
  uint64 bytes_read;

  /** Total bytes written */
  uint64 bytes_written;

  /** Aggregated packets (batched) */
  uint64 packets_aggregated;

  /** Read errors */
  uint64 read_errors;

  /** Write errors */
  uint64 write_errors;

  /** Sessions terminated */
  uint64 sessions_terminated;
};

/**
 * Configuration for Async Multiplexing
 */
struct AsyncMuxConfig {
  /** Enable async multiplexing */
  bool enabled;

  /** Number of read threads */
  uint32 read_threads;

  /** Number of write threads */
  uint32 write_threads;

  /** Max sessions per TCP connection */
  uint32 max_sessions_per_conn;

  /** Packet batch size for aggregation (bytes) */
  uint32 packet_batch_size;

  /** Packet batch timeout (milliseconds) */
  uint32 packet_batch_timeout_ms;

  /** Lock-free queue size */
  uint32 queue_size;

  /** Session idle timeout (seconds) */
  uint32 session_idle_timeout_sec;

  /** Enable packet compression */
  bool enable_compression;

  /** Compression threshold (bytes) */
  uint32 compression_threshold;
};

/** Default configuration values */
#define ASYNC_MUX_DEFAULT_READ_THREADS 4
#define ASYNC_MUX_DEFAULT_WRITE_THREADS 4
#define ASYNC_MUX_DEFAULT_MAX_SESSIONS_PER_CONN 50
#define ASYNC_MUX_DEFAULT_PACKET_BATCH_SIZE (64 * 1024)  // 64KB
#define ASYNC_MUX_DEFAULT_PACKET_BATCH_TIMEOUT_MS 1
#define ASYNC_MUX_DEFAULT_QUEUE_SIZE 10000
#define ASYNC_MUX_DEFAULT_SESSION_IDLE_TIMEOUT_SEC 3600  // 1 hour

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize async multiplexing configuration with defaults
 *
 * @param config  Configuration structure to initialize
 */
void async_mux_config_init(AsyncMuxConfig *config);

/**
 * Allocate and initialize a session context
 *
 * @param connid  Connection ID for this session
 * @return Pointer to new session context, or NULL on error
 */
AsyncMuxSessionContext *async_mux_session_create(async_mux_connid_t connid);

/**
 * Destroy a session context and free resources
 *
 * @param session  Session context to destroy
 */
void async_mux_session_destroy(AsyncMuxSessionContext *session);

/**
 * Update session activity timestamp
 *
 * @param session  Session context
 */
void async_mux_session_touch(AsyncMuxSessionContext *session);

/**
 * Check if session has timed out
 *
 * @param session     Session context
 * @param timeout_sec Timeout in seconds
 * @return true if session has timed out
 */
bool async_mux_session_is_expired(const AsyncMuxSessionContext *session,
                                    uint32 timeout_sec);

#ifdef __cplusplus
}
#endif

#endif  // MYSQL_COM_ASYNC_MUX_INCLUDED
