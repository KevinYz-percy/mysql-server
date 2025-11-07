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

/**
 * @file mysql_com_async_mux.cc
 *
 * Implementation of async multiplexing protocol functions
 */

#include "mysql_com_async_mux.h"

#include <string.h>
#include <time.h>
#include "my_sys.h"
#include "mysql_time.h"

/**
 * Get current timestamp in milliseconds
 */
static uint64 get_current_time_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64)ts.tv_sec * 1000 + (uint64)ts.tv_nsec / 1000000;
}

void async_mux_config_init(AsyncMuxConfig *config) {
  if (!config) return;

  config->enabled = false;
  config->read_threads = ASYNC_MUX_DEFAULT_READ_THREADS;
  config->write_threads = ASYNC_MUX_DEFAULT_WRITE_THREADS;
  config->max_sessions_per_conn = ASYNC_MUX_DEFAULT_MAX_SESSIONS_PER_CONN;
  config->packet_batch_size = ASYNC_MUX_DEFAULT_PACKET_BATCH_SIZE;
  config->packet_batch_timeout_ms = ASYNC_MUX_DEFAULT_PACKET_BATCH_TIMEOUT_MS;
  config->queue_size = ASYNC_MUX_DEFAULT_QUEUE_SIZE;
  config->session_idle_timeout_sec = ASYNC_MUX_DEFAULT_SESSION_IDLE_TIMEOUT_SEC;
  config->enable_compression = false;
  config->compression_threshold = 1024;  // 1KB
}

AsyncMuxSessionContext *async_mux_session_create(async_mux_connid_t connid) {
  AsyncMuxSessionContext *session = (AsyncMuxSessionContext *)my_malloc(
      PSI_NOT_INSTRUMENTED, sizeof(AsyncMuxSessionContext),
      MYF(MY_WME | MY_ZEROFILL));

  if (!session) return nullptr;

  session->connid = connid;
  session->sequence_num = 0;
  session->authenticated = false;
  session->in_transaction = false;
  session->last_activity_time = get_current_time_ms();
  session->current_db[0] = '\0';
  session->user[0] = '\0';
  session->session_variables = nullptr;
  session->thd = nullptr;
  session->ref_count = 1;

  // Allocate read buffer (16KB initial size)
  session->read_buffer_size = 16 * 1024;
  session->read_buffer = (unsigned char *)my_malloc(
      PSI_NOT_INSTRUMENTED, session->read_buffer_size, MYF(MY_WME));
  session->read_buffer_used = 0;

  if (!session->read_buffer) {
    my_free(session);
    return nullptr;
  }

  // Allocate write buffer (16KB initial size)
  session->write_buffer_size = 16 * 1024;
  session->write_buffer = (unsigned char *)my_malloc(
      PSI_NOT_INSTRUMENTED, session->write_buffer_size, MYF(MY_WME));
  session->write_buffer_used = 0;

  if (!session->write_buffer) {
    my_free(session->read_buffer);
    my_free(session);
    return nullptr;
  }

  return session;
}

void async_mux_session_destroy(AsyncMuxSessionContext *session) {
  if (!session) return;

  // Decrement reference count
  if (--session->ref_count > 0) {
    return;  // Still referenced
  }

  // Free buffers
  if (session->read_buffer) {
    my_free(session->read_buffer);
  }
  if (session->write_buffer) {
    my_free(session->write_buffer);
  }

  // Free session variables if allocated
  if (session->session_variables) {
    my_free(session->session_variables);
  }

  // Note: THD should be cleaned up separately before destroying session

  my_free(session);
}

void async_mux_session_touch(AsyncMuxSessionContext *session) {
  if (!session) return;
  session->last_activity_time = get_current_time_ms();
}

bool async_mux_session_is_expired(const AsyncMuxSessionContext *session,
                                    uint32 timeout_sec) {
  if (!session) return true;

  uint64 current_time = get_current_time_ms();
  uint64 timeout_ms = (uint64)timeout_sec * 1000;

  return (current_time - session->last_activity_time) > timeout_ms;
}
