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
 * @file backend.h
 * @brief The entity backend (model_backend.h) for every SQL engine.
 * @stability free
 *
 * A SQL backend (defiant-storage-sqlite, defiant-storage-postgres) is a
 * dialect (dialect.h) and an executor (executor.h). This library is the rest:
 * it implements every member of GDEFSTOR_ModelBackend by writing statements
 * and running them through the executor, so the schema, the statements and
 * the query compiler exist once and cannot drift between engines.
 *
 * ## The physical schema
 *
 * Every table name is generated: a prefix and a number, never a byte a caller
 * wrote. Per tenant (one database file, or one schema named by the
 * executor's qualifier) there are ten tables of this library's own:
 *
 * | Table | Holds |
 * | --- | --- |
 * | `dss_schema` | the layout version (1) |
 * | `dss_counter` | the next entity row, type number and commit sequence |
 * | `dss_type` | each declaration: its name, its number, its text |
 * | `dss_name` | the mapping from each generated name to what it stores |
 * | `dss_entity` | derived: public id to row and type number |
 * | `dss_draft` | one draft per public id |
 * | `dss_record` | records (story S6a) |
 * | `dss_seq` | counters, and each outbox stream's last sequence |
 * | `dss_outbox` | outbox rows, by stream and sequence |
 * | `dss_outbox_id` | each outbox row by public id |
 *
 * and for each declared type, numbered `N` in the order types are first
 * declared, the tables of AD-14:
 *
 * | Table | Holds | Key |
 * | --- | --- | --- |
 * | `tN_e` | entity: row, public id, creator, created, revision, last item | row |
 * | `tN_s` | state history: row, seq, published, deleted, actor, when | row, seq |
 * | `tN_r` | revisions: row, revision, commit seq, who, when, why | row, revision |
 * | `tN_gG` | item versions of group G: entity, item, valid_from, valid_to, parent, ord, `vC`... | entity, item, valid_from |
 * | `tN_n` | derived: each entity's newest state | row |
 * | `tN_cG` | derived: the open versions of group G | entity, item |
 *
 * Group 0 is the root group. Column `vC` holds the group's column C (the
 * port's stable column number); a personal field adds `vCs`, its subject. An
 * indexed field has the index `tN_iG_C` on `tN_cG (vC, entity)`; a personal
 * field's subject column has `tN_jG_C` on `tN_gG`, which serves the subject
 * lookup. A type with a personal field also has the blind table `tN_b`
 * (AD-14): one row per value version written with a MAC (entity, item,
 * valid_from, field, subject, MAC, key id, normalization version), indexed by
 * `tN_bm (fld, mac)` for blind equality and the history lookup and by
 * `tN_bs (subject)` for erasure. It is truth while its subject exists:
 * `rebuild` leaves it alone, and the MAC is in no other table. Records,
 * counters and the outbox (story S6a) are four more tenant tables, `dss_record`,
 * `dss_seq`, `dss_outbox` and `dss_outbox_id`, truth that `rebuild` leaves
 * alone. Nothing is ever dropped or
 * retyped: a schema step creates a table, adds a nullable column or creates
 * an index, and each is recorded in `dss_name`, so a step already taken is
 * never taken again (documentation/sql.md has every column and index).
 *
 * ## Transactions
 *
 * `begin` opens the tenant's write connection and runs the dialect's
 * `begin_write`; reads with no transaction open a read connection and run
 * `begin_read`, so a query's total and its page come from one snapshot.
 * Schema steps of `type_store` run inside the write (the dialect must have
 * transactional DDL). Every write sets a savepoint after `begin_write`: an
 * abort of a write that took a counter value rolls back to it, keeps the
 * counter's floor and commits only that (documentation/sql.md, "Counters and
 * aborts"). The first write of a tenant in a backend's life first
 * creates the six tenant tables in a transaction of their own (and, with
 * GDEFSSQL_Dialect::qualifier_create, the tenant's schema, first).
 *
 * ## Index builds
 *
 * No `CREATE INDEX` runs inside a transaction (AD-15). A step that needs an
 * index records it in `dss_name` as pending, in the step's transaction, and
 * the index is built after that transaction commits, on the same connection:
 * with GDEFSSQL_Dialect::index_build (`CREATE INDEX CONCURRENTLY` on
 * Postgres), or plainly (SQLite). It is marked built, in a short transaction
 * of its own, only once it exists and, with GDEFSSQL_Dialect::index_valid, is
 * valid. An invalid index, which a failed concurrent build leaves behind, is
 * dropped with GDEFSSQL_Dialect::index_drop and built again; that is the one
 * drop this library writes. A build that fails leaves its index pending and
 * the commit that ran it succeeds: the declaration is committed. Pending
 * builds are tried again after the backend's next commit of that tenant, and
 * on the first write of the tenant in a later backend's life. Until an index
 * is built its field reads ::GDEFSTOR_INDEX_INDEXING through
 * gdefstor_type_index_state(), and every query on it answers the same, by
 * reading the table.
 *
 * ## What it refuses
 *
 * A query the compiler cannot express exactly is ::GDEFSTOR_ERR_UNSUPPORTED,
 * never an answer that differs from gdefstor_model_scan(); a statement that
 * would need more parameters than the dialect allows is ::GDEFSTOR_ERR_LIMIT.
 */

#ifndef GHOTI_IO_GDEFSSQL_BACKEND_H
#define GHOTI_IO_GDEFSSQL_BACKEND_H

#include <ghoti.io/defiant-storage-sql/macros.h>
#include <ghoti.io/defiant-storage-sql/allocator.h>
#include <ghoti.io/defiant-storage-sql/core.h>
#include <ghoti.io/defiant-storage-sql/dialect.h>
#include <ghoti.io/defiant-storage-sql/executor.h>
#include <ghoti.io/defiant-storage/model_backend.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The layout version `dss_schema` records. A store holding another is refused.
 * 2 since the language axis (story S6a2): every item, current and blind row
 * holds its language and every revision row its languages, so a store of
 * layout 1 has a shape this library no longer reads and is refused as older
 * (::GDEFSTOR_ERR_FORMAT). Nothing was released, so nothing is migrated.
 */
#define GDEFSSQL_SCHEMA_VERSION 2

/**
 * @brief Make the entity backend for one engine.
 *
 * @param dialect Checked with gdefssql_dialect_check() and copied (its strings
 *   are borrowed for the backend's life).
 * @param executor Copied. On success the backend owns its `ctx` and calls its
 *   `destroy` from GDEFSTOR_ModelBackend::destroy; on failure nothing is
 *   taken.
 * @param allocator NULL uses the default. It must be the allocator the store
 *   is created with: the port frees what `type_load` returns with it.
 * @param out Receives the backend, to hand to gdefstor_store_open().
 * @return ::GDEFSSQL_OK, ::GDEFSSQL_ERR_INVALID (a dialect or an executor that
 *   is incomplete), ::GDEFSSQL_ERR_UNSUPPORTED (a dialect without
 *   transactional DDL), or ::GDEFSSQL_ERR_OOM.
 */
GDEFSSQL_API GDEFSSQL_Result gdefssql_model_backend(
    const GDEFSSQL_Dialect * dialect, const GDEFSSQL_Executor * executor,
    const GDEFSSQL_Allocator * allocator, GDEFSTOR_ModelBackend * out);

/** Which layout gdefssql_store_version() reads. */
typedef enum GDEFSSQL_StoreKind {
  GDEFSSQL_STORE_DATA = 1, ///< A tenant's data store: `dss_schema`.
  GDEFSSQL_STORE_KEYS      ///< A tenant's key store (keystore.h): `dsk_schema`.
} GDEFSSQL_StoreKind;

/**
 * @brief Read the layout version a tenant's store records, so that an
 * executor can refuse a store it must not serve before it hands a connection
 * back from `open`.
 *
 * The backends check the version on the first write of a tenant; a read does
 * not, and would read a newer layout as if it were this one. An executor that
 * can see from the engine's catalog that the version table exists (for
 * SQLite, a pragma) calls this on each new connection and refuses a version
 * that is not ::GDEFSSQL_SCHEMA_VERSION (or ::GDEFSSQL_KEYSTORE_VERSION).
 *
 * It runs one `SELECT` through @p executor on @p conn, outside any
 * transaction the caller has not opened; nothing is written.
 *
 * @param conn A connection @p executor opened (inside its `open` is allowed).
 * @param qualifier What that `open` would return as the qualifier, or NULL.
 * @param kind A ::GDEFSSQL_StoreKind.
 * @param out_version Receives the recorded version, or 0 when the table holds
 *   no row. Not written on failure.
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_INVALID (a NULL argument, an unknown
 *   kind, a dialect that gdefssql_dialect_check() refuses), ::GDEFSTOR_ERR_OOM,
 *   ::GDEFSTOR_ERR_CORRUPT (a version that is not a positive integer), or the
 *   executor's error (a missing table is the engine's error).
 */
GDEFSSQL_API GDEFSTOR_Result gdefssql_store_version(
    const GDEFSSQL_Dialect * dialect, const GDEFSSQL_Executor * executor,
    void * conn, const char * qualifier, int kind, int64_t * out_version);

/**
 * @brief Whether a tenant's store exists, for an executor whose `open` cannot
 * see a missing store any other way (a Postgres schema), so that a read of a
 * tenant with no store is answered by no connection, as executor.h asks.
 *
 * It runs GDEFSSQL_Dialect::store_exists through @p executor on @p conn, with
 * the qualifier and the version table of @p kind as its parameters, outside
 * any transaction the caller has not opened; nothing is written. An executor
 * that finds the store calls gdefssql_store_version() next and refuses a
 * version that is not this library's.
 *
 * @param qualifier What `open` would return as the qualifier: required.
 * @param kind A ::GDEFSSQL_StoreKind.
 * @param out_exists Receives 1 when the store's version table exists, 0 when
 *   there is no schema or an empty one (the first write makes the store in
 *   it). Not written on failure.
 * @return ::GDEFSTOR_OK, ::GDEFSTOR_ERR_FORMAT (a schema that holds tables
 *   but not this kind's version table: not a store of this kind, never to be
 *   written in), ::GDEFSTOR_ERR_INVALID (a NULL argument, an unknown kind, a
 *   qualifier that is not a generated name, a dialect that
 *   gdefssql_dialect_check() refuses or that has no store_exists), or the
 *   executor's error.
 */
GDEFSSQL_API GDEFSTOR_Result gdefssql_store_exists(
    const GDEFSSQL_Dialect * dialect, const GDEFSSQL_Executor * executor,
    void * conn, const char * qualifier, int kind, int * out_exists);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GDEFSSQL_BACKEND_H */
