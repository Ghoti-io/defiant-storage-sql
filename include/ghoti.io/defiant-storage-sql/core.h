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
 * @file core.h
 * @brief Result codes for the shared SQL layer.
 * @stability free
 *
 * The vocabulary matches the port's, name for name. Numeric values are not
 * a contract: a caller maps with a switch, it does not cast.
 */

#ifndef GHOTI_IO_GDEFSSQL_CORE_H
#define GHOTI_IO_GDEFSSQL_CORE_H

#include <ghoti.io/defiant-storage-sql/macros.h>
#include <ghoti.io/defiant-storage/core.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Result of a call into the shared SQL layer.
 *
 * Same names as the port, including ::GDEFSSQL_ERR_BUSY, so a backend and
 * the port can be talked about with one vocabulary.
 */
typedef enum GDEFSSQL_Result {
  GDEFSSQL_OK = 0,          ///< Succeeded.
  GDEFSSQL_ERR_IO,          ///< A stream or sink reported an error.
  GDEFSSQL_ERR_FORMAT,      ///< Well formed input that is not this format.
  GDEFSSQL_ERR_UNSUPPORTED, ///< Unknown format name, or a newer version.
  GDEFSSQL_ERR_LIMIT,       ///< A limit was exceeded. Nothing is truncated.
  GDEFSSQL_ERR_CORRUPT,     ///< This format, but the bytes are wrong.
  GDEFSSQL_ERR_OOM,         ///< The allocator returned NULL.
  GDEFSSQL_ERR_INVALID,     ///< A caller argument is wrong, or a missing row.
  GDEFSSQL_ERR_INTERNAL,    ///< A library invariant failed.
  GDEFSSQL_ERR_BUSY,        ///< The tenant's writer is busy and this is not a fiber.
  GDEFSSQL_ERR_CONFLICT,    ///< A write named a base revision that is no longer the newest.
  GDEFSSQL_RESULT_COUNT     ///< Not a result. Closes the string table.
} GDEFSSQL_Result;

/**
 * @brief A static description of a result. Never NULL.
 *
 * @param result The code, including out-of-range values.
 * @return A string the caller does not free.
 */
GDEFSSQL_API const char * gdefssql_result_string(GDEFSSQL_Result result);

/**
 * @brief The port's result of the same name.
 *
 * An out-of-range value is ::GDEFSTOR_ERR_INTERNAL.
 */
GDEFSSQL_API GDEFSTOR_Result gdefssql_result_to_port(GDEFSSQL_Result result);

/**
 * @brief This library's result of the same name as a port result.
 *
 * An out-of-range value is ::GDEFSSQL_ERR_INTERNAL.
 */
GDEFSSQL_API GDEFSSQL_Result gdefssql_result_from_port(GDEFSTOR_Result result);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GDEFSSQL_CORE_H */
