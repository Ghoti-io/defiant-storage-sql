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
 * @file compile.h
 * @brief The query compiler: the port's query form over entities, as SQL.
 * @stability free
 *
 * The backend made by gdefssql_model_backend() compiles and runs every query
 * itself. The compiler is public so that a backend can look at what it runs:
 * an engine's own plan (`EXPLAIN`) for rows-scanned assertions, or a golden
 * test of the text.
 *
 * ## What is compiled
 *
 * Exactly the form query.h describes "Over entities", as the port has checked
 * it (GDEFSTOR_ModelPlan), and meaning exactly what gdefstor_model_scan()
 * answers:
 *
 * - a FIELD operand is the set of that field's values on the entity's current
 *   items in the query's language: `e.row IN (SELECT c.entity FROM tN_cG c
 *   WHERE c.lang = ? AND c.vC op ...)`, so a field inside a multiple or a
 *   group holds when some item matches, and no other language's item is
 *   read; a comparison of two FIELD operands is an `EXISTS` over both item
 *   sets, each in the language, and a sort's join to the root group names
 *   the language in its `ON`;
 * - VALUES are bound parameters, a comparison against several is the `OR` of
 *   one per value, and a comparison of two VALUES lists is decided here;
 * - META and ENTITY are columns of `tN_e` and `tN_n`; a creator that is the
 *   system is no value;
 * - `and`, `or` and `not` are SQL's, and every comparison is written so that
 *   it is true or false, never unknown, so `not` is exact;
 * - EXISTS is `EXISTS (SELECT ... FROM tM_e ...)` over the other type, and an
 *   OUTER operand names the outer entity;
 * - sort keys, each with `NULLS FIRST` ascending and `NULLS LAST` descending
 *   where the engine's default differs (GDEFSSQL_Dialect::nulls_low), then
 *   the entity row ascending: the final tiebreak;
 * - the total is a `COUNT(*)` of the same predicate, with no page; the page
 *   is `LIMIT ? OFFSET ?`, every match being INT64_MAX.
 *
 * Anything else is refused with ::GDEFSSQL_ERR_UNSUPPORTED: a CONST operand
 * outside blind equality, a personal or group field, a node or flag this
 * compiler does not know. A statement that would need more parameters than the dialect allows
 * is ::GDEFSSQL_ERR_LIMIT. Nothing is ever answered differently.
 */

#ifndef GHOTI_IO_GDEFSSQL_COMPILE_H
#define GHOTI_IO_GDEFSSQL_COMPILE_H

#include <ghoti.io/defiant-storage-sql/macros.h>
#include <ghoti.io/defiant-storage-sql/allocator.h>
#include <ghoti.io/defiant-storage-sql/core.h>
#include <ghoti.io/defiant-storage-sql/dialect.h>
#include <ghoti.io/defiant-storage-sql/executor.h>
#include <ghoti.io/defiant-storage/model_backend.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Statement 0 of a compiled query: the total. */
#define GDEFSSQL_COMPILED_TOTAL 0
/** Statement 1 of a compiled query that is not count only: the page. */
#define GDEFSSQL_COMPILED_PAGE 1

/** A compiled query: one or two statements, each text and parameters. Opaque. */
typedef struct GDEFSSQL_Compiled GDEFSSQL_Compiled;

/**
 * @brief Compile a checked plan.
 *
 * @param dialect Checked with gdefssql_dialect_check().
 * @param qualifier What the executor returned for the tenant, or NULL.
 * @param plan A plan the port checked (model_backend.h). Its constants are
 *   borrowed: the compiled parameters point into it, so the plan must outlive
 *   the result.
 * @param type_numbers The number `N` of the queried type's tables, then of
 *   each type in GDEFSTOR_ModelPlan::exists_types, in order.
 * @param type_number_count `1 + plan->exists_count`.
 * @param allocator NULL uses the default.
 * @param out Receives the result; free it with gdefssql_compiled_free().
 * @return ::GDEFSSQL_OK, ::GDEFSSQL_ERR_INVALID, ::GDEFSSQL_ERR_UNSUPPORTED,
 *   ::GDEFSSQL_ERR_LIMIT, or ::GDEFSSQL_ERR_OOM.
 */
GDEFSSQL_API GDEFSSQL_Result gdefssql_query_compile(
    const GDEFSSQL_Dialect * dialect, const char * qualifier,
    const GDEFSTOR_ModelPlan * plan, const uint32_t * type_numbers,
    size_t type_number_count, const GDEFSSQL_Allocator * allocator,
    GDEFSSQL_Compiled ** out);

/** @return The statements: 1 for a count-only plan, else 2. */
GDEFSSQL_API size_t gdefssql_compiled_count(const GDEFSSQL_Compiled * c);

/**
 * @brief The text of statement @p index. Borrowed until the result is freed.
 *
 * The total returns one row, one column: the count. The page returns a row per
 * entity in order, with columns (row, public id).
 */
GDEFSSQL_API GDEFSSQL_Result gdefssql_compiled_text(
    const GDEFSSQL_Compiled * c, size_t index, const char ** out_text,
    size_t * out_len);

/** @return The parameters statement @p index takes, 0 for an index out of range. */
GDEFSSQL_API size_t gdefssql_compiled_bind_count(const GDEFSSQL_Compiled * c,
    size_t index);

/**
 * @brief Parameter @p k (0-based; it binds at position `k + 1`) of statement
 * @p index. Its bytes are borrowed from the plan.
 */
GDEFSSQL_API GDEFSSQL_Result gdefssql_compiled_bind(
    const GDEFSSQL_Compiled * c, size_t index, size_t k, GDEFSSQL_Datum * out);

/** @brief Release a compiled query. NULL is ignored. */
GDEFSSQL_API void gdefssql_compiled_free(GDEFSSQL_Compiled * c);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GDEFSSQL_COMPILE_H */
