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
 * @file keystore.h
 * @brief The key store backend (defiant-storage's keystore.h) for every SQL
 * engine.
 * @stability free
 *
 * The key store is a port of its own and never part of a data store (AD-13,
 * AD-19), so an engine backend gives this a dialect and an executor of their
 * own, which reach a database apart from every tenant's data store (a SQLite
 * file of its own; a Postgres database of its own). As for the entity backend
 * (backend.h), this library writes every statement and the executor only runs
 * them, so the key store's tables and statements exist once for every engine.
 *
 * Per tenant (one database, or one schema named by the executor's
 * qualifier) there are six tables:
 *
 * | Table | Holds | Key |
 * | --- | --- | --- |
 * | `dsk_schema` | `id`, `version` (::GDEFSSQL_KEYSTORE_VERSION) | `id` |
 * | `dsk_counter` | `id`, `last_seq`: the ledger's sequence | `id` |
 * | `dsk_key` | `subject` REFERENCE, `secret` BYTES: a subject's key | `subject` |
 * | `dsk_ledger` | `subject`, `seq`, `at`: one row per erased subject | `subject` |
 * | `dsk_sealed` | `subject`, `secret`: material a caller sealed | `subject` |
 * | `dsk_secret` | `name`, `secret`: a tenant secret, written once | `name` |
 *
 * and the unique index `dsk_ledger_seq` on `dsk_ledger (seq)`. A key is made
 * with the cipher's randomness inside the key store and leaves it only into
 * the cipher, to seal or open bytes under it. Erasing a subject deletes its
 * `dsk_key` and `dsk_sealed` rows and inserts its `dsk_ledger` row in one
 * transaction; a subject with a ledger row reads as erased and can never get
 * a key again. What the engine keeps of a deleted row (free pages, a
 * write-ahead log) is the engine backend's to clear (SQLite: `secure_delete`
 * and a checkpoint; Postgres: the dialect's `key_vacuum` after the commit).
 *
 * The first write of a tenant in a backend's life creates the six tables in
 * a transaction of its own and checks `dsk_schema.version`: a newer one is
 * ::GDEFSTOR_ERR_UNSUPPORTED, another ::GDEFSTOR_ERR_FORMAT. A read of a
 * tenant whose store does not exist reads as no key.
 */

#ifndef GHOTI_IO_GDEFSSQL_KEYSTORE_H
#define GHOTI_IO_GDEFSSQL_KEYSTORE_H

#include <ghoti.io/defiant-storage-sql/macros.h>
#include <ghoti.io/defiant-storage-sql/allocator.h>
#include <ghoti.io/defiant-storage-sql/core.h>
#include <ghoti.io/defiant-storage-sql/dialect.h>
#include <ghoti.io/defiant-storage-sql/executor.h>
#include <ghoti.io/defiant-storage/keystore.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The layout version `dsk_schema` records. A key store holding another is
 * refused. */
#define GDEFSSQL_KEYSTORE_VERSION 1

/** The longest key a cipher may ask for, in bytes. */
#define GDEFSSQL_KEYSTORE_MAX_KEY_BYTES 1024

/**
 * @brief Make the key store backend for one engine.
 *
 * @param dialect Checked with gdefssql_dialect_check() and copied (its strings
 *   are borrowed for the backend's life).
 * @param executor Copied. It must reach the key store's database, never a
 *   tenant's data store. On success the backend owns its `ctx` and calls its
 *   `destroy` from GDEFSTOR_KeyStoreBackend::destroy; on failure nothing is
 *   taken.
 * @param cipher Copied; its `ctx` is borrowed for the backend's life. Every
 *   member is required, and `key_bytes` is 1 to
 *   ::GDEFSSQL_KEYSTORE_MAX_KEY_BYTES.
 * @param allocator NULL uses the default. It must be the allocator the key
 *   store is created with: the port frees what wrap and unwrap return with it.
 * @param out Receives the backend, to hand to gdefstor_keystore_create().
 * @return ::GDEFSSQL_OK, ::GDEFSSQL_ERR_INVALID (a dialect, an executor or a
 *   cipher that is incomplete), ::GDEFSSQL_ERR_UNSUPPORTED (a dialect without
 *   transactional DDL), or ::GDEFSSQL_ERR_OOM.
 */
GDEFSSQL_API GDEFSSQL_Result gdefssql_keystore_backend(
    const GDEFSSQL_Dialect * dialect, const GDEFSSQL_Executor * executor,
    const GDEFSTOR_KeyCipher * cipher, const GDEFSSQL_Allocator * allocator,
    GDEFSTOR_KeyStoreBackend * out);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GDEFSSQL_KEYSTORE_H */
