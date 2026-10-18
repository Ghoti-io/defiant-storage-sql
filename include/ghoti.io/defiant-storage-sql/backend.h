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
 * executor's qualifier) there are six tables of this library's own:
 *
 * | Table | Holds |
 * | --- | --- |
 * | `dss_schema` | the layout version (1) |
 * | `dss_counter` | the next entity row, type number and commit sequence |
 * | `dss_type` | each declaration: its name, its number, its text |
 * | `dss_name` | the mapping from each generated name to what it stores |
 * | `dss_entity` | derived: public id to row and type number |
 * | `dss_draft` | one draft per public id |
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
 * field's subject column has `tN_jG_C` on `tN_gG`. Nothing is ever dropped or
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
 * transactional DDL). The first write of a tenant in a backend's life first
 * creates the six tenant tables in a transaction of their own.
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

/** The layout version `dss_schema` records. A store holding another is refused. */
#define GDEFSSQL_SCHEMA_VERSION 1

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
 * @param out Receives the backend, to hand to gdefstor_store_create_model().
 * @return ::GDEFSSQL_OK, ::GDEFSSQL_ERR_INVALID (a dialect or an executor that
 *   is incomplete), ::GDEFSSQL_ERR_UNSUPPORTED (a dialect without
 *   transactional DDL), or ::GDEFSSQL_ERR_OOM.
 */
GDEFSSQL_API GDEFSSQL_Result gdefssql_model_backend(
    const GDEFSSQL_Dialect * dialect, const GDEFSSQL_Executor * executor,
    const GDEFSSQL_Allocator * allocator, GDEFSTOR_ModelBackend * out);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GDEFSSQL_BACKEND_H */
