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
 * @file dialect.h
 * @brief What one SQL engine says differently: the dialect a backend supplies.
 * @stability free
 *
 * defiant-storage-sql writes every statement itself, from a fixed vocabulary
 * that SQLite and Postgres both speak (`CREATE TABLE IF NOT EXISTS`, `ALTER
 * TABLE ... ADD COLUMN`, `CREATE INDEX IF NOT EXISTS`, `INSERT`, `UPDATE`,
 * `DELETE`, `SELECT` with joins, `IN` and `EXISTS` subqueries, `ORDER BY`,
 * `LIMIT ? OFFSET ?`). What differs between engines is only what this struct
 * names, and nothing else may differ: a dialect is data, so two dialects can
 * be compared and a fake one can be written for a test. documentation/sql.md
 * lists every statement form.
 *
 * Nothing a caller wrote ever reaches SQL text. Identifiers are generated
 * short names, `[a-z0-9_]`, at most 24 bytes, each quoted with
 * @ref GDEFSSQL_Dialect::quote_open and @ref GDEFSSQL_Dialect::quote_close;
 * every value, the page included, is a bound parameter. So the only strings
 * here that reach the text are the dialect's own.
 *
 * What the dialect does not need, and why:
 *
 * - **No identity columns and no last-insert id.** Every key is assigned by
 *   the port (item ids, revision numbers) or by a counter row this library
 *   reads and writes in the write's own transaction (entity rows, the commit
 *   sequence, type numbers), which every engine does the same way. The one
 *   round trip a dialect can save is @ref GDEFSSQL_Dialect::returning.
 * - **No partial indexes.** Readers use the current tables, so no access path
 *   depends on `WHERE valid_to IS NULL` (AD-13).
 * - **No collation in queries.** Text is compared by bytes because every text
 *   column is declared with @ref GDEFSSQL_Dialect::text_collation, so a
 *   comparison, a sort and an index all use it without the query saying so.
 */

#ifndef GHOTI_IO_GDEFSSQL_DIALECT_H
#define GHOTI_IO_GDEFSSQL_DIALECT_H

#include <ghoti.io/defiant-storage-sql/macros.h>
#include <ghoti.io/defiant-storage-sql/core.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The physical kind of a column, which a dialect names.
 *
 * Field columns take the kind of their logical type (AD-13); the others are
 * this library's own. Integer kinds are bound and read as `int64_t`; FLOAT as
 * a `double`, exactly; TEXT and REFERENCE as UTF-8 bytes; BYTES as bytes.
 */
typedef enum GDEFSSQL_ColType {
  GDEFSSQL_COL_ID = 0,    ///< A row, item, revision, sequence or type number.
  GDEFSSQL_COL_INTEGER,   ///< GDEFSTOR_FIELD_INTEGER.
  GDEFSSQL_COL_DECIMAL,   ///< GDEFSTOR_FIELD_DECIMAL: the unscaled integer, never a float.
  GDEFSSQL_COL_FLOAT,     ///< GDEFSTOR_FIELD_FLOAT, and every order key.
  GDEFSSQL_COL_BOOLEAN,   ///< GDEFSTOR_FIELD_BOOLEAN, and the state flags: 0 or 1.
  GDEFSSQL_COL_DATE,      ///< GDEFSTOR_FIELD_DATE: days since 1970-01-01.
  GDEFSSQL_COL_DATETIME,  ///< GDEFSTOR_FIELD_DATETIME, and every time: milliseconds.
  GDEFSSQL_COL_TEXT,      ///< GDEFSTOR_FIELD_TEXT, and names, actors, reasons.
  GDEFSSQL_COL_REFERENCE, ///< GDEFSTOR_FIELD_REFERENCE, and public ids.
  GDEFSSQL_COL_BYTES,     ///< A personal ciphertext, a declaration, a draft.
  GDEFSSQL_COL_COUNT      ///< Not a kind. The size of GDEFSSQL_Dialect::type_names.
} GDEFSSQL_ColType;

/** How a dialect writes "insert, or replace the row with this key". */
typedef enum GDEFSSQL_Upsert {
  /** `INSERT ... ON CONFLICT (k, ...) DO UPDATE SET c = EXCLUDED.c, ...`
   * (SQLite 3.24 and later, Postgres). */
  GDEFSSQL_UPSERT_ON_CONFLICT = 1,
  /** `INSERT ... ON DUPLICATE KEY UPDATE c = VALUES(c), ...` (MySQL). */
  GDEFSSQL_UPSERT_ON_DUPLICATE_KEY
} GDEFSSQL_Upsert;

/**
 * @brief One engine's dialect. Size-prefixed; every string is the engine's
 * own and is copied into statements verbatim.
 *
 * The strings must outlive every backend made with the dialect (a backend
 * copies the struct, not the strings).
 */
typedef struct GDEFSSQL_Dialect {
  size_t size;                ///< `sizeof(GDEFSSQL_Dialect)`.
  const char * name;          ///< For diagnostics only, e.g. "sqlite".
  /** Written before every generated identifier, e.g. `"`. Must not contain
   * a byte of `[a-z0-9_]`. */
  const char * quote_open;
  const char * quote_close;   ///< Written after it, e.g. `"`.
  /** A bound parameter, e.g. `?`, or the prefix of a numbered one, e.g. `$`.
   * Must not contain a quote, a letter of `[a-z]`, a digit or `_`. */
  const char * placeholder;
  /** Non-zero: each parameter is @ref placeholder followed by its 1-based
   * position in the text (`$1`, `$2`). Zero: @ref placeholder alone, bound
   * by position. Either way GDEFSSQL_Executor::bind receives the position. */
  int placeholder_numbered;
  /** The column type written for each ::GDEFSSQL_ColType, e.g. `BIGINT`. It
   * must hold every value of its kind exactly: a FLOAT a double, an integer
   * kind an int64. */
  const char * type_names[GDEFSSQL_COL_COUNT];
  /** Written after the type of every TEXT and REFERENCE column so that they
   * compare and sort by bytes, e.g. `COLLATE "C"`; NULL or empty when the
   * engine already does (SQLite's BINARY). */
  const char * text_collation;
  int upsert;                 ///< A ::GDEFSSQL_Upsert.
  /** Non-zero: `UPDATE ... RETURNING c` reads a counter in the statement that
   * advances it. Zero: a second `SELECT` reads it. */
  int returning;
  /** Non-zero when the engine sorts NULL before every value in ascending
   * order (SQLite, MySQL); zero when after (Postgres). The port wants no
   * value first ascending and last descending, and the compiler writes
   * `NULLS FIRST` or `NULLS LAST` exactly where the engine's default differs. */
  int nulls_low;
  /** Non-zero when DDL may run inside the write's transaction and is undone by
   * its rollback (SQLite, Postgres). Zero is refused for now: an engine
   * without it (MySQL) needs each step run and recorded apart, which is not
   * built (backend.h). */
  int ddl_transactional;
  size_t max_identifier;      ///< Bytes of one identifier, e.g. 63. At least 32.
  /** Parameters in one statement: at least 999 (SQLite's oldest limit), so
   * that any single operand of the port's form fits. */
  size_t max_binds;
  size_t max_columns;         ///< Columns in one table. At least 16.
  const char * begin_read;    ///< Starts a read transaction: one snapshot.
  const char * begin_write;   ///< Starts the tenant's write transaction.
  const char * commit;        ///< Commits either.
  const char * rollback;      ///< Rolls either back.

  /* ---- index builds and schemas (story S4) ------------------------------
   * Every member below may be NULL (a dialect zeroed and filled as before
   * has none of them), and a NULL one means what SQLite does. */

  /** Written between `CREATE [UNIQUE] INDEX ` and `IF NOT EXISTS ` of every
   * index build, e.g. `CONCURRENTLY ` (Postgres). An index is never built in
   * a transaction (AD-15): it is recorded in its step and built after the step
   * commits (backend.h, "Index builds"). NULL or empty: a plain build. */
  const char * index_build;
  /**
   * Says whether a built index is valid: one statement taking two TEXT
   * parameters, the tenant's qualifier ("" for none) and the index's name,
   * whose one row's first column is an integer, non-zero for a valid index,
   * and which returns no row when there is no such index. Postgres leaves an
   * invalid index behind when a concurrent build fails. NULL: a build that
   * succeeded made a valid index and one that failed made none (SQLite).
   */
  const char * index_valid;
  /** Written before the qualified name of an index that @ref index_valid
   * found invalid, to drop it before it is built again, e.g. `DROP INDEX
   * CONCURRENTLY IF EXISTS `. Required with @ref index_valid. Dropping an
   * invalid index is the one drop this library ever writes: such an index
   * holds no data and serves no query. */
  const char * index_drop;
  /** Written before the quoted qualifier to make a tenant's schema, e.g.
   * `CREATE SCHEMA IF NOT EXISTS `, first in the transaction that makes the
   * tenant tables. NULL: the executor's `open` makes the store (a file). */
  const char * qualifier_create;
  /** Says whether a tenant's store exists: one statement taking two TEXT
   * parameters, the qualifier and the name of the store's version table
   * (`dss_schema`, or `dsk_schema` for a key store), that returns no row when
   * there is no such schema and otherwise one row of two integers: non-zero
   * when the version table is in it, and how many tables it holds. This
   * library does not run it on its own: an executor whose `open` cannot see a
   * missing store otherwise (a schema, where SQLite has a missing file) runs
   * it through gdefssql_store_exists(). */
  const char * store_exists;
} GDEFSSQL_Dialect;

/**
 * @brief Check that a dialect is complete and cannot corrupt the text it is
 * spliced into.
 *
 * @return ::GDEFSSQL_OK, or ::GDEFSSQL_ERR_INVALID naming nothing: a NULL or
 *   empty string, a quote or placeholder that could be read as part of a
 *   generated name, a limit below its minimum, an unknown upsert form, or a
 *   @ref GDEFSSQL_Dialect::size that is not this header's, an empty
 *   @ref GDEFSSQL_Dialect::index_valid, @ref GDEFSSQL_Dialect::index_drop,
 *   @ref GDEFSSQL_Dialect::qualifier_create or
 *   @ref GDEFSSQL_Dialect::store_exists (NULL is allowed), an
 *   @ref GDEFSSQL_Dialect::index_build with a byte of `[a-z0-9_]`, or
 *   @ref GDEFSSQL_Dialect::index_valid without
 *   @ref GDEFSSQL_Dialect::index_drop. Zero
 *   @ref GDEFSSQL_Dialect::ddl_transactional is ::GDEFSSQL_ERR_UNSUPPORTED.
 */
GDEFSSQL_API GDEFSSQL_Result gdefssql_dialect_check(
    const GDEFSSQL_Dialect * dialect);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GDEFSSQL_DIALECT_H */
