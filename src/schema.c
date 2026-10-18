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
 * The physical schema: the six tenant tables, and the additive steps a
 * declaration needs (backend.h, "The physical schema").
 *
 * A step creates a table, adds a nullable column or creates an index, and
 * nothing else: no step drops, renames or retypes anything, so a retired
 * field's column stays and an old binary keeps working (AD-15). Every step is
 * recorded in dss_name in the same transaction as its DDL, and a step whose
 * record exists is not taken, so declaring the same tree twice runs no DDL.
 * Tables and indexes are also written IF NOT EXISTS.
 */

#include <ghoti.io/defiant-storage-sql/macros.h>
#include "backend_internal.h"

#include <ghoti.io/cutil/allocator.h>
#include <stdio.h>
#include <string.h>

typedef struct ColDef {
  const char * name;
  int coltype;
  int notnull;
} ColDef;

/* "CREATE TABLE IF NOT EXISTS t (" and the fixed columns. */
static void create_open(GDEFSSQL_Sb * sb, const char * table,
    const ColDef * cols, size_t n) {
  size_t i;
  gdefssql_sb_kw(sb, "CREATE TABLE IF NOT EXISTS ");
  gdefssql_sb_table(sb, table);
  gdefssql_sb_kw(sb, " (");
  for (i = 0; i < n; i++) {
    if (i > 0) {
      gdefssql_sb_kw(sb, ", ");
    }
    gdefssql_sb_ident(sb, cols[i].name);
    gdefssql_sb_kw(sb, " ");
    gdefssql_sb_type(sb, cols[i].coltype);
    if (cols[i].notnull) {
      gdefssql_sb_kw(sb, " NOT NULL");
    }
  }
}

/* ", PRIMARY KEY (k, ...))" */
static void create_close(GDEFSSQL_Sb * sb, const char * const * key,
    size_t n) {
  size_t i;
  gdefssql_sb_kw(sb, ", PRIMARY KEY (");
  for (i = 0; i < n; i++) {
    if (i > 0) {
      gdefssql_sb_kw(sb, ", ");
    }
    gdefssql_sb_ident(sb, key[i]);
  }
  gdefssql_sb_kw(sb, "))");
}

static GDEFSTOR_Result create_table(GDEFSSQL_Cx * c, const char * table,
    const ColDef * cols, size_t n, const char * const * key, size_t nkey) {
  GDEFSSQL_Sb sb;
  GDEFSTOR_Result rc;
  gdefssql_sb_init(&sb, c->b->alloc, &c->b->d, c->qual);
  create_open(&sb, table, cols, n);
  create_close(&sb, key, nkey);
  rc = gdefssql_cx_exec(c, &sb);
  gdefssql_sb_free(&sb);
  return rc;
}

static GDEFSTOR_Result create_index(GDEFSSQL_Cx * c, int unique,
    const char * index, const char * table, const char * const * cols,
    size_t n) {
  GDEFSSQL_Sb sb;
  GDEFSTOR_Result rc;
  size_t i;
  gdefssql_sb_init(&sb, c->b->alloc, &c->b->d, c->qual);
  gdefssql_sb_kw(&sb, unique ? "CREATE UNIQUE INDEX IF NOT EXISTS "
                             : "CREATE INDEX IF NOT EXISTS ");
  /* An index lives in its table's schema: Postgres takes no qualifier on
   * the index name, so it is written bare. */
  gdefssql_sb_ident(&sb, index);
  gdefssql_sb_kw(&sb, " ON ");
  gdefssql_sb_table(&sb, table);
  gdefssql_sb_kw(&sb, " (");
  for (i = 0; i < n; i++) {
    if (i > 0) {
      gdefssql_sb_kw(&sb, ", ");
    }
    gdefssql_sb_ident(&sb, cols[i]);
  }
  gdefssql_sb_kw(&sb, ")");
  rc = gdefssql_cx_exec(c, &sb);
  gdefssql_sb_free(&sb);
  return rc;
}

/* SELECT "a0"."<col>" FROM <table> "a0" WHERE "a0"."id" = 1: the one row of
 * a tenant table, or *out_found 0. */
static GDEFSTOR_Result read_one(GDEFSSQL_Cx * c, const char * table,
    const char * col, int64_t * out, int * out_found) {
  GDEFSSQL_Sb sb;
  GDEFSSQL_St s;
  GDEFSTOR_Result rc;
  int row = 0;
  *out_found = 0;
  gdefssql_sb_init(&sb, c->b->alloc, &c->b->d, c->qual);
  gdefssql_sb_kw(&sb, "SELECT ");
  gdefssql_sb_col(&sb, "a0", col);
  gdefssql_sb_kw(&sb, " FROM ");
  gdefssql_sb_table(&sb, table);
  gdefssql_sb_kw(&sb, " ");
  gdefssql_sb_ident(&sb, "a0");
  gdefssql_sb_kw(&sb, " WHERE ");
  gdefssql_sb_col(&sb, "a0", "id");
  gdefssql_sb_kw(&sb, " = ");
  gdefssql_sb_int(&sb, 1);
  rc = gdefssql_st_open(c, &sb, &s);
  if (rc == GDEFSTOR_OK) {
    rc = gdefssql_st_step(&s, &row);
  }
  if (rc == GDEFSTOR_OK && row) {
    rc = gdefssql_st_int(&s, 0, out, NULL);
    *out_found = rc == GDEFSTOR_OK;
  }
  gdefssql_st_close(&s);
  gdefssql_sb_free(&sb);
  return rc;
}

GDEFSTOR_Result gdefssql_tenant_init(GDEFSSQL_Cx * c) {
  static const ColDef schema_cols[] = {{"id", GDEFSSQL_COL_ID, 1},
      {"version", GDEFSSQL_COL_ID, 1}};
  static const ColDef counter_cols[] = {{"id", GDEFSSQL_COL_ID, 1},
      {"last_row", GDEFSSQL_COL_ID, 1}, {"last_type", GDEFSSQL_COL_ID, 1},
      {"last_cseq", GDEFSSQL_COL_ID, 1}};
  static const ColDef type_cols[] = {{"name", GDEFSSQL_COL_TEXT, 1},
      {"tno", GDEFSSQL_COL_ID, 1}, {"decl", GDEFSSQL_COL_BYTES, 1}};
  static const ColDef name_cols[] = {{"tno", GDEFSSQL_COL_ID, 1},
      {"grp", GDEFSSQL_COL_ID, 1}, {"col", GDEFSSQL_COL_ID, 1},
      {"role", GDEFSSQL_COL_ID, 1}, {"name", GDEFSSQL_COL_TEXT, 1},
      {"kind", GDEFSSQL_COL_ID, 1}};
  static const ColDef entity_cols[] = {{"pid", GDEFSSQL_COL_REFERENCE, 1},
      {"rid", GDEFSSQL_COL_ID, 1}, {"tno", GDEFSSQL_COL_ID, 1}};
  static const ColDef draft_cols[] = {{"pid", GDEFSSQL_COL_REFERENCE, 1},
      {"type", GDEFSSQL_COL_TEXT, 1}, {"base", GDEFSSQL_COL_ID, 1},
      {"body", GDEFSSQL_COL_BYTES, 1}};
  static const char * const id_key[] = {"id"};
  static const char * const name_key[] = {"name"};
  static const char * const tno_key[] = {"tno"};
  static const char * const names_key[] = {"tno", "grp", "col", "role"};
  static const char * const pid_key[] = {"pid"};
  static const char * const rid_key[] = {"rid"};
  GDEFSTOR_Result rc;
  int64_t v = 0;
  int found = 0;
  rc = create_table(c, "dss_schema", schema_cols, 2, id_key, 1);
  if (rc == GDEFSTOR_OK) {
    rc = create_table(c, "dss_counter", counter_cols, 4, id_key, 1);
  }
  if (rc == GDEFSTOR_OK) {
    rc = create_table(c, "dss_type", type_cols, 3, name_key, 1);
  }
  if (rc == GDEFSTOR_OK) {
    rc = create_index(c, 1, "dss_type_tno", "dss_type", tno_key, 1);
  }
  if (rc == GDEFSTOR_OK) {
    rc = create_table(c, "dss_name", name_cols, 6, names_key, 4);
  }
  if (rc == GDEFSTOR_OK) {
    rc = create_table(c, "dss_entity", entity_cols, 3, pid_key, 1);
  }
  if (rc == GDEFSTOR_OK) {
    rc = create_index(c, 1, "dss_entity_rid", "dss_entity", rid_key, 1);
  }
  if (rc == GDEFSTOR_OK) {
    rc = create_table(c, "dss_draft", draft_cols, 4, pid_key, 1);
  }
  if (rc == GDEFSTOR_OK) {
    rc = read_one(c, "dss_schema", "version", &v, &found);
  }
  if (rc == GDEFSTOR_OK && found && v != GDEFSSQL_SCHEMA_VERSION) {
    /* Another layout: refused, never read as this one. */
    return v > GDEFSSQL_SCHEMA_VERSION ? GDEFSTOR_ERR_UNSUPPORTED
                                       : GDEFSTOR_ERR_FORMAT;
  }
  if (rc == GDEFSTOR_OK && !found) {
    GDEFSSQL_Sb sb;
    gdefssql_sb_init(&sb, c->b->alloc, &c->b->d, c->qual);
    gdefssql_sb_kw(&sb, "INSERT INTO ");
    gdefssql_sb_table(&sb, "dss_schema");
    gdefssql_sb_kw(&sb, " (");
    gdefssql_sb_ident(&sb, "id");
    gdefssql_sb_kw(&sb, ", ");
    gdefssql_sb_ident(&sb, "version");
    gdefssql_sb_kw(&sb, ") VALUES (");
    gdefssql_sb_int(&sb, 1);
    gdefssql_sb_kw(&sb, ", ");
    gdefssql_sb_int(&sb, GDEFSSQL_SCHEMA_VERSION);
    gdefssql_sb_kw(&sb, ")");
    rc = gdefssql_cx_run(c, &sb);
    gdefssql_sb_free(&sb);
  }
  if (rc == GDEFSTOR_OK) {
    rc = read_one(c, "dss_counter", "last_row", &v, &found);
  }
  if (rc == GDEFSTOR_OK && !found) {
    GDEFSSQL_Sb sb;
    unsigned i;
    gdefssql_sb_init(&sb, c->b->alloc, &c->b->d, c->qual);
    gdefssql_sb_kw(&sb, "INSERT INTO ");
    gdefssql_sb_table(&sb, "dss_counter");
    gdefssql_sb_kw(&sb, " (");
    for (i = 0; i < 4; i++) {
      gdefssql_sb_kw(&sb, i == 0 ? "" : ", ");
      gdefssql_sb_ident(&sb, counter_cols[i].name);
    }
    gdefssql_sb_kw(&sb, ") VALUES (");
    gdefssql_sb_int(&sb, 1);
    for (i = 1; i < 4; i++) {
      gdefssql_sb_kw(&sb, ", ");
      gdefssql_sb_int(&sb, 0);
    }
    gdefssql_sb_kw(&sb, ")");
    rc = gdefssql_cx_run(c, &sb);
    gdefssql_sb_free(&sb);
  }
  return rc;
}

GDEFSTOR_Result gdefssql_type_number(GDEFSSQL_Cx * c, const char * name,
    size_t name_len, uint32_t * out) {
  GDEFSSQL_Sb sb;
  GDEFSSQL_St s;
  GDEFSTOR_Result rc;
  int row = 0;
  int64_t v = 0;
  *out = 0;
  gdefssql_sb_init(&sb, c->b->alloc, &c->b->d, c->qual);
  gdefssql_sb_kw(&sb, "SELECT ");
  gdefssql_sb_col(&sb, "a0", "tno");
  gdefssql_sb_kw(&sb, " FROM ");
  gdefssql_sb_table(&sb, "dss_type");
  gdefssql_sb_kw(&sb, " ");
  gdefssql_sb_ident(&sb, "a0");
  gdefssql_sb_kw(&sb, " WHERE ");
  gdefssql_sb_col(&sb, "a0", "name");
  gdefssql_sb_kw(&sb, " = ");
  gdefssql_sb_text(&sb, name, name_len);
  rc = gdefssql_st_open(c, &sb, &s);
  if (rc == GDEFSTOR_OK) {
    rc = gdefssql_st_step(&s, &row);
  }
  if (rc == GDEFSTOR_OK && row) {
    rc = gdefssql_st_int(&s, 0, &v, NULL);
    if (rc == GDEFSTOR_OK && (v <= 0 || v > (int64_t)UINT32_MAX)) {
      rc = GDEFSTOR_ERR_CORRUPT;
    }
    if (rc == GDEFSTOR_OK) {
      *out = (uint32_t)v;
    }
  }
  gdefssql_st_close(&s);
  gdefssql_sb_free(&sb);
  return rc;
}

/* ---- the steps of one declaration ---------------------------------------- */

/* One column a declaration wants. */
typedef struct Want {
  uint32_t grp;
  uint32_t col;
  int type;
  int personal;
  int indexed;
} Want;

typedef struct Plan {
  const GDEFSTOR_Allocator * alloc;
  uint32_t * groups; /* ascending, 0 first */
  size_t ngroups;
  Want * cols;       /* ascending by (grp, col) */
  size_t ncols;
} Plan;

static void plan_free(Plan * p) {
  gcu_allocator_free(p->alloc, p->groups);
  gcu_allocator_free(p->alloc, p->cols);
}

static GDEFSTOR_Result plan_make(const GDEFSTOR_Allocator * alloc,
    const GDEFSTOR_Type * decl, Plan * out) {
  size_t n = gdefstor_type_field_count(decl);
  size_t i;
  memset(out, 0, sizeof *out);
  out->alloc = alloc;
  out->groups = (uint32_t *)gdefssql_mem0(alloc, (n + 1) * sizeof *out->groups);
  out->cols = (Want *)gdefssql_mem0(alloc, (n + 1) * sizeof *out->cols);
  if (out->groups == NULL || out->cols == NULL) {
    plan_free(out);
    return GDEFSTOR_ERR_OOM;
  }
  out->groups[0] = 0;
  out->ngroups = 1;
  for (i = 0; i < n; i++) {
    GDEFSTOR_FieldInfo f;
    memset(&f, 0, sizeof f);
    f.size = sizeof f;
    if (gdefstor_type_field(decl, i, &f) != GDEFSTOR_OK) {
      plan_free(out);
      return GDEFSTOR_ERR_INTERNAL;
    }
    if (f.type == GDEFSTOR_FIELD_GROUP ||
        (f.flags & GDEFSTOR_FIELD_MULTIPLE) != 0) {
      if (f.group == UINT32_MAX || f.group == 0) {
        plan_free(out);
        return GDEFSTOR_ERR_INTERNAL;
      }
      out->groups[out->ngroups++] = f.group;
    }
    if (f.type != GDEFSTOR_FIELD_GROUP) {
      Want * w = &out->cols[out->ncols++];
      int multiple = (f.flags & GDEFSTOR_FIELD_MULTIPLE) != 0;
      w->grp = multiple ? f.group : f.in_group;
      w->col = multiple ? 0 : f.column;
      w->type = f.type;
      w->personal = (f.flags & GDEFSTOR_FIELD_PERSONAL) != 0;
      w->indexed = (f.flags & GDEFSTOR_FIELD_INDEXED) != 0 && !w->personal;
      if (gdefssql_value_coltype(w->type, w->personal) < 0) {
        plan_free(out);
        return GDEFSTOR_ERR_INTERNAL;
      }
    }
  }
  /* Sort both (insertion: a declaration is at most 512 fields). */
  for (i = 1; i < out->ngroups; i++) {
    uint32_t g = out->groups[i];
    size_t k = i;
    while (k > 0 && out->groups[k - 1] > g) {
      out->groups[k] = out->groups[k - 1];
      k -= 1;
    }
    out->groups[k] = g;
  }
  for (i = 1; i < out->ncols; i++) {
    Want w = out->cols[i];
    size_t k = i;
    while (k > 0 && (out->cols[k - 1].grp > w.grp ||
                        (out->cols[k - 1].grp == w.grp &&
                            out->cols[k - 1].col > w.col))) {
      out->cols[k] = out->cols[k - 1];
      k -= 1;
    }
    out->cols[k] = w;
  }
  for (i = 1; i < out->ngroups; i++) {
    if (out->groups[i] == out->groups[i - 1]) {
      plan_free(out);
      return GDEFSTOR_ERR_INTERNAL;
    }
  }
  for (i = 1; i < out->ncols; i++) {
    if (out->cols[i].grp == out->cols[i - 1].grp &&
        out->cols[i].col == out->cols[i - 1].col) {
      plan_free(out);
      return GDEFSTOR_ERR_INTERNAL;
    }
  }
  return GDEFSTOR_OK;
}

static const GDEFSSQL_CatRow * have(const GDEFSSQL_Cat * cat, uint32_t grp,
    uint32_t col, int role) {
  size_t i;
  for (i = 0; i < cat->nrows; i++) {
    const GDEFSSQL_CatRow * r = &cat->rows[i];
    if (r->grp == grp && r->col == col && r->role == role) {
      return r;
    }
  }
  return NULL;
}

/* Record one step in dss_name. */
static GDEFSTOR_Result record(GDEFSSQL_Cx * c, uint32_t tno, uint32_t grp,
    uint32_t col, int role, const char * name, int kind) {
  GDEFSSQL_Sb sb;
  GDEFSTOR_Result rc;
  gdefssql_sb_init(&sb, c->b->alloc, &c->b->d, c->qual);
  gdefssql_sb_kw(&sb, "INSERT INTO ");
  gdefssql_sb_table(&sb, "dss_name");
  gdefssql_sb_kw(&sb, " (");
  gdefssql_sb_ident(&sb, "tno");
  gdefssql_sb_kw(&sb, ", ");
  gdefssql_sb_ident(&sb, "grp");
  gdefssql_sb_kw(&sb, ", ");
  gdefssql_sb_ident(&sb, "col");
  gdefssql_sb_kw(&sb, ", ");
  gdefssql_sb_ident(&sb, "role");
  gdefssql_sb_kw(&sb, ", ");
  gdefssql_sb_ident(&sb, "name");
  gdefssql_sb_kw(&sb, ", ");
  gdefssql_sb_ident(&sb, "kind");
  gdefssql_sb_kw(&sb, ") VALUES (");
  gdefssql_sb_int(&sb, tno);
  gdefssql_sb_kw(&sb, ", ");
  gdefssql_sb_int(&sb, grp);
  gdefssql_sb_kw(&sb, ", ");
  gdefssql_sb_int(&sb, col);
  gdefssql_sb_kw(&sb, ", ");
  gdefssql_sb_int(&sb, role);
  gdefssql_sb_kw(&sb, ", ");
  gdefssql_sb_text(&sb, name, strlen(name));
  gdefssql_sb_kw(&sb, ", ");
  gdefssql_sb_int(&sb, kind);
  gdefssql_sb_kw(&sb, ")");
  rc = gdefssql_cx_run(c, &sb);
  gdefssql_sb_free(&sb);
  return rc;
}

static GDEFSTOR_Result type_tables(GDEFSSQL_Cx * c, const GDEFSSQL_Cat * cat,
    uint32_t tno) {
  static const ColDef e_cols[] = {{"rid", GDEFSSQL_COL_ID, 1},
      {"pid", GDEFSSQL_COL_REFERENCE, 1}, {"creator", GDEFSSQL_COL_REFERENCE, 0},
      {"created", GDEFSSQL_COL_DATETIME, 1}, {"rev", GDEFSSQL_COL_ID, 1},
      {"last_item", GDEFSSQL_COL_ID, 1}};
  static const ColDef s_cols[] = {{"rid", GDEFSSQL_COL_ID, 1},
      {"seq", GDEFSSQL_COL_ID, 1}, {"pub", GDEFSSQL_COL_BOOLEAN, 1},
      {"del", GDEFSSQL_COL_BOOLEAN, 1}, {"actor", GDEFSSQL_COL_REFERENCE, 1},
      {"at", GDEFSSQL_COL_DATETIME, 1}};
  static const ColDef r_cols[] = {{"rid", GDEFSSQL_COL_ID, 1},
      {"rev", GDEFSSQL_COL_ID, 1}, {"cseq", GDEFSSQL_COL_ID, 1},
      {"who", GDEFSSQL_COL_REFERENCE, 1}, {"at", GDEFSSQL_COL_DATETIME, 1},
      {"why", GDEFSSQL_COL_TEXT, 1}};
  static const ColDef n_cols[] = {{"rid", GDEFSSQL_COL_ID, 1},
      {"pub", GDEFSSQL_COL_BOOLEAN, 1}, {"del", GDEFSSQL_COL_BOOLEAN, 1}};
  static const char * const rid_key[] = {"rid"};
  static const char * const seq_key[] = {"rid", "seq"};
  static const char * const rev_key[] = {"rid", "rev"};
  static const char * const pid_col[] = {"pid"};
  struct {
    int role;
    const ColDef * cols;
    size_t n;
    const char * const * key;
    size_t nkey;
  } t[4] = {{GDEFSSQL_ROLE_ENTITY, e_cols, 6, rid_key, 1},
      {GDEFSSQL_ROLE_STATE, s_cols, 6, seq_key, 2},
      {GDEFSSQL_ROLE_REVISION, r_cols, 6, rev_key, 2},
      {GDEFSSQL_ROLE_NOW, n_cols, 3, rid_key, 1}};
  size_t i;
  for (i = 0; i < 4; i++) {
    char name[GDEFSSQL_NAME_CAP];
    GDEFSTOR_Result rc;
    if (have(cat, 0, 0, t[i].role) != NULL) {
      continue;
    }
    gdefssql_name_table(name, tno, t[i].role, 0);
    rc = create_table(c, name, t[i].cols, t[i].n, t[i].key, t[i].nkey);
    if (rc == GDEFSTOR_OK && t[i].role == GDEFSSQL_ROLE_ENTITY) {
      /* The entity lookup by public id, which ENTITY operands use. */
      char index[GDEFSSQL_NAME_CAP];
      snprintf(index, sizeof index, "t%lup", (unsigned long)tno);
      rc = create_index(c, 1, index, name, pid_col, 1);
    }
    if (rc == GDEFSTOR_OK) {
      rc = record(c, tno, 0, 0, t[i].role, name, 0);
    }
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  return GDEFSTOR_OK;
}

/* The value column, then (personal) its subject column, of a definition. */
static void col_defs(GDEFSSQL_Sb * sb, const Want * w) {
  char name[GDEFSSQL_NAME_CAP];
  gdefssql_name_column(name, w->col, 0);
  gdefssql_sb_ident(sb, name);
  gdefssql_sb_kw(sb, " ");
  gdefssql_sb_type(sb, gdefssql_value_coltype(w->type, w->personal));
  if (w->personal) {
    gdefssql_sb_kw(sb, ", ");
    gdefssql_name_column(name, w->col, 1);
    gdefssql_sb_ident(sb, name);
    gdefssql_sb_kw(sb, " ");
    gdefssql_sb_type(sb, GDEFSSQL_COL_REFERENCE);
  }
}

static int kind_of(const Want * w) {
  return w->type | (w->personal ? GDEFSSQL_KIND_PERSONAL : 0);
}

/* Create the item-version table and the current table of group @p g with
 * every column the declaration gives it, and record them. */
static GDEFSTOR_Result group_tables(GDEFSSQL_Cx * c, const GDEFSSQL_Cat * cat,
    const Plan * p, uint32_t tno, uint32_t g) {
  static const ColDef g_cols[] = {{"ent", GDEFSSQL_COL_ID, 1},
      {"item", GDEFSSQL_COL_ID, 1}, {"vfrom", GDEFSSQL_COL_ID, 1},
      {"vto", GDEFSSQL_COL_ID, 0}, {"parent", GDEFSSQL_COL_ID, 1},
      {"ord", GDEFSSQL_COL_FLOAT, 1}};
  static const ColDef c_cols[] = {{"ent", GDEFSSQL_COL_ID, 1},
      {"item", GDEFSSQL_COL_ID, 1}, {"vfrom", GDEFSSQL_COL_ID, 1},
      {"parent", GDEFSSQL_COL_ID, 1}, {"ord", GDEFSSQL_COL_FLOAT, 1}};
  static const char * const g_key[] = {"ent", "item", "vfrom"};
  static const char * const c_key[] = {"ent", "item"};
  int pass;
  GDEFSTOR_Result rc = GDEFSTOR_OK;
  for (pass = 0; pass < 2 && rc == GDEFSTOR_OK; pass++) {
    int role = pass == 0 ? GDEFSSQL_ROLE_GROUP : GDEFSSQL_ROLE_CURRENT;
    char name[GDEFSSQL_NAME_CAP];
    GDEFSSQL_Sb sb;
    size_t i;
    if (have(cat, g, 0, role) != NULL) {
      continue;
    }
    gdefssql_name_table(name, tno, role, g);
    gdefssql_sb_init(&sb, c->b->alloc, &c->b->d, c->qual);
    if (pass == 0) {
      create_open(&sb, name, g_cols, 6);
    } else {
      create_open(&sb, name, c_cols, 5);
    }
    for (i = 0; i < p->ncols; i++) {
      if (p->cols[i].grp == g) {
        gdefssql_sb_kw(&sb, ", ");
        col_defs(&sb, &p->cols[i]);
      }
    }
    if (pass == 0) {
      create_close(&sb, g_key, 3);
    } else {
      create_close(&sb, c_key, 2);
    }
    rc = gdefssql_cx_exec(c, &sb);
    gdefssql_sb_free(&sb);
    if (rc == GDEFSTOR_OK) {
      rc = record(c, tno, g, 0, role, name, 0);
    }
  }
  /* A table made here holds its columns: record them, so they are not
   * added again. */
  if (rc == GDEFSTOR_OK && have(cat, g, 0, GDEFSSQL_ROLE_GROUP) == NULL) {
    size_t i;
    for (i = 0; rc == GDEFSTOR_OK && i < p->ncols; i++) {
      const Want * w = &p->cols[i];
      char name[GDEFSSQL_NAME_CAP];
      if (w->grp != g) {
        continue;
      }
      gdefssql_name_column(name, w->col, 0);
      rc = record(c, tno, g, w->col, GDEFSSQL_ROLE_VALUE, name, kind_of(w));
      if (rc == GDEFSTOR_OK && w->personal) {
        gdefssql_name_column(name, w->col, 1);
        rc = record(c, tno, g, w->col, GDEFSSQL_ROLE_SUBJECT, name,
            GDEFSTOR_FIELD_REFERENCE);
      }
    }
  }
  return rc;
}

static GDEFSTOR_Result add_column(GDEFSSQL_Cx * c, uint32_t tno,
    const Want * w) {
  int pass;
  GDEFSTOR_Result rc = GDEFSTOR_OK;
  char name[GDEFSSQL_NAME_CAP];
  for (pass = 0; pass < 2 && rc == GDEFSTOR_OK; pass++) {
    int subject;
    for (subject = 0; subject <= w->personal && rc == GDEFSTOR_OK;
         subject++) {
      GDEFSSQL_Sb sb;
      gdefssql_name_table(name, tno,
          pass == 0 ? GDEFSSQL_ROLE_GROUP : GDEFSSQL_ROLE_CURRENT, w->grp);
      gdefssql_sb_init(&sb, c->b->alloc, &c->b->d, c->qual);
      gdefssql_sb_kw(&sb, "ALTER TABLE ");
      gdefssql_sb_table(&sb, name);
      gdefssql_sb_kw(&sb, " ADD COLUMN ");
      gdefssql_name_column(name, w->col, subject);
      gdefssql_sb_ident(&sb, name);
      gdefssql_sb_kw(&sb, " ");
      gdefssql_sb_type(&sb, subject ? GDEFSSQL_COL_REFERENCE
                                    : gdefssql_value_coltype(w->type,
                                          w->personal));
      rc = gdefssql_cx_exec(c, &sb);
      gdefssql_sb_free(&sb);
    }
  }
  if (rc == GDEFSTOR_OK) {
    gdefssql_name_column(name, w->col, 0);
    rc = record(c, tno, w->grp, w->col, GDEFSSQL_ROLE_VALUE, name, kind_of(w));
  }
  if (rc == GDEFSTOR_OK && w->personal) {
    gdefssql_name_column(name, w->col, 1);
    rc = record(c, tno, w->grp, w->col, GDEFSSQL_ROLE_SUBJECT, name,
        GDEFSTOR_FIELD_REFERENCE);
  }
  return rc;
}

static GDEFSTOR_Result add_index(GDEFSSQL_Cx * c, uint32_t tno,
    const Want * w, int role) {
  char index[GDEFSSQL_NAME_CAP];
  char table[GDEFSSQL_NAME_CAP];
  char col[GDEFSSQL_NAME_CAP];
  const char * cols[2];
  GDEFSTOR_Result rc;
  gdefssql_name_index(index, tno, role, w->grp, w->col);
  if (role == GDEFSSQL_ROLE_INDEX) {
    /* What a filter on the field reads: matching current items, then their
     * entity. */
    gdefssql_name_table(table, tno, GDEFSSQL_ROLE_CURRENT, w->grp);
    gdefssql_name_column(col, w->col, 0);
    cols[0] = col;
    cols[1] = "ent";
    rc = create_index(c, 0, index, table, cols, 2);
  } else {
    /* "Everything about this person": every version naming the subject. */
    gdefssql_name_table(table, tno, GDEFSSQL_ROLE_GROUP, w->grp);
    gdefssql_name_column(col, w->col, 1);
    cols[0] = col;
    rc = create_index(c, 0, index, table, cols, 1);
  }
  if (rc == GDEFSTOR_OK) {
    rc = record(c, tno, w->grp, w->col, role, index, 0);
  }
  return rc;
}

GDEFSTOR_Result gdefssql_type_steps(GDEFSSQL_Cx * c, uint32_t tno,
    const GDEFSTOR_Type * decl) {
  GDEFSSQL_Cat cat;
  Plan p;
  size_t i;
  GDEFSTOR_Result rc = plan_make(c->b->alloc, decl, &p);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = gdefssql_cat_load(c, tno, &cat);
  if (rc != GDEFSTOR_OK) {
    plan_free(&p);
    return rc;
  }
  /* Checks first, so that a refusal runs no DDL at all: a column kept with
   * another kind (a retype is never a step), and the engine's column limit. */
  for (i = 0; rc == GDEFSTOR_OK && i < p.ncols; i++) {
    const GDEFSSQL_CatRow * r =
        have(&cat, p.cols[i].grp, p.cols[i].col, GDEFSSQL_ROLE_VALUE);
    if (r != NULL && r->kind != kind_of(&p.cols[i])) {
      rc = GDEFSTOR_ERR_CORRUPT;
    }
  }
  for (i = 0; rc == GDEFSTOR_OK && i < p.ngroups; i++) {
    size_t n = 6;
    size_t k;
    for (k = 0; k < p.ncols; k++) {
      if (p.cols[k].grp == p.groups[i]) {
        n += p.cols[k].personal ? 2 : 1;
      }
    }
    for (k = 0; k < cat.nrows; k++) {
      const GDEFSSQL_CatRow * r = &cat.rows[k];
      /* A kept column the declaration no longer names still counts. */
      if (r->grp == p.groups[i] && (r->role == GDEFSSQL_ROLE_VALUE ||
                                       r->role == GDEFSSQL_ROLE_SUBJECT)) {
        size_t q;
        int named = 0;
        for (q = 0; q < p.ncols; q++) {
          named |= p.cols[q].grp == r->grp && p.cols[q].col == r->col;
        }
        n += named ? 0 : 1;
      }
    }
    if (n > c->b->d.max_columns) {
      rc = GDEFSTOR_ERR_LIMIT;
    }
  }
  if (rc == GDEFSTOR_OK) {
    rc = type_tables(c, &cat, tno);
  }
  for (i = 0; rc == GDEFSTOR_OK && i < p.ngroups; i++) {
    rc = group_tables(c, &cat, &p, tno, p.groups[i]);
  }
  for (i = 0; rc == GDEFSTOR_OK && i < p.ncols; i++) {
    if (have(&cat, p.cols[i].grp, 0, GDEFSSQL_ROLE_GROUP) != NULL &&
        have(&cat, p.cols[i].grp, p.cols[i].col, GDEFSSQL_ROLE_VALUE) == NULL) {
      rc = add_column(c, tno, &p.cols[i]);
    }
  }
  for (i = 0; rc == GDEFSTOR_OK && i < p.ncols; i++) {
    const Want * w = &p.cols[i];
    if (w->indexed && have(&cat, w->grp, w->col, GDEFSSQL_ROLE_INDEX) == NULL) {
      rc = add_index(c, tno, w, GDEFSSQL_ROLE_INDEX);
    }
    if (rc == GDEFSTOR_OK && w->personal &&
        have(&cat, w->grp, w->col, GDEFSSQL_ROLE_SUBJECT_INDEX) == NULL) {
      rc = add_index(c, tno, w, GDEFSSQL_ROLE_SUBJECT_INDEX);
    }
  }
  gdefssql_cat_free(&cat);
  plan_free(&p);
  return rc;
}
