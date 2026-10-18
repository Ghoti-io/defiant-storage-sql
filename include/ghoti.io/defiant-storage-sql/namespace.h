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
 * @file namespace.h
 * @brief Maps every public name into this build's version namespace.
 * @stability free
 */

#ifndef GHOTI_IO_GDEFSSQL_NAMESPACE_H
#define GHOTI_IO_GDEFSSQL_NAMESPACE_H

#include <ghoti.io/defiant-storage-sql/libver.h>

/// @cond HIDDEN_SYMBOLS

#define GDEFSSQL_Allocator GHOTIIO_DEFSSQL(GDEFSSQL_Allocator)
#define GDEFSSQL_Result GHOTIIO_DEFSSQL(GDEFSSQL_Result)
#define GDEFSSQL_ColType GHOTIIO_DEFSSQL(GDEFSSQL_ColType)
#define GDEFSSQL_Upsert GHOTIIO_DEFSSQL(GDEFSSQL_Upsert)
#define GDEFSSQL_Dialect GHOTIIO_DEFSSQL(GDEFSSQL_Dialect)
#define GDEFSSQL_DatumKind GHOTIIO_DEFSSQL(GDEFSSQL_DatumKind)
#define GDEFSSQL_Datum GHOTIIO_DEFSSQL(GDEFSSQL_Datum)
#define GDEFSSQL_Executor GHOTIIO_DEFSSQL(GDEFSSQL_Executor)
#define GDEFSSQL_Compiled GHOTIIO_DEFSSQL(GDEFSSQL_Compiled)

#define gdefssql_allocator_default GHOTIIO_DEFSSQL(gdefssql_allocator_default)
#define gdefssql_compiled_bind GHOTIIO_DEFSSQL(gdefssql_compiled_bind)
#define gdefssql_compiled_bind_count \
  GHOTIIO_DEFSSQL(gdefssql_compiled_bind_count)
#define gdefssql_compiled_count GHOTIIO_DEFSSQL(gdefssql_compiled_count)
#define gdefssql_compiled_free GHOTIIO_DEFSSQL(gdefssql_compiled_free)
#define gdefssql_compiled_text GHOTIIO_DEFSSQL(gdefssql_compiled_text)
#define gdefssql_dialect_check GHOTIIO_DEFSSQL(gdefssql_dialect_check)
#define gdefssql_model_backend GHOTIIO_DEFSSQL(gdefssql_model_backend)
#define gdefssql_query_compile GHOTIIO_DEFSSQL(gdefssql_query_compile)
#define gdefssql_result_from_port GHOTIIO_DEFSSQL(gdefssql_result_from_port)
#define gdefssql_result_string GHOTIIO_DEFSSQL(gdefssql_result_string)
#define gdefssql_result_to_port GHOTIIO_DEFSSQL(gdefssql_result_to_port)

/// @endcond

#endif /* GHOTI_IO_GDEFSSQL_NAMESPACE_H */
