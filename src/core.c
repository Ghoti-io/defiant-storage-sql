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
 * @file
 *
 * Result strings and the default allocator.
 */

#include <ghoti.io/defiant-storage-sql/allocator.h>
#include <ghoti.io/defiant-storage-sql/core.h>
#include <ghoti.io/cutil/allocator.h>

const GDEFSSQL_Allocator * gdefssql_allocator_default(void) {
  return gcu_allocator_default();
}

const char * gdefssql_result_string(GDEFSSQL_Result result) {
  switch (result) {
  case GDEFSSQL_OK:
    return "No error";
  case GDEFSSQL_ERR_IO:
    return "I/O error";
  case GDEFSSQL_ERR_FORMAT:
    return "Invalid format";
  case GDEFSSQL_ERR_UNSUPPORTED:
    return "Unsupported format";
  case GDEFSSQL_ERR_LIMIT:
    return "Limit exceeded";
  case GDEFSSQL_ERR_CORRUPT:
    return "Corrupt data";
  case GDEFSSQL_ERR_OOM:
    return "Out of memory";
  case GDEFSSQL_ERR_INVALID:
    return "Invalid argument";
  case GDEFSSQL_ERR_INTERNAL:
    return "Internal error";
  case GDEFSSQL_ERR_BUSY:
    return "Writer busy";
  case GDEFSSQL_ERR_CONFLICT:
    return "Revision conflict";
  case GDEFSSQL_RESULT_COUNT:
  default:
    return "Unknown error";
  }
}

GDEFSTOR_Result gdefssql_result_to_port(GDEFSSQL_Result result) {
  switch (result) {
  case GDEFSSQL_OK:
    return GDEFSTOR_OK;
  case GDEFSSQL_ERR_IO:
    return GDEFSTOR_ERR_IO;
  case GDEFSSQL_ERR_FORMAT:
    return GDEFSTOR_ERR_FORMAT;
  case GDEFSSQL_ERR_UNSUPPORTED:
    return GDEFSTOR_ERR_UNSUPPORTED;
  case GDEFSSQL_ERR_LIMIT:
    return GDEFSTOR_ERR_LIMIT;
  case GDEFSSQL_ERR_CORRUPT:
    return GDEFSTOR_ERR_CORRUPT;
  case GDEFSSQL_ERR_OOM:
    return GDEFSTOR_ERR_OOM;
  case GDEFSSQL_ERR_INVALID:
    return GDEFSTOR_ERR_INVALID;
  case GDEFSSQL_ERR_BUSY:
    return GDEFSTOR_ERR_BUSY;
  case GDEFSSQL_ERR_CONFLICT:
    return GDEFSTOR_ERR_CONFLICT;
  case GDEFSSQL_ERR_INTERNAL:
  case GDEFSSQL_RESULT_COUNT:
  default:
    return GDEFSTOR_ERR_INTERNAL;
  }
}

GDEFSSQL_Result gdefssql_result_from_port(GDEFSTOR_Result result) {
  switch (result) {
  case GDEFSTOR_OK:
    return GDEFSSQL_OK;
  case GDEFSTOR_ERR_IO:
    return GDEFSSQL_ERR_IO;
  case GDEFSTOR_ERR_FORMAT:
    return GDEFSSQL_ERR_FORMAT;
  case GDEFSTOR_ERR_UNSUPPORTED:
    return GDEFSSQL_ERR_UNSUPPORTED;
  case GDEFSTOR_ERR_LIMIT:
    return GDEFSSQL_ERR_LIMIT;
  case GDEFSTOR_ERR_CORRUPT:
    return GDEFSSQL_ERR_CORRUPT;
  case GDEFSTOR_ERR_OOM:
    return GDEFSSQL_ERR_OOM;
  case GDEFSTOR_ERR_INVALID:
    return GDEFSSQL_ERR_INVALID;
  case GDEFSTOR_ERR_BUSY:
    return GDEFSSQL_ERR_BUSY;
  case GDEFSTOR_ERR_CONFLICT:
    return GDEFSSQL_ERR_CONFLICT;
  case GDEFSTOR_ERR_INTERNAL:
  case GDEFSTOR_RESULT_COUNT:
  default:
    return GDEFSSQL_ERR_INTERNAL;
  }
}
