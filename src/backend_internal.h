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
 * What the entity backend's files share: a connection in use, a running
 * statement, a type's catalog and the backend itself. Not installed.
 */

#ifndef GHOTI_IO_GDEFSSQL_SRC_BACKEND_INTERNAL_H
#define GHOTI_IO_GDEFSSQL_SRC_BACKEND_INTERNAL_H

#include <ghoti.io/defiant-storage-sql/macros.h>
#include "sql_internal.h"

#define gdefssql_cx_exec GHOTIIO_DEFSSQL(gdefssql_cx_exec)
#define gdefssql_cx_run GHOTIIO_DEFSSQL(gdefssql_cx_run)
#define gdefssql_st_open GHOTIIO_DEFSSQL(gdefssql_st_open)
#define gdefssql_st_step GHOTIIO_DEFSSQL(gdefssql_st_step)
#define gdefssql_st_int GHOTIIO_DEFSSQL(gdefssql_st_int)
#define gdefssql_st_real GHOTIIO_DEFSSQL(gdefssql_st_real)
#define gdefssql_st_bytes GHOTIIO_DEFSSQL(gdefssql_st_bytes)
#define gdefssql_st_close GHOTIIO_DEFSSQL(gdefssql_st_close)
#define gdefssql_cx_counter GHOTIIO_DEFSSQL(gdefssql_cx_counter)
#define gdefssql_cx_counter_raise GHOTIIO_DEFSSQL(gdefssql_cx_counter_raise)
#define gdefssql_cat_load GHOTIIO_DEFSSQL(gdefssql_cat_load)
#define gdefssql_cat_free GHOTIIO_DEFSSQL(gdefssql_cat_free)
#define gdefssql_cat_group GHOTIIO_DEFSSQL(gdefssql_cat_group)
#define gdefssql_tenant_init GHOTIIO_DEFSSQL(gdefssql_tenant_init)
#define gdefssql_type_steps GHOTIIO_DEFSSQL(gdefssql_type_steps)
#define gdefssql_type_number GHOTIIO_DEFSSQL(gdefssql_type_number)
#define gdefssql_index_sweep GHOTIIO_DEFSSQL(gdefssql_index_sweep)
#define gdefssql_index_state GHOTIIO_DEFSSQL(gdefssql_index_state)

/* The backend: one per store. */
typedef struct GDEFSSQL_Back {
  const GDEFSTOR_Allocator * alloc;
  GDEFSSQL_Dialect d;
  GDEFSSQL_Executor x;
  struct GDEFSSQL_Ready * ready; /* tenants whose six tables are known made */
  struct GDEFSSQL_Txn * open;    /* open writes, one per tenant at most */
  /* The write's savepoint, set after BEGIN and rolled back to by an abort
   * that keeps counters (story S6a): built once, so that an abort needs no
   * allocation to undo the write. */
  char * sp_set;
  char * sp_undo;
} GDEFSSQL_Back;

/* A connection in use. */
typedef struct GDEFSSQL_Cx {
  GDEFSSQL_Back * b;
  void * conn;
  const char * qual;
  int failed; /* an executor call failed: roll back and discard */
  int pending; /* a step recorded an index to build after the commit */
} GDEFSSQL_Cx;

/* A statement being stepped. */
typedef struct GDEFSSQL_St {
  GDEFSSQL_Cx * c;
  void * stmt;
} GDEFSSQL_St;

/* Run a statement with no parameter and no row (DDL, transactions). */
GDEFSTOR_Result gdefssql_cx_exec(GDEFSSQL_Cx * c, const GDEFSSQL_Sb * sb);
/* Prepare, bind and step a statement to its end, ignoring rows (writes). */
GDEFSTOR_Result gdefssql_cx_run(GDEFSSQL_Cx * c, const GDEFSSQL_Sb * sb);
/* Prepare and bind. */
GDEFSTOR_Result gdefssql_st_open(GDEFSSQL_Cx * c, const GDEFSSQL_Sb * sb,
    GDEFSSQL_St * out);
GDEFSTOR_Result gdefssql_st_step(GDEFSSQL_St * s, int * out_row);
/* Column reads; *out_null is set for a NULL column (may be NULL to refuse
 * one with GDEFSTOR_ERR_CORRUPT). */
GDEFSTOR_Result gdefssql_st_int(GDEFSSQL_St * s, unsigned col, int64_t * out,
    int * out_null);
GDEFSTOR_Result gdefssql_st_real(GDEFSSQL_St * s, unsigned col, double * out,
    int * out_null);
GDEFSTOR_Result gdefssql_st_bytes(GDEFSSQL_St * s, unsigned col, int kind,
    const uint8_t ** out, size_t * out_len, int * out_null);
void gdefssql_st_close(GDEFSSQL_St * s);

/* dss_counter: advance @p which ("next_row", "next_type", "cseq") by one and
 * return the value it had (next_row, next_type) or has now (cseq). */
enum { GDEFSSQL_COUNTER_ROW, GDEFSSQL_COUNTER_TYPE, GDEFSSQL_COUNTER_CSEQ };
GDEFSTOR_Result gdefssql_cx_counter(GDEFSSQL_Cx * c, int which,
    uint64_t * out);

/* dss_counter: raise @p which to at least @p v (an import's commit
 * sequence), leaving a larger value as it is. */
GDEFSTOR_Result gdefssql_cx_counter_raise(GDEFSSQL_Cx * c, int which,
    uint64_t v);

/* ---- a type's catalog (dss_name) ----------------------------------------- */

typedef struct GDEFSSQL_CatCol {
  uint32_t col;
  int type;      /* the field's logical type */
  int personal;
} GDEFSSQL_CatCol;

typedef struct GDEFSSQL_CatGroup {
  uint32_t grp;
  GDEFSSQL_CatCol * cols; /* by column number, ascending */
  size_t ncols;
} GDEFSSQL_CatGroup;

/* One row of dss_name. */
typedef struct GDEFSSQL_CatRow {
  uint32_t grp;
  uint32_t col;
  int role;
  int kind;
} GDEFSSQL_CatRow;

typedef struct GDEFSSQL_Cat {
  const GDEFSTOR_Allocator * alloc;
  uint32_t tno;
  GDEFSSQL_CatRow * rows;
  size_t nrows;
  GDEFSSQL_CatGroup * groups; /* ascending */
  size_t ngroups;
} GDEFSSQL_Cat;

GDEFSTOR_Result gdefssql_cat_load(GDEFSSQL_Cx * c, uint32_t tno,
    GDEFSSQL_Cat * out);
void gdefssql_cat_free(GDEFSSQL_Cat * cat);
/* The group @p grp, or NULL when it has no table. */
const GDEFSSQL_CatGroup * gdefssql_cat_group(const GDEFSSQL_Cat * cat,
    uint32_t grp);

/* ---- schema (schema.c) ---------------------------------------------------- */

/* Create the six tenant tables and their rows, if missing (with the
 * dialect's qualifier_create, the tenant's schema first), record their two
 * indexes as pending steps, and check the layout version. Runs inside a
 * transaction the caller opened. */
GDEFSTOR_Result gdefssql_tenant_init(GDEFSSQL_Cx * c);

/* The number of type @p name: 0 in *out when it was never declared. */
GDEFSTOR_Result gdefssql_type_number(GDEFSSQL_Cx * c, const char * name,
    size_t name_len, uint32_t * out);

/* Every additive step @p decl needs beyond what dss_name records for type
 * @p tno, run in order and recorded. An index is only recorded, as pending,
 * and sets GDEFSSQL_Cx::pending: gdefssql_index_sweep() builds it after the
 * transaction commits. */
GDEFSTOR_Result gdefssql_type_steps(GDEFSSQL_Cx * c, uint32_t tno,
    const GDEFSTOR_Type * decl);

/* Build every index dss_name records as pending, on @p c, which has no
 * transaction open (AD-15: no index is built inside one), and mark each one
 * built that is. Stops at the first failure, which it returns; the rest stay
 * pending. *out_left receives how many are still pending (on success, 0). */
GDEFSTOR_Result gdefssql_index_sweep(GDEFSSQL_Cx * c, size_t * out_left);

/* The state of the field index of column @p col of group @p grp of type
 * @p tno, read inside the transaction open on @p c: GDEFSTOR_INDEX_READY once
 * built, GDEFSTOR_INDEX_INDEXING while pending or not recorded. */
GDEFSTOR_Result gdefssql_index_state(GDEFSSQL_Cx * c, uint32_t tno,
    uint32_t grp, uint32_t col, int * out_state);

#endif /* GHOTI_IO_GDEFSSQL_SRC_BACKEND_INTERNAL_H */
