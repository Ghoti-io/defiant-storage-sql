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
 * Cross-file declarations of the SQL layer. Not installed.
 *
 * Every byte of SQL text this library writes goes through the statement
 * builder here: keywords from string literals in this library's sources,
 * identifiers through gdefssql_sb_ident() (which refuses anything that is not
 * a generated name), and values only as bound parameters through
 * gdefssql_sb_param(). There is no other way into the text.
 */

#ifndef GHOTI_IO_GDEFSSQL_SRC_SQL_INTERNAL_H
#define GHOTI_IO_GDEFSSQL_SRC_SQL_INTERNAL_H

#include <ghoti.io/defiant-storage-sql/macros.h>
#include <ghoti.io/defiant-storage-sql/backend.h>
#include <ghoti.io/defiant-storage-sql/compile.h>
#include <ghoti.io/defiant-storage-sql/dialect.h>
#include <ghoti.io/defiant-storage-sql/executor.h>
#include <ghoti.io/defiant-storage/model.h>
#include <ghoti.io/defiant-storage/model_backend.h>
#include <stddef.h>
#include <stdint.h>

/* Renames for symbols that are not part of the installed API. */
#define gdefssql_sb_init GHOTIIO_DEFSSQL(gdefssql_sb_init)
#define gdefssql_sb_free GHOTIIO_DEFSSQL(gdefssql_sb_free)
#define gdefssql_sb_reset GHOTIIO_DEFSSQL(gdefssql_sb_reset)
#define gdefssql_sb_kw GHOTIIO_DEFSSQL(gdefssql_sb_kw)
#define gdefssql_sb_ident GHOTIIO_DEFSSQL(gdefssql_sb_ident)
#define gdefssql_sb_table GHOTIIO_DEFSSQL(gdefssql_sb_table)
#define gdefssql_sb_col GHOTIIO_DEFSSQL(gdefssql_sb_col)
#define gdefssql_sb_param GHOTIIO_DEFSSQL(gdefssql_sb_param)
#define gdefssql_sb_int GHOTIIO_DEFSSQL(gdefssql_sb_int)
#define gdefssql_sb_text GHOTIIO_DEFSSQL(gdefssql_sb_text)
#define gdefssql_sb_type GHOTIIO_DEFSSQL(gdefssql_sb_type)
#define gdefssql_name_ok GHOTIIO_DEFSSQL(gdefssql_name_ok)
#define gdefssql_name_table GHOTIIO_DEFSSQL(gdefssql_name_table)
#define gdefssql_name_column GHOTIIO_DEFSSQL(gdefssql_name_column)
#define gdefssql_name_index GHOTIIO_DEFSSQL(gdefssql_name_index)
#define gdefssql_name_alias GHOTIIO_DEFSSQL(gdefssql_name_alias)
#define gdefssql_value_coltype GHOTIIO_DEFSSQL(gdefssql_value_coltype)
#define gdefssql_value_datum GHOTIIO_DEFSSQL(gdefssql_value_datum)
#define gdefssql_value_compare GHOTIIO_DEFSSQL(gdefssql_value_compare)
#define gdefssql_compile_plan GHOTIIO_DEFSSQL(gdefssql_compile_plan)
#define gdefssql_mem0 GHOTIIO_DEFSSQL(gdefssql_mem0)

/* ---- generated names ------------------------------------------------------ */

/* Room for any generated name and its NUL. The longest is
 * "t4294967295_i4294967295_4294967295", 34 bytes. */
#define GDEFSSQL_NAME_CAP 40

/* What a generated table holds (dss_name.role). Stable: stored. */
enum {
  GDEFSSQL_ROLE_ENTITY = 1,  /* tN_e */
  GDEFSSQL_ROLE_STATE = 2,   /* tN_s */
  GDEFSSQL_ROLE_REVISION = 3,/* tN_r */
  GDEFSSQL_ROLE_NOW = 4,     /* tN_n, derived */
  GDEFSSQL_ROLE_GROUP = 5,   /* tN_gG */
  GDEFSSQL_ROLE_CURRENT = 6, /* tN_cG, derived */
  GDEFSSQL_ROLE_VALUE = 7,   /* column vC of tN_gG and tN_cG */
  GDEFSSQL_ROLE_SUBJECT = 8, /* column vCs of the same */
  GDEFSSQL_ROLE_INDEX = 9,   /* tN_iG_C on tN_cG (vC, ent) */
  GDEFSSQL_ROLE_SUBJECT_INDEX = 10, /* tN_jG_C on tN_gG (vCs) */
  GDEFSSQL_ROLE_ENTITY_INDEX = 11,  /* tNp, unique on tN_e (pid) */
  /* tno 0: the tenant tables' own, col 0 dss_type_tno (unique, dss_type
   * (tno)), col 1 dss_entity_rid (unique, dss_entity (rid)) */
  GDEFSSQL_ROLE_TENANT_INDEX = 12,
  /* The blind index (AD-14), per type with a personal field: */
  GDEFSSQL_ROLE_BLIND = 13,         /* tN_b: one row per value version's MAC */
  GDEFSSQL_ROLE_BLIND_INDEX = 14,   /* tN_bm on tN_b (fld, mac) */
  GDEFSSQL_ROLE_BLIND_SUBJECT = 15, /* tN_bs on tN_b (subject) */
  /* tno 0: the tenant tables of story S6a, col 0 dss_record, col 1 dss_seq,
   * col 2 dss_outbox, col 3 dss_outbox_id */
  GDEFSSQL_ROLE_SYSTEM = 16
};

/* dss_name.kind of a VALUE column: the field's logical type, plus this bit
 * for a personal field (whose value column holds ciphertext). */
#define GDEFSSQL_KIND_PERSONAL 0x100

/* dss_name.kind of an index row (roles INDEX to TENANT_INDEX): 0 when it is
 * built, this while it waits to be built after its step (AD-15). No value
 * column's kind reaches it. */
#define GDEFSSQL_KIND_PENDING 0x200

/* A generated name: `[a-z][a-z0-9_]*`, at most @p cap bytes. This is the only
 * shape that may reach SQL text as an identifier. */
int gdefssql_name_ok(const char * name, size_t cap);

/* The table of @p role (ENTITY .. CURRENT, BLIND) of type @p tno, group
 * @p group. */
void gdefssql_name_table(char out[GDEFSSQL_NAME_CAP], uint32_t tno, int role,
    uint32_t group);

/* Column @p col of a group table; with @p subject, its subject column. */
void gdefssql_name_column(char out[GDEFSSQL_NAME_CAP], uint32_t col,
    int subject);

/* An index of @p role (INDEX, SUBJECT_INDEX) on column @p col of a group;
 * for BLIND_INDEX and BLIND_SUBJECT, the blind table's (group and column 0). */
void gdefssql_name_index(char out[GDEFSSQL_NAME_CAP], uint32_t tno, int role,
    uint32_t group, uint32_t col);

/* A table alias of a statement. */
void gdefssql_name_alias(char out[GDEFSSQL_NAME_CAP], unsigned n);

/* ---- the statement builder ------------------------------------------------ */

typedef struct GDEFSSQL_Sb {
  const GDEFSTOR_Allocator * alloc;
  const GDEFSSQL_Dialect * d;
  const char * qual;        /* the tenant's qualifier, or NULL */
  char * text;              /* NUL-terminated */
  size_t len;
  size_t cap;
  GDEFSSQL_Datum * binds;   /* bytes borrowed from the caller */
  size_t nbinds;
  size_t capbinds;
  GDEFSTOR_Result rc;       /* the first error; every call after it is a no-op */
} GDEFSSQL_Sb;

void gdefssql_sb_init(GDEFSSQL_Sb * b, const GDEFSTOR_Allocator * alloc,
    const GDEFSSQL_Dialect * d, const char * qual);
void gdefssql_sb_free(GDEFSSQL_Sb * b);
/* Empty the text and the parameters, keep the buffers and the error. */
void gdefssql_sb_reset(GDEFSSQL_Sb * b);
/* Fixed vocabulary: a string literal of this library, written as is. */
void gdefssql_sb_kw(GDEFSSQL_Sb * b, const char * kw);
/* A generated identifier, quoted. Anything else stops the builder with
 * GDEFSTOR_ERR_INTERNAL. */
void gdefssql_sb_ident(GDEFSSQL_Sb * b, const char * name);
/* A table: the qualifier, if any, then the name. */
void gdefssql_sb_table(GDEFSSQL_Sb * b, const char * name);
/* "alias"."column" */
void gdefssql_sb_col(GDEFSSQL_Sb * b, const char * alias, const char * col);
/* A placeholder, and @p v as its parameter. */
void gdefssql_sb_param(GDEFSSQL_Sb * b, const GDEFSSQL_Datum * v);
void gdefssql_sb_int(GDEFSSQL_Sb * b, int64_t v);
/* A TEXT parameter (@p bytes borrowed). */
void gdefssql_sb_text(GDEFSSQL_Sb * b, const void * bytes, size_t len);
/* The dialect's type for a column of @p coltype, with the collation for text. */
void gdefssql_sb_type(GDEFSSQL_Sb * b, int coltype);

/* ---- values --------------------------------------------------------------- */

/* The column kind of a field's logical type (personal: BYTES). */
int gdefssql_value_coltype(int field_type, int personal);
/* The parameter for a value of a non-personal field (bytes borrowed). */
void gdefssql_value_datum(const GDEFSTOR_Value * v, GDEFSSQL_Datum * out);
/* The port's order of two values of one type (value.c's compare). */
int gdefssql_value_compare(const GDEFSTOR_Value * a, const GDEFSTOR_Value * b);

void * gdefssql_mem0(const GDEFSTOR_Allocator * alloc, size_t n);

/* ---- the compiler, for the backend --------------------------------------- */

/* Compile into two builders (the total, then the page unless count only). */
GDEFSTOR_Result gdefssql_compile_plan(const GDEFSSQL_Dialect * d,
    const char * qual, const GDEFSTOR_ModelPlan * plan,
    const uint32_t * tnos, size_t ntnos, GDEFSSQL_Sb * total,
    GDEFSSQL_Sb * page);

#endif /* GHOTI_IO_GDEFSSQL_SRC_SQL_INTERNAL_H */
