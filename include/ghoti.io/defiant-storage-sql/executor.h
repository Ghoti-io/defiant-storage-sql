/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Defiant Storage SQL.
 *
 * Ghoti.io Defiant Storage SQL is free software: you can redistribute it
 * and/or modify it under the terms of the GNU Lesser General Public License
 * version 3 as published by the Free Software Foundation.
 *
 * Ghoti.io Defiant Storage SQL is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file executor.h
 * @brief What a backend gives this library to run statements: an executor.
 * @stability free
 *
 * This library executes nothing itself. It writes a statement's text and its
 * parameters and hands them to the executor, which is the engine binding
 * (sqlite3, libpq) and nothing more: it opens a tenant's connection, prepares,
 * binds, steps, reads a column and finalizes. It decides no meaning.
 *
 * The rules an executor keeps:
 *
 * - **Tenant selection is the connection or the text, never session state**
 *   (AD-14). `open` names the tenant; a backend that keeps tenants apart by
 *   schema returns that schema as the qualifier, and this library writes it
 *   in front of every table. Nothing else is selected by state.
 * - A statement is prepared on the connection it runs on. Parameters are
 *   bound by 1-based position, every one before the first step. A column is
 *   read by 0-based position, after a step that returned a row; what it
 *   lends is valid until the next step or the finalize.
 * - **One statement at a time.** The library finalizes each statement before
 *   it prepares or execs the next on the same connection, so an executor
 *   never has two results in flight on one connection (libpq cannot).
 * - Statement texts repeat exactly for the same tenant and query shape, so an
 *   executor can cache prepared statements keyed by the text (AD-14: per
 *   tenant and query). The library calls finalize either way.
 * - An error is reported as the port's result: ::GDEFSTOR_ERR_IO for a failed
 *   engine call, ::GDEFSTOR_ERR_BUSY for a lock the engine gave up on,
 *   ::GDEFSTOR_ERR_CORRUPT for a column that cannot be read as asked,
 *   ::GDEFSTOR_ERR_OOM, ::GDEFSTOR_ERR_LIMIT. The library then rolls back and
 *   closes the connection with `discard` set.
 */

#ifndef GHOTI_IO_GDEFSSQL_EXECUTOR_H
#define GHOTI_IO_GDEFSSQL_EXECUTOR_H

#include <ghoti.io/defiant-storage-sql/macros.h>
#include <ghoti.io/defiant-storage/core.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** What a datum holds. */
typedef enum GDEFSSQL_DatumKind {
  GDEFSSQL_DATUM_NULL = 0, ///< SQL NULL. Never bound: only read.
  GDEFSSQL_DATUM_INT,      ///< GDEFSSQL_Datum::integer.
  GDEFSSQL_DATUM_REAL,     ///< GDEFSSQL_Datum::real, a finite double, exactly.
  GDEFSSQL_DATUM_TEXT,     ///< GDEFSSQL_Datum::bytes: UTF-8, no NUL.
  GDEFSSQL_DATUM_BYTES     ///< GDEFSSQL_Datum::bytes: any bytes.
} GDEFSSQL_DatumKind;

/**
 * @brief One bound parameter or one column read. Not size-prefixed: a datum.
 *
 * Bytes are borrowed: a bound datum's for the bind call (the executor copies
 * them, or keeps them until the statement is finalized, as it likes; the
 * library keeps them alive until it finalizes), a read datum's until the next
 * step or finalize.
 */
typedef struct GDEFSSQL_Datum {
  int kind;              ///< A ::GDEFSSQL_DatumKind.
  int64_t integer;       ///< INT.
  double real;           ///< REAL.
  const uint8_t * bytes; ///< TEXT, BYTES. May be NULL when @ref len is 0.
  size_t len;            ///< Length of @ref bytes.
} GDEFSSQL_Datum;

/**
 * @brief The engine binding. A backend zeroes it, sets @ref size and fills
 * every member except @ref destroy, which may be NULL.
 */
typedef struct GDEFSSQL_Executor {
  size_t size; ///< `sizeof(GDEFSSQL_Executor)`.
  void * ctx;  ///< Passed to every member. Owned by the executor.
  /** Release @ref ctx, when the backend made with this executor is
   * destroyed. May be NULL. */
  void (*destroy)(void * ctx);
  /**
   * Open a connection to @p tenant's store.
   *
   * With @p write set, the store is created when it does not exist, and
   * the connection is the one the tenant's write runs on. Without it, a
   * tenant with no store is success with `*out_conn` NULL: it holds nothing.
   *
   * @param out_qualifier Receives the name to write in front of every table
   *   of this tenant (a Postgres schema), or NULL or "" for none. It must be
   *   `[a-z][a-z0-9_]*` and within the dialect's identifier limit; it is
   *   valid until `close`.
   */
  GDEFSTOR_Result (*open)(void * ctx, const char * tenant, size_t tenant_len,
      int write, void ** out_conn, const char ** out_qualifier);
  /** Return a connection. @p discard is set after an error: the connection
   * must not be reused (AD-14). */
  void (*close)(void * ctx, void * conn, int discard);
  /** Run one statement that takes no parameter and returns no row: DDL and
   * the dialect's transaction statements. */
  GDEFSTOR_Result (*exec)(void * ctx, void * conn, const char * sql,
      size_t sql_len);
  /** Prepare one statement. */
  GDEFSTOR_Result (*prepare)(void * ctx, void * conn, const char * sql,
      size_t sql_len, void ** out_stmt);
  /** Bind parameter @p index (1-based) to @p value, never a NULL datum. */
  GDEFSTOR_Result (*bind)(void * ctx, void * stmt, unsigned index,
      const GDEFSSQL_Datum * value);
  /** Run to the next row: `*out_row` 1 with a row to read, 0 when done. */
  GDEFSTOR_Result (*step)(void * ctx, void * stmt, int * out_row);
  /**
   * Read column @p index (0-based) of the current row as @p kind (a
   * ::GDEFSSQL_DatumKind other than NULL). A NULL column is read as kind
   * NULL; a column that is not NULL and cannot be read as @p kind is
   * ::GDEFSTOR_ERR_CORRUPT.
   */
  GDEFSTOR_Result (*column)(void * ctx, void * stmt, unsigned index, int kind,
      GDEFSSQL_Datum * out);
  /** Release a prepared statement. */
  void (*finalize)(void * ctx, void * stmt);
} GDEFSSQL_Executor;

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GDEFSSQL_EXECUTOR_H */
