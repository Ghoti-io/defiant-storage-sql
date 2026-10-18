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
 * The entity backend (backend.h): every member of GDEFSTOR_ModelBackend as
 * statements run through the executor.
 *
 * The port decides what every write means (model_backend.h); this file keeps
 * the rows it is handed in the tables of schema.c, and reads them back. The
 * truth is only ever inserted, except `vto`, written once from NULL, and the
 * entity's newest revision and last item; tN_n, tN_cG and dss_entity are
 * derived, written in the same transaction, and rebuilt from the truth.
 */

#include <ghoti.io/defiant-storage-sql/macros.h>
#include "backend_internal.h"

#include <ghoti.io/cutil/allocator.h>
#include <string.h>

typedef struct GDEFSSQL_Ready {
  char * tenant;
  size_t len;
  struct GDEFSSQL_Ready * next;
} Ready;

typedef struct GDEFSSQL_Txn {
  GDEFSSQL_Back * b;
  char * tenant;
  size_t tenant_len;
  GDEFSSQL_Cx cx;
  uint64_t cseq; /* this write's commit sequence, once a revision took it */
  struct GDEFSSQL_Txn * next;
} Txn;

typedef GDEFSSQL_Back Back;
typedef GDEFSSQL_Cx Cx;
typedef GDEFSSQL_Sb Sb;
typedef GDEFSSQL_St St;

/* ---- small things --------------------------------------------------------- */

static int same(const char * a, size_t an, const char * b, size_t bn) {
  return an == bn && (an == 0 || memcmp(a, b, an) == 0);
}

static char * dup(const GDEFSTOR_Allocator * alloc, const void * p, size_t n) {
  char * c = (char *)gcu_allocator_malloc(alloc, n + 1);
  if (c == NULL) {
    return NULL;
  }
  if (n > 0) {
    memcpy(c, p, n);
  }
  c[n] = '\0';
  return c;
}

static GDEFSSQL_Datum dint(int64_t v) {
  GDEFSSQL_Datum d;
  memset(&d, 0, sizeof d);
  d.kind = GDEFSSQL_DATUM_INT;
  d.integer = v;
  return d;
}

static GDEFSSQL_Datum dreal(double v) {
  GDEFSSQL_Datum d;
  memset(&d, 0, sizeof d);
  d.kind = GDEFSSQL_DATUM_REAL;
  d.real = v;
  return d;
}

static GDEFSSQL_Datum dbytes(int kind, const void * p, size_t n) {
  GDEFSSQL_Datum d;
  memset(&d, 0, sizeof d);
  d.kind = kind;
  d.bytes = (const uint8_t *)p;
  d.len = n;
  return d;
}

/* A port number in an int64 column. */
static int fits(uint64_t v) {
  return v <= (uint64_t)INT64_MAX;
}

static void sb_open(Sb * sb, const Cx * c) {
  gdefssql_sb_init(sb, c->b->alloc, &c->b->d, c->qual);
}

/* " FROM <table> "a0"" */
static void from(Sb * sb, const char * table) {
  gdefssql_sb_kw(sb, " FROM ");
  gdefssql_sb_table(sb, table);
  gdefssql_sb_kw(sb, " ");
  gdefssql_sb_ident(sb, "a0");
}

/* "a0"."col" = ? */
static void where_eq(Sb * sb, const char * kw, const char * col,
    const GDEFSSQL_Datum * v) {
  gdefssql_sb_kw(sb, kw);
  gdefssql_sb_col(sb, "a0", col);
  gdefssql_sb_kw(sb, " = ");
  gdefssql_sb_param(sb, v);
}

/* col = ? in an UPDATE or DELETE, which name no alias. */
static void bare_eq(Sb * sb, const char * kw, const char * col,
    const GDEFSSQL_Datum * v) {
  gdefssql_sb_kw(sb, kw);
  gdefssql_sb_ident(sb, col);
  gdefssql_sb_kw(sb, " = ");
  gdefssql_sb_param(sb, v);
}

/* A column and its value, for an INSERT. */
typedef struct Kv {
  const char * col;
  GDEFSSQL_Datum v;
} Kv;

static void insert_sql(Sb * sb, const char * table, const Kv * kv, size_t n) {
  size_t i;
  gdefssql_sb_kw(sb, "INSERT INTO ");
  gdefssql_sb_table(sb, table);
  gdefssql_sb_kw(sb, " (");
  for (i = 0; i < n; i++) {
    gdefssql_sb_kw(sb, i == 0 ? "" : ", ");
    gdefssql_sb_ident(sb, kv[i].col);
  }
  gdefssql_sb_kw(sb, ") VALUES (");
  for (i = 0; i < n; i++) {
    gdefssql_sb_kw(sb, i == 0 ? "" : ", ");
    gdefssql_sb_param(sb, &kv[i].v);
  }
  gdefssql_sb_kw(sb, ")");
}

static GDEFSTOR_Result insert(Cx * c, const char * table, const Kv * kv,
    size_t n) {
  Sb sb;
  GDEFSTOR_Result rc;
  sb_open(&sb, c);
  insert_sql(&sb, table, kv, n);
  rc = gdefssql_cx_run(c, &sb);
  gdefssql_sb_free(&sb);
  return rc;
}

/* Insert, or replace the non-key columns of the row with the first @p nkey
 * columns' values: the dialect's upsert. */
static GDEFSTOR_Result upsert(Cx * c, const char * table, const Kv * kv,
    size_t n, size_t nkey) {
  Sb sb;
  GDEFSTOR_Result rc;
  size_t i;
  sb_open(&sb, c);
  insert_sql(&sb, table, kv, n);
  if (c->b->d.upsert == GDEFSSQL_UPSERT_ON_CONFLICT) {
    gdefssql_sb_kw(&sb, " ON CONFLICT (");
    for (i = 0; i < nkey; i++) {
      gdefssql_sb_kw(&sb, i == 0 ? "" : ", ");
      gdefssql_sb_ident(&sb, kv[i].col);
    }
    gdefssql_sb_kw(&sb, ") DO UPDATE SET ");
    for (i = nkey; i < n; i++) {
      gdefssql_sb_kw(&sb, i == nkey ? "" : ", ");
      gdefssql_sb_ident(&sb, kv[i].col);
      gdefssql_sb_kw(&sb, " = EXCLUDED.");
      gdefssql_sb_ident(&sb, kv[i].col);
    }
  } else {
    gdefssql_sb_kw(&sb, " ON DUPLICATE KEY UPDATE ");
    for (i = nkey; i < n; i++) {
      gdefssql_sb_kw(&sb, i == nkey ? "" : ", ");
      gdefssql_sb_ident(&sb, kv[i].col);
      gdefssql_sb_kw(&sb, " = VALUES(");
      gdefssql_sb_ident(&sb, kv[i].col);
      gdefssql_sb_kw(&sb, ")");
    }
  }
  rc = gdefssql_cx_run(c, &sb);
  gdefssql_sb_free(&sb);
  return rc;
}

/* One of the dialect's transaction statements: the dialect's own text, run
 * as it is, so that a commit or a rollback never needs an allocation. */
static GDEFSTOR_Result txn_sql(Cx * c, const char * sql) {
  GDEFSTOR_Result rc = c->b->x.exec(c->b->x.ctx, c->conn, sql, strlen(sql));
  if (rc != GDEFSTOR_OK) {
    c->failed = 1;
  }
  return rc;
}

/* ---- connections ---------------------------------------------------------- */

static GDEFSTOR_Result checked_qualifier(const Back * b, const char * q) {
  if (q == NULL || q[0] == '\0' || gdefssql_name_ok(q, b->d.max_identifier)) {
    return GDEFSTOR_OK;
  }
  return GDEFSTOR_ERR_INTERNAL;
}

/* A read: the write's own connection, or a snapshot of the last commit. */
typedef struct Rd {
  Cx local;
  Cx * c;
  int own;
  int empty; /* the tenant has no store */
} Rd;

static GDEFSTOR_Result rd_open(Back * b, Txn * x, const char * tenant,
    size_t tenant_len, Rd * r) {
  const char * qual = NULL;
  GDEFSTOR_Result rc;
  memset(r, 0, sizeof *r);
  if (x != NULL) {
    r->c = &x->cx;
    return GDEFSTOR_OK;
  }
  r->own = 1;
  r->c = &r->local;
  r->local.b = b;
  rc = b->x.open(b->x.ctx, tenant, tenant_len, 0, &r->local.conn, &qual);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (r->local.conn == NULL) {
    r->empty = 1;
    return GDEFSTOR_OK;
  }
  r->local.qual = qual;
  rc = checked_qualifier(b, qual);
  if (rc == GDEFSTOR_OK) {
    rc = txn_sql(&r->local, b->d.begin_read);
  }
  if (rc != GDEFSTOR_OK) {
    b->x.close(b->x.ctx, r->local.conn, 1);
    r->local.conn = NULL;
  }
  return rc;
}

static GDEFSTOR_Result rd_close(Rd * r, GDEFSTOR_Result rc) {
  Back * b = r->c->b;
  if (!r->own || r->empty || r->local.conn == NULL) {
    return rc;
  }
  if (rc == GDEFSTOR_OK) {
    rc = txn_sql(&r->local, b->d.commit);
  } else {
    txn_sql(&r->local, b->d.rollback);
    r->local.failed = 1;
  }
  b->x.close(b->x.ctx, r->local.conn, r->local.failed);
  r->local.conn = NULL;
  return rc;
}

/* ---- lookups -------------------------------------------------------------- */

/* The type number and public id of entity row @p rid. */
static GDEFSTOR_Result entity_tno(Cx * c, uint64_t rid, uint32_t * out) {
  Sb sb;
  St s;
  GDEFSTOR_Result rc;
  GDEFSSQL_Datum v = dint((int64_t)rid);
  int row = 0;
  int64_t tno = 0;
  if (!fits(rid)) {
    return GDEFSTOR_ERR_INVALID;
  }
  sb_open(&sb, c);
  gdefssql_sb_kw(&sb, "SELECT ");
  gdefssql_sb_col(&sb, "a0", "tno");
  from(&sb, "dss_entity");
  where_eq(&sb, " WHERE ", "rid", &v);
  rc = gdefssql_st_open(c, &sb, &s);
  if (rc == GDEFSTOR_OK) {
    rc = gdefssql_st_step(&s, &row);
  }
  if (rc == GDEFSTOR_OK && !row) {
    rc = GDEFSTOR_ERR_INVALID;
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefssql_st_int(&s, 0, &tno, NULL);
  }
  if (rc == GDEFSTOR_OK && (tno <= 0 || tno > (int64_t)UINT32_MAX)) {
    rc = GDEFSTOR_ERR_CORRUPT;
  }
  gdefssql_st_close(&s);
  gdefssql_sb_free(&sb);
  if (rc == GDEFSTOR_OK) {
    *out = (uint32_t)tno;
  }
  return rc;
}

/* The name of type number @p tno, allocated. */
static GDEFSTOR_Result type_name(Cx * c, uint32_t tno, char ** out,
    size_t * out_len) {
  Sb sb;
  St s;
  GDEFSTOR_Result rc;
  GDEFSSQL_Datum v = dint(tno);
  int row = 0;
  const uint8_t * p = NULL;
  size_t n = 0;
  sb_open(&sb, c);
  gdefssql_sb_kw(&sb, "SELECT ");
  gdefssql_sb_col(&sb, "a0", "name");
  from(&sb, "dss_type");
  where_eq(&sb, " WHERE ", "tno", &v);
  rc = gdefssql_st_open(c, &sb, &s);
  if (rc == GDEFSTOR_OK) {
    rc = gdefssql_st_step(&s, &row);
  }
  if (rc == GDEFSTOR_OK && !row) {
    rc = GDEFSTOR_ERR_CORRUPT;
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefssql_st_bytes(&s, 0, GDEFSSQL_DATUM_TEXT, &p, &n, NULL);
  }
  if (rc == GDEFSTOR_OK) {
    *out = dup(c->b->alloc, p, n);
    *out_len = n;
    if (*out == NULL) {
      rc = GDEFSTOR_ERR_OOM;
    }
  }
  gdefssql_st_close(&s);
  gdefssql_sb_free(&sb);
  return rc;
}

/* ---- entities ------------------------------------------------------------- */

/* "SELECT e.rid, e.pid, e.creator, e.created, e.rev, e.last_item, n.pub,
 * n.del FROM tN_e a0 JOIN tN_n a1 ON a1.rid = a0.rid" */
static void entity_select(Sb * sb, uint32_t tno) {
  static const char * const e_cols[] = {"rid", "pid", "creator", "created",
      "rev", "last_item"};
  char t[GDEFSSQL_NAME_CAP];
  size_t i;
  gdefssql_sb_kw(sb, "SELECT ");
  for (i = 0; i < 6; i++) {
    gdefssql_sb_col(sb, "a0", e_cols[i]);
    gdefssql_sb_kw(sb, ", ");
  }
  gdefssql_sb_col(sb, "a1", "pub");
  gdefssql_sb_kw(sb, ", ");
  gdefssql_sb_col(sb, "a1", "del");
  gdefssql_name_table(t, tno, GDEFSSQL_ROLE_ENTITY, 0);
  from(sb, t);
  gdefssql_sb_kw(sb, " JOIN ");
  gdefssql_name_table(t, tno, GDEFSSQL_ROLE_NOW, 0);
  gdefssql_sb_table(sb, t);
  gdefssql_sb_kw(sb, " ");
  gdefssql_sb_ident(sb, "a1");
  gdefssql_sb_kw(sb, " ON ");
  gdefssql_sb_col(sb, "a1", "rid");
  gdefssql_sb_kw(sb, " = ");
  gdefssql_sb_col(sb, "a0", "rid");
}

/* Read the current row of an entity_select() into @p e (bytes borrowed). */
static GDEFSTOR_Result entity_read(St * s, const char * type, size_t type_len,
    GDEFSTOR_ModelEntity * e) {
  int64_t v[6];
  int nul = 0;
  const uint8_t * p = NULL;
  size_t n = 0;
  GDEFSTOR_Result rc;
  memset(e, 0, sizeof *e);
  e->type = type;
  e->type_len = type_len;
  rc = gdefssql_st_int(s, 0, &v[0], NULL);
  if (rc == GDEFSTOR_OK) {
    rc = gdefssql_st_bytes(s, 1, GDEFSSQL_DATUM_TEXT, &p, &n, NULL);
    e->public_id = (const char *)p;
    e->public_id_len = n;
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefssql_st_bytes(s, 2, GDEFSSQL_DATUM_TEXT, &p, &n, &nul);
    /* The system created it: stored as no value, read as empty. */
    e->creator = nul ? "" : (const char *)p;
    e->creator_len = nul ? 0 : n;
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefssql_st_int(s, 3, &v[3], NULL);
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefssql_st_int(s, 4, &v[4], NULL);
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefssql_st_int(s, 5, &v[5], NULL);
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefssql_st_int(s, 6, &v[1], NULL);
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefssql_st_int(s, 7, &v[2], NULL);
  }
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (v[0] <= 0 || v[4] < 0 || v[5] < 0) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  if (e->public_id == NULL) {
    e->public_id = "";
  }
  if (e->creator == NULL) {
    e->creator = "";
  }
  e->row = (uint64_t)v[0];
  e->created_ms = v[3];
  e->revision = (uint64_t)v[4];
  e->last_item = (uint64_t)v[5];
  e->published = v[1] != 0;
  e->deleted = v[2] != 0;
  return GDEFSTOR_OK;
}

/* Visit the entities of type @p tno: one row (@p rid non-zero) or all. */
static GDEFSTOR_Result visit_entities(Cx * c, uint32_t tno, uint64_t rid,
    const GDEFSTOR_ModelVisitor * visitor) {
  char * type = NULL;
  size_t type_len = 0;
  Sb sb;
  St s;
  GDEFSTOR_Result rc = type_name(c, tno, &type, &type_len);
  int row = 0;
  int seen = 0;
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  sb_open(&sb, c);
  entity_select(&sb, tno);
  if (rid != 0) {
    GDEFSSQL_Datum v = dint((int64_t)rid);
    where_eq(&sb, " WHERE ", "rid", &v);
  }
  gdefssql_sb_kw(&sb, " ORDER BY ");
  gdefssql_sb_col(&sb, "a0", "rid");
  rc = gdefssql_st_open(c, &sb, &s);
  while (rc == GDEFSTOR_OK) {
    GDEFSTOR_ModelEntity e;
    rc = gdefssql_st_step(&s, &row);
    if (rc != GDEFSTOR_OK || !row) {
      break;
    }
    rc = entity_read(&s, type, type_len, &e);
    if (rc == GDEFSTOR_OK && visitor->entity != NULL) {
      rc = visitor->entity(visitor->ctx, &e);
    }
    seen = 1;
  }
  gdefssql_st_close(&s);
  gdefssql_sb_free(&sb);
  gcu_allocator_free(c->b->alloc, type);
  if (rc == GDEFSTOR_OK && rid != 0 && !seen) {
    /* dss_entity names a row its type's table does not hold. */
    rc = GDEFSTOR_ERR_CORRUPT;
  }
  return rc;
}

/* ---- items ---------------------------------------------------------------- */

/* Visit item versions of group @p g: from the truth (@p truth) or the current
 * table; of entity @p rid or (0) all; with @p as_of, those valid in it. */
static GDEFSTOR_Result visit_items(Cx * c, uint32_t tno,
    const GDEFSSQL_CatGroup * g, int truth, uint64_t rid, uint64_t as_of,
    const GDEFSTOR_ModelVisitor * visitor) {
  char t[GDEFSSQL_NAME_CAP];
  char col[GDEFSSQL_NAME_CAP];
  Sb sb;
  St s;
  GDEFSTOR_Value * vals = NULL;
  size_t nvals = 0;
  size_t i;
  unsigned base = truth ? 6 : 5;
  int row = 0;
  GDEFSTOR_Result rc;
  if (visitor->item == NULL) {
    return GDEFSTOR_OK;
  }
  if (g->ncols > 0) {
    nvals = (size_t)g->cols[g->ncols - 1].col + 1;
    vals = (GDEFSTOR_Value *)gdefssql_mem0(c->b->alloc, nvals * sizeof *vals);
    if (vals == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
  }
  sb_open(&sb, c);
  gdefssql_sb_kw(&sb, "SELECT ");
  gdefssql_sb_col(&sb, "a0", "ent");
  gdefssql_sb_kw(&sb, ", ");
  gdefssql_sb_col(&sb, "a0", "item");
  gdefssql_sb_kw(&sb, ", ");
  gdefssql_sb_col(&sb, "a0", "vfrom");
  if (truth) {
    gdefssql_sb_kw(&sb, ", ");
    gdefssql_sb_col(&sb, "a0", "vto");
  }
  gdefssql_sb_kw(&sb, ", ");
  gdefssql_sb_col(&sb, "a0", "parent");
  gdefssql_sb_kw(&sb, ", ");
  gdefssql_sb_col(&sb, "a0", "ord");
  for (i = 0; i < g->ncols; i++) {
    gdefssql_sb_kw(&sb, ", ");
    gdefssql_name_column(col, g->cols[i].col, 0);
    gdefssql_sb_col(&sb, "a0", col);
    if (g->cols[i].personal) {
      gdefssql_sb_kw(&sb, ", ");
      gdefssql_name_column(col, g->cols[i].col, 1);
      gdefssql_sb_col(&sb, "a0", col);
    }
  }
  gdefssql_name_table(t, tno,
      truth ? GDEFSSQL_ROLE_GROUP : GDEFSSQL_ROLE_CURRENT, g->grp);
  from(&sb, t);
  if (rid != 0) {
    GDEFSSQL_Datum v = dint((int64_t)rid);
    where_eq(&sb, " WHERE ", "ent", &v);
    if (as_of != 0) {
      GDEFSSQL_Datum a = dint((int64_t)as_of);
      gdefssql_sb_kw(&sb, " AND ");
      gdefssql_sb_col(&sb, "a0", "vfrom");
      gdefssql_sb_kw(&sb, " <= ");
      gdefssql_sb_param(&sb, &a);
      gdefssql_sb_kw(&sb, " AND (");
      gdefssql_sb_col(&sb, "a0", "vto");
      gdefssql_sb_kw(&sb, " IS NULL OR ");
      gdefssql_sb_col(&sb, "a0", "vto");
      gdefssql_sb_kw(&sb, " > ");
      gdefssql_sb_param(&sb, &a);
      gdefssql_sb_kw(&sb, ")");
    }
  }
  gdefssql_sb_kw(&sb, " ORDER BY ");
  gdefssql_sb_col(&sb, "a0", "ent");
  gdefssql_sb_kw(&sb, ", ");
  gdefssql_sb_col(&sb, "a0", "item");
  gdefssql_sb_kw(&sb, ", ");
  gdefssql_sb_col(&sb, "a0", "vfrom");
  rc = gdefssql_st_open(c, &sb, &s);
  while (rc == GDEFSTOR_OK) {
    GDEFSTOR_ModelItem it;
    int64_t ent = 0;
    int64_t v = 0;
    int nul = 0;
    unsigned at = 0;
    size_t used = 0;
    rc = gdefssql_st_step(&s, &row);
    if (rc != GDEFSTOR_OK || !row) {
      break;
    }
    memset(&it, 0, sizeof it);
    it.group = g->grp;
    rc = gdefssql_st_int(&s, at++, &ent, NULL);
    if (rc == GDEFSTOR_OK) {
      rc = gdefssql_st_int(&s, at++, &v, NULL);
      it.item = (uint64_t)v;
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefssql_st_int(&s, at++, &v, NULL);
      it.valid_from = (uint64_t)v;
    }
    if (rc == GDEFSTOR_OK && truth) {
      rc = gdefssql_st_int(&s, at++, &v, &nul);
      it.valid_to = nul ? 0 : (uint64_t)v;
      if (rc == GDEFSTOR_OK && !nul && v <= 0) {
        rc = GDEFSTOR_ERR_CORRUPT;
      }
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefssql_st_int(&s, at++, &v, NULL);
      it.parent = (uint64_t)v;
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefssql_st_real(&s, at++, &it.order, NULL);
    }
    if (rc == GDEFSTOR_OK && at != base) {
      rc = GDEFSTOR_ERR_INTERNAL;
    }
    for (i = 0; rc == GDEFSTOR_OK && i < g->ncols; i++) {
      const GDEFSSQL_CatCol * cc = &g->cols[i];
      GDEFSTOR_Value * out = &vals[cc->col];
      memset(out, 0, sizeof *out);
      if (cc->personal) {
        const uint8_t * p = NULL;
        size_t n = 0;
        rc = gdefssql_st_bytes(&s, at++, GDEFSSQL_DATUM_BYTES, &p, &n, &nul);
        if (rc == GDEFSTOR_OK && !nul) {
          out->type = cc->type;
          out->bytes = p;
          out->len = n;
        }
        if (rc == GDEFSTOR_OK) {
          rc = gdefssql_st_bytes(&s, at++, GDEFSSQL_DATUM_TEXT, &p, &n, &nul);
          if (rc == GDEFSTOR_OK && !nul && out->type != 0) {
            out->subject = p;
            out->subject_len = n;
          }
        }
      } else if (cc->type == GDEFSTOR_FIELD_FLOAT) {
        rc = gdefssql_st_real(&s, at++, &out->real, &nul);
        out->type = nul ? 0 : cc->type;
      } else if (cc->type == GDEFSTOR_FIELD_TEXT ||
                 cc->type == GDEFSTOR_FIELD_REFERENCE) {
        const uint8_t * p = NULL;
        size_t n = 0;
        rc = gdefssql_st_bytes(&s, at++, GDEFSSQL_DATUM_TEXT, &p, &n, &nul);
        if (rc == GDEFSTOR_OK && !nul) {
          out->type = cc->type;
          out->bytes = p;
          out->len = n;
        }
      } else {
        rc = gdefssql_st_int(&s, at++, &out->integer, &nul);
        out->type = nul ? 0 : cc->type;
      }
      if (rc == GDEFSTOR_OK && out->type != 0 && cc->col + 1 > used) {
        used = cc->col + 1;
      }
    }
    if (rc != GDEFSTOR_OK) {
      break;
    }
    if (ent <= 0 || it.item == 0) {
      rc = GDEFSTOR_ERR_CORRUPT;
      break;
    }
    /* Trailing columns with no value are not stored differences. */
    it.values = vals;
    it.value_count = used;
    rc = visitor->item(visitor->ctx, (uint64_t)ent, &it);
  }
  gdefssql_st_close(&s);
  gdefssql_sb_free(&sb);
  gcu_allocator_free(c->b->alloc, vals);
  return rc;
}

static GDEFSTOR_Result visit_all_items(Cx * c, uint32_t tno, int truth,
    uint64_t rid, uint64_t as_of, const GDEFSTOR_ModelVisitor * visitor) {
  GDEFSSQL_Cat cat;
  size_t i;
  GDEFSTOR_Result rc = gdefssql_cat_load(c, tno, &cat);
  for (i = 0; rc == GDEFSTOR_OK && i < cat.ngroups; i++) {
    rc = visit_items(c, tno, &cat.groups[i], truth, rid, as_of, visitor);
  }
  gdefssql_cat_free(&cat);
  return rc;
}

/* ---- history -------------------------------------------------------------- */

static GDEFSTOR_Result visit_revisions(Cx * c, uint32_t tno, uint64_t rid,
    const GDEFSTOR_ModelVisitor * visitor) {
  static const char * const cols[] = {"rid", "rev", "cseq", "who", "at",
      "why"};
  char t[GDEFSSQL_NAME_CAP];
  Sb sb;
  St s;
  size_t i;
  int row = 0;
  GDEFSTOR_Result rc;
  if (visitor->revision == NULL) {
    return GDEFSTOR_OK;
  }
  sb_open(&sb, c);
  gdefssql_sb_kw(&sb, "SELECT ");
  for (i = 0; i < 6; i++) {
    gdefssql_sb_kw(&sb, i == 0 ? "" : ", ");
    gdefssql_sb_col(&sb, "a0", cols[i]);
  }
  gdefssql_name_table(t, tno, GDEFSSQL_ROLE_REVISION, 0);
  from(&sb, t);
  if (rid != 0) {
    GDEFSSQL_Datum v = dint((int64_t)rid);
    where_eq(&sb, " WHERE ", "rid", &v);
  }
  gdefssql_sb_kw(&sb, " ORDER BY ");
  gdefssql_sb_col(&sb, "a0", "rid");
  gdefssql_sb_kw(&sb, ", ");
  gdefssql_sb_col(&sb, "a0", "rev");
  rc = gdefssql_st_open(c, &sb, &s);
  while (rc == GDEFSTOR_OK) {
    GDEFSTOR_ModelRevision r;
    int64_t v[3];
    const uint8_t * p = NULL;
    size_t n = 0;
    rc = gdefssql_st_step(&s, &row);
    if (rc != GDEFSTOR_OK || !row) {
      break;
    }
    memset(&r, 0, sizeof r);
    for (i = 0; rc == GDEFSTOR_OK && i < 3; i++) {
      rc = gdefssql_st_int(&s, (unsigned)i, &v[i], NULL);
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefssql_st_bytes(&s, 3, GDEFSSQL_DATUM_TEXT, &p, &n, NULL);
      r.who = p == NULL ? "" : (const char *)p;
      r.who_len = n;
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefssql_st_int(&s, 4, &r.when_ms, NULL);
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefssql_st_bytes(&s, 5, GDEFSSQL_DATUM_TEXT, &p, &n, NULL);
      r.why = p == NULL ? "" : (const char *)p;
      r.why_len = n;
    }
    if (rc == GDEFSTOR_OK && (v[0] <= 0 || v[1] <= 0 || v[2] <= 0)) {
      rc = GDEFSTOR_ERR_CORRUPT;
    }
    if (rc != GDEFSTOR_OK) {
      break;
    }
    r.revision = (uint64_t)v[1];
    r.commit_seq = (uint64_t)v[2];
    rc = visitor->revision(visitor->ctx, (uint64_t)v[0], &r);
  }
  gdefssql_st_close(&s);
  gdefssql_sb_free(&sb);
  return rc;
}

static GDEFSTOR_Result visit_states(Cx * c, uint32_t tno, uint64_t rid,
    const GDEFSTOR_ModelVisitor * visitor) {
  static const char * const cols[] = {"rid", "seq", "pub", "del", "actor",
      "at"};
  char t[GDEFSSQL_NAME_CAP];
  Sb sb;
  St s;
  size_t i;
  int row = 0;
  GDEFSTOR_Result rc;
  if (visitor->state == NULL) {
    return GDEFSTOR_OK;
  }
  sb_open(&sb, c);
  gdefssql_sb_kw(&sb, "SELECT ");
  for (i = 0; i < 6; i++) {
    gdefssql_sb_kw(&sb, i == 0 ? "" : ", ");
    gdefssql_sb_col(&sb, "a0", cols[i]);
  }
  gdefssql_name_table(t, tno, GDEFSSQL_ROLE_STATE, 0);
  from(&sb, t);
  if (rid != 0) {
    GDEFSSQL_Datum v = dint((int64_t)rid);
    where_eq(&sb, " WHERE ", "rid", &v);
  }
  gdefssql_sb_kw(&sb, " ORDER BY ");
  gdefssql_sb_col(&sb, "a0", "rid");
  gdefssql_sb_kw(&sb, ", ");
  gdefssql_sb_col(&sb, "a0", "seq");
  rc = gdefssql_st_open(c, &sb, &s);
  while (rc == GDEFSTOR_OK) {
    GDEFSTOR_ModelState st;
    int64_t v[4];
    const uint8_t * p = NULL;
    size_t n = 0;
    rc = gdefssql_st_step(&s, &row);
    if (rc != GDEFSTOR_OK || !row) {
      break;
    }
    memset(&st, 0, sizeof st);
    for (i = 0; rc == GDEFSTOR_OK && i < 4; i++) {
      rc = gdefssql_st_int(&s, (unsigned)i, &v[i], NULL);
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefssql_st_bytes(&s, 4, GDEFSSQL_DATUM_TEXT, &p, &n, NULL);
      st.actor = p == NULL ? "" : (const char *)p;
      st.actor_len = n;
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefssql_st_int(&s, 5, &st.when_ms, NULL);
    }
    if (rc == GDEFSTOR_OK && (v[0] <= 0 || v[1] <= 0)) {
      rc = GDEFSTOR_ERR_CORRUPT;
    }
    if (rc != GDEFSTOR_OK) {
      break;
    }
    st.seq = (uint64_t)v[1];
    st.published = v[2] != 0;
    st.deleted = v[3] != 0;
    rc = visitor->state(visitor->ctx, (uint64_t)v[0], &st);
  }
  gdefssql_st_close(&s);
  gdefssql_sb_free(&sb);
  return rc;
}

/* ---- the members ---------------------------------------------------------- */

static Ready * ready_find(Back * b, const char * t, size_t n) {
  Ready * r;
  for (r = b->ready; r != NULL; r = r->next) {
    if (same(r->tenant, r->len, t, n)) {
      return r;
    }
  }
  return NULL;
}

static void txn_free(Back * b, Txn * x) {
  Txn ** pp = &b->open;
  while (*pp != NULL) {
    if (*pp == x) {
      *pp = x->next;
      break;
    }
    pp = &(*pp)->next;
  }
  gcu_allocator_free(b->alloc, x->tenant);
  gcu_allocator_free(b->alloc, x);
}

static GDEFSTOR_Result m_begin(void * ctx, const char * tenant,
    size_t tenant_len, void ** out_txn) {
  Back * b = (Back *)ctx;
  Txn * x;
  Cx c;
  const char * qual = NULL;
  GDEFSTOR_Result rc;
  if (out_txn == NULL || (tenant == NULL && tenant_len != 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  for (x = b->open; x != NULL; x = x->next) {
    if (same(x->tenant, x->tenant_len, tenant, tenant_len)) {
      return GDEFSTOR_ERR_INTERNAL;
    }
  }
  memset(&c, 0, sizeof c);
  c.b = b;
  rc = b->x.open(b->x.ctx, tenant, tenant_len, 1, &c.conn, &qual);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (c.conn == NULL) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  c.qual = qual;
  rc = checked_qualifier(b, qual);
  if (rc == GDEFSTOR_OK && ready_find(b, tenant, tenant_len) == NULL) {
    /* The tenant tables, in a transaction of their own before any data. */
    rc = txn_sql(&c, b->d.begin_write);
    if (rc == GDEFSTOR_OK) {
      rc = gdefssql_tenant_init(&c);
      if (rc == GDEFSTOR_OK) {
        rc = txn_sql(&c, b->d.commit);
      } else {
        txn_sql(&c, b->d.rollback);
      }
    }
    if (rc == GDEFSTOR_OK) {
      Ready * r = (Ready *)gdefssql_mem0(b->alloc, sizeof *r);
      if (r != NULL) {
        r->tenant = dup(b->alloc, tenant, tenant_len);
        r->len = tenant_len;
        if (r->tenant == NULL) {
          gcu_allocator_free(b->alloc, r);
        } else {
          r->next = b->ready;
          b->ready = r;
        }
      }
      /* Not remembering it costs only doing it again next time. */
    }
  }
  if (rc == GDEFSTOR_OK) {
    rc = txn_sql(&c, b->d.begin_write);
  }
  x = NULL;
  if (rc == GDEFSTOR_OK) {
    x = (Txn *)gdefssql_mem0(b->alloc, sizeof *x);
    if (x != NULL) {
      x->tenant = dup(b->alloc, tenant, tenant_len);
    }
    if (x == NULL || x->tenant == NULL) {
      gcu_allocator_free(b->alloc, x);
      x = NULL;
      txn_sql(&c, b->d.rollback);
      rc = GDEFSTOR_ERR_OOM;
    }
  }
  if (rc != GDEFSTOR_OK) {
    b->x.close(b->x.ctx, c.conn, 1);
    return rc;
  }
  x->b = b;
  x->tenant_len = tenant_len;
  x->cx = c;
  x->next = b->open;
  b->open = x;
  *out_txn = x;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result m_commit(void * ctx, void * txn) {
  Back * b = (Back *)ctx;
  Txn * x = (Txn *)txn;
  GDEFSTOR_Result rc;
  if (x == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (x->cx.failed) {
    /* A statement of this write failed: what it left is not the write that
     * was asked for (an engine may even report a commit of an aborted
     * transaction as success), so it is rolled back, never committed. */
    txn_sql(&x->cx, b->d.rollback);
    rc = GDEFSTOR_ERR_IO;
  } else {
    rc = txn_sql(&x->cx, b->d.commit);
    if (rc != GDEFSTOR_OK) {
      txn_sql(&x->cx, b->d.rollback);
    }
  }
  b->x.close(b->x.ctx, x->cx.conn, x->cx.failed);
  txn_free(b, x);
  return rc;
}

static GDEFSTOR_Result m_abort(void * ctx, void * txn) {
  Back * b = (Back *)ctx;
  Txn * x = (Txn *)txn;
  GDEFSTOR_Result rc;
  if (x == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = txn_sql(&x->cx, b->d.rollback);
  /* A write that met an error, or whose rollback did, leaves a connection
   * that is not trusted again (AD-14). */
  b->x.close(b->x.ctx, x->cx.conn, x->cx.failed);
  txn_free(b, x);
  return rc;
}

static GDEFSTOR_Result m_type_load(void * ctx, void * txn, const char * tenant,
    size_t tenant_len, const char * type, size_t type_len, uint8_t ** out_text,
    size_t * out_len) {
  Back * b = (Back *)ctx;
  Rd r;
  Sb sb;
  St s;
  GDEFSSQL_Datum v = dbytes(GDEFSSQL_DATUM_TEXT, type, type_len);
  int row = 0;
  uint8_t * text = NULL;
  size_t n = 0;
  GDEFSTOR_Result rc;
  if (out_text == NULL || out_len == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = rd_open(b, (Txn *)txn, tenant, tenant_len, &r);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (r.empty) {
    return GDEFSTOR_ERR_INVALID;
  }
  sb_open(&sb, r.c);
  gdefssql_sb_kw(&sb, "SELECT ");
  gdefssql_sb_col(&sb, "a0", "decl");
  from(&sb, "dss_type");
  where_eq(&sb, " WHERE ", "name", &v);
  rc = gdefssql_st_open(r.c, &sb, &s);
  if (rc == GDEFSTOR_OK) {
    rc = gdefssql_st_step(&s, &row);
  }
  if (rc == GDEFSTOR_OK && !row) {
    rc = GDEFSTOR_ERR_INVALID;
  }
  if (rc == GDEFSTOR_OK) {
    const uint8_t * p = NULL;
    rc = gdefssql_st_bytes(&s, 0, GDEFSSQL_DATUM_BYTES, &p, &n, NULL);
    if (rc == GDEFSTOR_OK) {
      text = (uint8_t *)dup(b->alloc, p, n);
      if (text == NULL) {
        rc = GDEFSTOR_ERR_OOM;
      }
    }
  }
  gdefssql_st_close(&s);
  gdefssql_sb_free(&sb);
  /* A missing type is an answer, not a failed read. */
  if (rc == GDEFSTOR_ERR_INVALID) {
    GDEFSTOR_Result end = rd_close(&r, GDEFSTOR_OK);
    return end == GDEFSTOR_OK ? GDEFSTOR_ERR_INVALID : end;
  }
  rc = rd_close(&r, rc);
  if (rc != GDEFSTOR_OK) {
    /* Written only on success: a read whose end failed hands back nothing. */
    gcu_allocator_free(b->alloc, text);
    return rc;
  }
  *out_text = text;
  *out_len = n;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result m_type_store(void * ctx, void * txn,
    const GDEFSTOR_Type * decl, const uint8_t * text, size_t text_len) {
  Back * b = (Back *)ctx;
  Txn * x = (Txn *)txn;
  const char * name = NULL;
  size_t name_len = 0;
  uint32_t tno = 0;
  GDEFSTOR_Result rc;
  (void)b;
  if (x == NULL || decl == NULL || (text == NULL && text_len != 0) ||
      gdefstor_type_name(decl, &name, &name_len) != GDEFSTOR_OK) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = gdefssql_type_number(&x->cx, name, name_len, &tno);
  if (rc == GDEFSTOR_OK && tno == 0) {
    uint64_t next = 0;
    rc = gdefssql_cx_counter(&x->cx, GDEFSSQL_COUNTER_TYPE, &next);
    if (rc == GDEFSTOR_OK && next > UINT32_MAX) {
      rc = GDEFSTOR_ERR_LIMIT;
    }
    tno = (uint32_t)next;
  }
  if (rc == GDEFSTOR_OK) {
    Kv kv[3];
    kv[0].col = "name";
    kv[0].v = dbytes(GDEFSSQL_DATUM_TEXT, name, name_len);
    kv[1].col = "tno";
    kv[1].v = dint(tno);
    kv[2].col = "decl";
    kv[2].v = dbytes(GDEFSSQL_DATUM_BYTES, text, text_len);
    rc = upsert(&x->cx, "dss_type", kv, 3, 1);
  }
  if (rc == GDEFSTOR_OK) {
    rc = gdefssql_type_steps(&x->cx, tno, decl);
  }
  return rc;
}

static GDEFSTOR_Result m_entity_find(void * ctx, void * txn,
    const char * tenant, size_t tenant_len, const char * public_id,
    size_t public_id_len, const GDEFSTOR_ModelVisitor * visitor) {
  Back * b = (Back *)ctx;
  Rd r;
  Sb sb;
  St s;
  GDEFSSQL_Datum v = dbytes(GDEFSSQL_DATUM_TEXT, public_id, public_id_len);
  int row = 0;
  int64_t rid = 0;
  int64_t tno = 0;
  GDEFSTOR_Result rc = rd_open(b, (Txn *)txn, tenant, tenant_len, &r);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (r.empty) {
    return GDEFSTOR_ERR_INVALID;
  }
  sb_open(&sb, r.c);
  gdefssql_sb_kw(&sb, "SELECT ");
  gdefssql_sb_col(&sb, "a0", "rid");
  gdefssql_sb_kw(&sb, ", ");
  gdefssql_sb_col(&sb, "a0", "tno");
  from(&sb, "dss_entity");
  where_eq(&sb, " WHERE ", "pid", &v);
  rc = gdefssql_st_open(r.c, &sb, &s);
  if (rc == GDEFSTOR_OK) {
    rc = gdefssql_st_step(&s, &row);
  }
  if (rc == GDEFSTOR_OK && row) {
    rc = gdefssql_st_int(&s, 0, &rid, NULL);
    if (rc == GDEFSTOR_OK) {
      rc = gdefssql_st_int(&s, 1, &tno, NULL);
    }
    if (rc == GDEFSTOR_OK &&
        (rid <= 0 || tno <= 0 || tno > (int64_t)UINT32_MAX)) {
      rc = GDEFSTOR_ERR_CORRUPT;
    }
  }
  gdefssql_st_close(&s);
  gdefssql_sb_free(&sb);
  if (rc == GDEFSTOR_OK && row) {
    rc = visit_entities(r.c, (uint32_t)tno, (uint64_t)rid, visitor);
  }
  rc = rd_close(&r, rc);
  if (rc == GDEFSTOR_OK && !row) {
    return GDEFSTOR_ERR_INVALID;
  }
  return rc;
}

static GDEFSTOR_Result m_entity_insert(void * ctx, void * txn,
    const GDEFSTOR_ModelEntity * e, uint64_t * out_row) {
  Back * b = (Back *)ctx;
  Txn * x = (Txn *)txn;
  uint32_t tno = 0;
  uint64_t rid = 0;
  char t[GDEFSSQL_NAME_CAP];
  GDEFSTOR_Result rc;
  (void)b;
  if (x == NULL || e == NULL || out_row == NULL || e->type == NULL ||
      e->public_id == NULL || (e->creator == NULL && e->creator_len != 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = gdefssql_type_number(&x->cx, e->type, e->type_len, &tno);
  if (rc == GDEFSTOR_OK && tno == 0) {
    rc = GDEFSTOR_ERR_INVALID;
  }
  if (rc == GDEFSTOR_OK) {
    /* Rows increase and are never reused: a counter in the write. */
    rc = gdefssql_cx_counter(&x->cx, GDEFSSQL_COUNTER_ROW, &rid);
  }
  if (rc == GDEFSTOR_OK) {
    Kv kv[6];
    size_t n = 0;
    kv[n].col = "rid";
    kv[n++].v = dint((int64_t)rid);
    kv[n].col = "pid";
    kv[n++].v = dbytes(GDEFSSQL_DATUM_TEXT, e->public_id, e->public_id_len);
    if (e->creator_len > 0) {
      kv[n].col = "creator";
      kv[n++].v = dbytes(GDEFSSQL_DATUM_TEXT, e->creator, e->creator_len);
    }
    kv[n].col = "created";
    kv[n++].v = dint(e->created_ms);
    kv[n].col = "rev";
    kv[n++].v = dint(0);
    kv[n].col = "last_item";
    kv[n++].v = dint(0);
    gdefssql_name_table(t, tno, GDEFSSQL_ROLE_ENTITY, 0);
    rc = insert(&x->cx, t, kv, n);
  }
  if (rc == GDEFSTOR_OK) {
    Kv kv[3];
    kv[0].col = "rid";
    kv[0].v = dint((int64_t)rid);
    kv[1].col = "pub";
    kv[1].v = dint(0);
    kv[2].col = "del";
    kv[2].v = dint(0);
    gdefssql_name_table(t, tno, GDEFSSQL_ROLE_NOW, 0);
    rc = insert(&x->cx, t, kv, 3);
  }
  if (rc == GDEFSTOR_OK) {
    Kv kv[3];
    kv[0].col = "pid";
    kv[0].v = dbytes(GDEFSSQL_DATUM_TEXT, e->public_id, e->public_id_len);
    kv[1].col = "rid";
    kv[1].v = dint((int64_t)rid);
    kv[2].col = "tno";
    kv[2].v = dint(tno);
    rc = insert(&x->cx, "dss_entity", kv, 3);
  }
  if (rc == GDEFSTOR_OK) {
    *out_row = rid;
  }
  return rc;
}

static GDEFSTOR_Result m_state_append(void * ctx, void * txn, uint64_t row,
    const GDEFSTOR_ModelState * st) {
  Back * b = (Back *)ctx;
  Txn * x = (Txn *)txn;
  uint32_t tno = 0;
  int64_t seq = 0;
  char t[GDEFSSQL_NAME_CAP];
  GDEFSTOR_Result rc;
  (void)b;
  if (x == NULL || st == NULL || (st->actor == NULL && st->actor_len != 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = entity_tno(&x->cx, row, &tno);
  if (rc == GDEFSTOR_ERR_INVALID) {
    rc = GDEFSTOR_ERR_INTERNAL;
  }
  if (rc == GDEFSTOR_OK) {
    /* The entity's last state number: the newest row, by the key. */
    Sb sb;
    St s;
    GDEFSSQL_Datum v = dint((int64_t)row);
    int got = 0;
    sb_open(&sb, &x->cx);
    gdefssql_sb_kw(&sb, "SELECT ");
    gdefssql_sb_col(&sb, "a0", "seq");
    gdefssql_name_table(t, tno, GDEFSSQL_ROLE_STATE, 0);
    from(&sb, t);
    where_eq(&sb, " WHERE ", "rid", &v);
    gdefssql_sb_kw(&sb, " ORDER BY ");
    gdefssql_sb_col(&sb, "a0", "seq");
    gdefssql_sb_kw(&sb, " DESC LIMIT ");
    gdefssql_sb_int(&sb, 1);
    gdefssql_sb_kw(&sb, " OFFSET ");
    gdefssql_sb_int(&sb, 0);
    rc = gdefssql_st_open(&x->cx, &sb, &s);
    if (rc == GDEFSTOR_OK) {
      rc = gdefssql_st_step(&s, &got);
    }
    if (rc == GDEFSTOR_OK && got) {
      rc = gdefssql_st_int(&s, 0, &seq, NULL);
    }
    gdefssql_st_close(&s);
    gdefssql_sb_free(&sb);
    if (rc == GDEFSTOR_OK && (seq < 0 || seq == INT64_MAX)) {
      rc = GDEFSTOR_ERR_CORRUPT;
    }
  }
  if (rc == GDEFSTOR_OK) {
    Kv kv[6];
    kv[0].col = "rid";
    kv[0].v = dint((int64_t)row);
    kv[1].col = "seq";
    kv[1].v = dint(seq + 1);
    kv[2].col = "pub";
    kv[2].v = dint(st->published != 0);
    kv[3].col = "del";
    kv[3].v = dint(st->deleted != 0);
    kv[4].col = "actor";
    kv[4].v = dbytes(GDEFSSQL_DATUM_TEXT, st->actor, st->actor_len);
    kv[5].col = "at";
    kv[5].v = dint(st->when_ms);
    gdefssql_name_table(t, tno, GDEFSSQL_ROLE_STATE, 0);
    rc = insert(&x->cx, t, kv, 6);
  }
  if (rc == GDEFSTOR_OK) {
    /* The newest state follows it (derived). */
    Sb sb;
    GDEFSSQL_Datum pub = dint(st->published != 0);
    GDEFSSQL_Datum del = dint(st->deleted != 0);
    GDEFSSQL_Datum v = dint((int64_t)row);
    sb_open(&sb, &x->cx);
    gdefssql_sb_kw(&sb, "UPDATE ");
    gdefssql_name_table(t, tno, GDEFSSQL_ROLE_NOW, 0);
    gdefssql_sb_table(&sb, t);
    bare_eq(&sb, " SET ", "pub", &pub);
    bare_eq(&sb, ", ", "del", &del);
    bare_eq(&sb, " WHERE ", "rid", &v);
    rc = gdefssql_cx_run(&x->cx, &sb);
    gdefssql_sb_free(&sb);
  }
  return rc;
}

/* Close one open version: valid_to from NULL to @p rev, once. */
static GDEFSTOR_Result close_version(Cx * c, uint32_t tno,
    const GDEFSTOR_ModelClose * cl, uint64_t row, uint64_t rev) {
  char t[GDEFSSQL_NAME_CAP];
  Sb sb;
  St s;
  GDEFSSQL_Datum ent = dint((int64_t)row);
  GDEFSSQL_Datum item = dint((int64_t)cl->item);
  GDEFSSQL_Datum from_rev = dint((int64_t)cl->valid_from);
  GDEFSSQL_Datum to = dint((int64_t)rev);
  int got = 0;
  int nul = 0;
  int64_t vto = 0;
  GDEFSTOR_Result rc;
  if (!fits(cl->item) || !fits(cl->valid_from)) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  gdefssql_name_table(t, tno, GDEFSSQL_ROLE_GROUP, cl->group);
  sb_open(&sb, c);
  gdefssql_sb_kw(&sb, "SELECT ");
  gdefssql_sb_col(&sb, "a0", "vto");
  from(&sb, t);
  where_eq(&sb, " WHERE ", "ent", &ent);
  where_eq(&sb, " AND ", "item", &item);
  where_eq(&sb, " AND ", "vfrom", &from_rev);
  rc = gdefssql_st_open(c, &sb, &s);
  if (rc == GDEFSTOR_OK) {
    rc = gdefssql_st_step(&s, &got);
  }
  if (rc == GDEFSTOR_OK && got) {
    rc = gdefssql_st_int(&s, 0, &vto, &nul);
  }
  gdefssql_st_close(&s);
  /* valid_to is written once: a version that is not there, or not open, is
   * the port's invariant broken, and nothing is changed. */
  if (rc == GDEFSTOR_OK && (!got || !nul)) {
    rc = GDEFSTOR_ERR_INTERNAL;
  }
  if (rc == GDEFSTOR_OK) {
    gdefssql_sb_reset(&sb);
    gdefssql_sb_kw(&sb, "UPDATE ");
    gdefssql_sb_table(&sb, t);
    bare_eq(&sb, " SET ", "vto", &to);
    bare_eq(&sb, " WHERE ", "ent", &ent);
    bare_eq(&sb, " AND ", "item", &item);
    bare_eq(&sb, " AND ", "vfrom", &from_rev);
    rc = gdefssql_cx_run(c, &sb);
  }
  if (rc == GDEFSTOR_OK) {
    gdefssql_name_table(t, tno, GDEFSSQL_ROLE_CURRENT, cl->group);
    gdefssql_sb_reset(&sb);
    gdefssql_sb_kw(&sb, "DELETE FROM ");
    gdefssql_sb_table(&sb, t);
    bare_eq(&sb, " WHERE ", "ent", &ent);
    bare_eq(&sb, " AND ", "item", &item);
    rc = gdefssql_cx_run(c, &sb);
  }
  gdefssql_sb_free(&sb);
  return rc;
}

/* Insert a new version into the group's truth and current tables. */
static GDEFSTOR_Result insert_version(Cx * c, uint32_t tno,
    const GDEFSSQL_Cat * cat, const GDEFSTOR_ModelItem * it, uint64_t row,
    uint64_t rev) {
  const GDEFSSQL_CatGroup * g = gdefssql_cat_group(cat, it->group);
  /* Fixed columns, then up to two per value: the names live in @p names. */
  Kv * kv;
  char (*names)[GDEFSSQL_NAME_CAP];
  size_t n = 0;
  size_t k;
  int pass;
  GDEFSTOR_Result rc = GDEFSTOR_OK;
  if (g == NULL || !fits(it->item) || !fits(it->parent) || it->item == 0 ||
      (it->values == NULL && it->value_count != 0) ||
      !(it->order == it->order) || it->order - it->order != 0.0) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  kv = (Kv *)gdefssql_mem0(c->b->alloc, (5 + 2 * it->value_count) * sizeof *kv);
  names = (char (*)[GDEFSSQL_NAME_CAP])gdefssql_mem0(c->b->alloc,
      (2 * it->value_count + 1) * sizeof *names);
  if (kv == NULL || names == NULL) {
    gcu_allocator_free(c->b->alloc, kv);
    gcu_allocator_free(c->b->alloc, names);
    return GDEFSTOR_ERR_OOM;
  }
  kv[n].col = "ent";
  kv[n++].v = dint((int64_t)row);
  kv[n].col = "item";
  kv[n++].v = dint((int64_t)it->item);
  kv[n].col = "vfrom";
  kv[n++].v = dint((int64_t)rev);
  kv[n].col = "parent";
  kv[n++].v = dint((int64_t)it->parent);
  kv[n].col = "ord";
  kv[n++].v = dreal(it->order == 0.0 ? 0.0 : it->order);
  for (k = 0; rc == GDEFSTOR_OK && k < it->value_count; k++) {
    const GDEFSTOR_Value * v = &it->values[k];
    const GDEFSSQL_CatCol * cc = NULL;
    size_t q;
    if (v->type == 0) {
      /* No value: the column is left out, and is NULL. */
      continue;
    }
    for (q = 0; q < g->ncols; q++) {
      if (g->cols[q].col == k) {
        cc = &g->cols[q];
      }
    }
    if (cc == NULL || cc->type != v->type ||
        (v->bytes == NULL && v->len != 0) ||
        (v->subject == NULL && v->subject_len != 0)) {
      rc = GDEFSTOR_ERR_INTERNAL;
      break;
    }
    gdefssql_name_column(names[2 * k], (uint32_t)k, 0);
    kv[n].col = names[2 * k];
    if (cc->personal) {
      kv[n++].v = dbytes(GDEFSSQL_DATUM_BYTES, v->bytes, v->len);
      if (v->subject_len > 0) {
        gdefssql_name_column(names[2 * k + 1], (uint32_t)k, 1);
        kv[n].col = names[2 * k + 1];
        kv[n++].v = dbytes(GDEFSSQL_DATUM_TEXT, v->subject, v->subject_len);
      }
    } else {
      gdefssql_value_datum(v, &kv[n++].v);
    }
  }
  for (pass = 0; rc == GDEFSTOR_OK && pass < 2; pass++) {
    char t[GDEFSSQL_NAME_CAP];
    gdefssql_name_table(t, tno,
        pass == 0 ? GDEFSSQL_ROLE_GROUP : GDEFSSQL_ROLE_CURRENT, it->group);
    rc = insert(c, t, kv, n);
  }
  gcu_allocator_free(c->b->alloc, kv);
  gcu_allocator_free(c->b->alloc, names);
  return rc;
}

static GDEFSTOR_Result m_apply(void * ctx, void * txn,
    const GDEFSTOR_ModelApply * a) {
  Back * b = (Back *)ctx;
  Txn * x = (Txn *)txn;
  const GDEFSTOR_ModelRevision * r;
  GDEFSSQL_Cat cat;
  uint32_t tno = 0;
  size_t i;
  char t[GDEFSSQL_NAME_CAP];
  GDEFSTOR_Result rc;
  (void)b;
  if (x == NULL || a == NULL || a->revision == NULL ||
      (a->closes == NULL && a->close_count != 0) ||
      (a->inserts == NULL && a->insert_count != 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  r = a->revision;
  if (!fits(r->revision) || r->revision == 0 || !fits(a->last_item) ||
      (r->who == NULL && r->who_len != 0) ||
      (r->why == NULL && r->why_len != 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = entity_tno(&x->cx, a->row, &tno);
  if (rc == GDEFSTOR_ERR_INVALID) {
    rc = GDEFSTOR_ERR_INTERNAL;
  }
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = gdefssql_cat_load(&x->cx, tno, &cat);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (x->cseq == 0) {
    /* One commit sequence for every revision this write makes. */
    rc = gdefssql_cx_counter(&x->cx, GDEFSSQL_COUNTER_CSEQ, &x->cseq);
    if (rc != GDEFSTOR_OK) {
      x->cseq = 0;
    }
  }
  for (i = 0; rc == GDEFSTOR_OK && i < a->close_count; i++) {
    if (gdefssql_cat_group(&cat, a->closes[i].group) == NULL) {
      rc = GDEFSTOR_ERR_INTERNAL;
      break;
    }
    rc = close_version(&x->cx, tno, &a->closes[i], a->row, r->revision);
  }
  for (i = 0; rc == GDEFSTOR_OK && i < a->insert_count; i++) {
    rc = insert_version(&x->cx, tno, &cat, &a->inserts[i], a->row,
        r->revision);
  }
  gdefssql_cat_free(&cat);
  if (rc == GDEFSTOR_OK) {
    Kv kv[6];
    kv[0].col = "rid";
    kv[0].v = dint((int64_t)a->row);
    kv[1].col = "rev";
    kv[1].v = dint((int64_t)r->revision);
    kv[2].col = "cseq";
    kv[2].v = dint((int64_t)x->cseq);
    kv[3].col = "who";
    kv[3].v = dbytes(GDEFSSQL_DATUM_TEXT, r->who, r->who_len);
    kv[4].col = "at";
    kv[4].v = dint(r->when_ms);
    kv[5].col = "why";
    kv[5].v = dbytes(GDEFSSQL_DATUM_TEXT, r->why, r->why_len);
    gdefssql_name_table(t, tno, GDEFSSQL_ROLE_REVISION, 0);
    rc = insert(&x->cx, t, kv, 6);
  }
  if (rc == GDEFSTOR_OK) {
    Sb sb;
    GDEFSSQL_Datum rev = dint((int64_t)r->revision);
    GDEFSSQL_Datum last = dint((int64_t)a->last_item);
    GDEFSSQL_Datum rid = dint((int64_t)a->row);
    sb_open(&sb, &x->cx);
    gdefssql_sb_kw(&sb, "UPDATE ");
    gdefssql_name_table(t, tno, GDEFSSQL_ROLE_ENTITY, 0);
    gdefssql_sb_table(&sb, t);
    bare_eq(&sb, " SET ", "rev", &rev);
    bare_eq(&sb, ", ", "last_item", &last);
    bare_eq(&sb, " WHERE ", "rid", &rid);
    rc = gdefssql_cx_run(&x->cx, &sb);
    gdefssql_sb_free(&sb);
  }
  return rc;
}

static GDEFSTOR_Result m_history(void * ctx, void * txn, const char * tenant,
    size_t tenant_len, uint64_t row, const GDEFSTOR_ModelVisitor * visitor) {
  Back * b = (Back *)ctx;
  Rd r;
  uint32_t tno = 0;
  GDEFSTOR_Result rc = rd_open(b, (Txn *)txn, tenant, tenant_len, &r);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (r.empty) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = entity_tno(r.c, row, &tno);
  if (rc == GDEFSTOR_OK) {
    rc = visit_revisions(r.c, tno, row, visitor);
  }
  if (rc == GDEFSTOR_OK) {
    rc = visit_states(r.c, tno, row, visitor);
  }
  if (rc == GDEFSTOR_ERR_INVALID) {
    GDEFSTOR_Result end = rd_close(&r, GDEFSTOR_OK);
    return end == GDEFSTOR_OK ? GDEFSTOR_ERR_INVALID : end;
  }
  return rd_close(&r, rc);
}

static GDEFSTOR_Result m_items(void * ctx, void * txn, const char * tenant,
    size_t tenant_len, uint64_t row, uint64_t as_of,
    const GDEFSTOR_ModelVisitor * visitor) {
  Back * b = (Back *)ctx;
  Rd r;
  uint32_t tno = 0;
  GDEFSTOR_Result rc;
  if (!fits(as_of)) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = rd_open(b, (Txn *)txn, tenant, tenant_len, &r);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (r.empty) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = entity_tno(r.c, row, &tno);
  if (rc == GDEFSTOR_OK) {
    rc = visit_all_items(r.c, tno, as_of != 0, row, as_of, visitor);
  }
  if (rc == GDEFSTOR_ERR_INVALID) {
    GDEFSTOR_Result end = rd_close(&r, GDEFSTOR_OK);
    return end == GDEFSTOR_OK ? GDEFSTOR_ERR_INVALID : end;
  }
  return rd_close(&r, rc);
}

static GDEFSTOR_Result m_draft_put(void * ctx, void * txn,
    const GDEFSTOR_ModelDraft * d) {
  Txn * x = (Txn *)txn;
  Kv kv[4];
  (void)ctx;
  if (x == NULL || d == NULL || d->public_id == NULL || d->type == NULL ||
      (d->json == NULL && d->json_len != 0) || !fits(d->base_revision)) {
    return GDEFSTOR_ERR_INVALID;
  }
  kv[0].col = "pid";
  kv[0].v = dbytes(GDEFSSQL_DATUM_TEXT, d->public_id, d->public_id_len);
  kv[1].col = "type";
  kv[1].v = dbytes(GDEFSSQL_DATUM_TEXT, d->type, d->type_len);
  kv[2].col = "base";
  kv[2].v = dint((int64_t)d->base_revision);
  kv[3].col = "body";
  kv[3].v = dbytes(GDEFSSQL_DATUM_BYTES, d->json, d->json_len);
  return upsert(&x->cx, "dss_draft", kv, 4, 1);
}

/* Visit drafts: one public id, or (NULL) all. *out_seen counts them. */
static GDEFSTOR_Result visit_drafts(Cx * c, const char * pid, size_t pid_len,
    const GDEFSTOR_ModelVisitor * visitor, int * out_seen) {
  Sb sb;
  St s;
  int row = 0;
  GDEFSTOR_Result rc;
  *out_seen = 0;
  sb_open(&sb, c);
  gdefssql_sb_kw(&sb, "SELECT ");
  gdefssql_sb_col(&sb, "a0", "pid");
  gdefssql_sb_kw(&sb, ", ");
  gdefssql_sb_col(&sb, "a0", "type");
  gdefssql_sb_kw(&sb, ", ");
  gdefssql_sb_col(&sb, "a0", "base");
  gdefssql_sb_kw(&sb, ", ");
  gdefssql_sb_col(&sb, "a0", "body");
  from(&sb, "dss_draft");
  if (pid != NULL) {
    GDEFSSQL_Datum v = dbytes(GDEFSSQL_DATUM_TEXT, pid, pid_len);
    where_eq(&sb, " WHERE ", "pid", &v);
  }
  gdefssql_sb_kw(&sb, " ORDER BY ");
  gdefssql_sb_col(&sb, "a0", "pid");
  rc = gdefssql_st_open(c, &sb, &s);
  while (rc == GDEFSTOR_OK) {
    GDEFSTOR_ModelDraft d;
    const uint8_t * p = NULL;
    size_t n = 0;
    int64_t base = 0;
    rc = gdefssql_st_step(&s, &row);
    if (rc != GDEFSTOR_OK || !row) {
      break;
    }
    memset(&d, 0, sizeof d);
    rc = gdefssql_st_bytes(&s, 0, GDEFSSQL_DATUM_TEXT, &p, &n, NULL);
    d.public_id = p == NULL ? "" : (const char *)p;
    d.public_id_len = n;
    if (rc == GDEFSTOR_OK) {
      rc = gdefssql_st_bytes(&s, 1, GDEFSSQL_DATUM_TEXT, &p, &n, NULL);
      d.type = p == NULL ? "" : (const char *)p;
      d.type_len = n;
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefssql_st_int(&s, 2, &base, NULL);
      d.base_revision = (uint64_t)base;
      if (rc == GDEFSTOR_OK && base < 0) {
        rc = GDEFSTOR_ERR_CORRUPT;
      }
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefssql_st_bytes(&s, 3, GDEFSSQL_DATUM_BYTES, &d.json, &d.json_len,
          NULL);
    }
    if (rc == GDEFSTOR_OK && visitor->draft != NULL) {
      rc = visitor->draft(visitor->ctx, &d);
    }
    *out_seen += 1;
  }
  gdefssql_st_close(&s);
  gdefssql_sb_free(&sb);
  return rc;
}

static GDEFSTOR_Result m_draft_get(void * ctx, void * txn, const char * tenant,
    size_t tenant_len, const char * public_id, size_t public_id_len,
    const GDEFSTOR_ModelVisitor * visitor) {
  Back * b = (Back *)ctx;
  Rd r;
  int seen = 0;
  GDEFSTOR_Result rc;
  if (public_id == NULL || visitor == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = rd_open(b, (Txn *)txn, tenant, tenant_len, &r);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (r.empty) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = visit_drafts(r.c, public_id, public_id_len, visitor, &seen);
  rc = rd_close(&r, rc);
  if (rc == GDEFSTOR_OK && seen == 0) {
    return GDEFSTOR_ERR_INVALID;
  }
  return rc;
}

static GDEFSTOR_Result m_draft_delete(void * ctx, void * txn,
    const char * public_id, size_t public_id_len) {
  Txn * x = (Txn *)txn;
  GDEFSTOR_ModelVisitor none;
  GDEFSSQL_Datum v = dbytes(GDEFSSQL_DATUM_TEXT, public_id, public_id_len);
  int seen = 0;
  GDEFSTOR_Result rc;
  (void)ctx;
  if (x == NULL || public_id == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  memset(&none, 0, sizeof none);
  rc = visit_drafts(&x->cx, public_id, public_id_len, &none, &seen);
  if (rc == GDEFSTOR_OK && seen == 0) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (rc == GDEFSTOR_OK) {
    Sb sb;
    sb_open(&sb, &x->cx);
    gdefssql_sb_kw(&sb, "DELETE FROM ");
    gdefssql_sb_table(&sb, "dss_draft");
    bare_eq(&sb, " WHERE ", "pid", &v);
    rc = gdefssql_cx_run(&x->cx, &sb);
    gdefssql_sb_free(&sb);
  }
  return rc;
}

/* ---- queries -------------------------------------------------------------- */

static GDEFSTOR_Result tno_of(Cx * c, const GDEFSTOR_Type * t, uint32_t * out) {
  const char * name = NULL;
  size_t len = 0;
  GDEFSTOR_Result rc;
  if (gdefstor_type_name(t, &name, &len) != GDEFSTOR_OK) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = gdefssql_type_number(c, name, len, out);
  if (rc == GDEFSTOR_OK && *out == 0) {
    rc = GDEFSTOR_ERR_INVALID;
  }
  return rc;
}

static GDEFSTOR_Result run_query(Cx * c, const GDEFSTOR_ModelPlan * plan,
    GDEFSTOR_ModelHits * out) {
  const GDEFSTOR_Allocator * alloc =
      plan->alloc != NULL ? plan->alloc : gcu_allocator_default();
  uint32_t * tnos;
  size_t ntnos = 1 + plan->exists_count;
  size_t i;
  Sb total;
  Sb page;
  St s;
  int row = 0;
  int64_t n = 0;
  size_t cap = 0;
  GDEFSTOR_Result rc = GDEFSTOR_OK;
  tnos = (uint32_t *)gdefssql_mem0(c->b->alloc, ntnos * sizeof *tnos);
  if (tnos == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  rc = tno_of(c, plan->type, &tnos[0]);
  for (i = 0; rc == GDEFSTOR_OK && i < plan->exists_count; i++) {
    rc = tno_of(c, plan->exists_types[i], &tnos[i + 1]);
  }
  sb_open(&total, c);
  sb_open(&page, c);
  if (rc == GDEFSTOR_OK) {
    rc = gdefssql_compile_plan(&c->b->d, c->qual, plan, tnos, ntnos, &total,
        &page);
  }
  /* The total, over the same snapshot as the page. */
  if (rc == GDEFSTOR_OK) {
    rc = gdefssql_st_open(c, &total, &s);
    if (rc == GDEFSTOR_OK) {
      rc = gdefssql_st_step(&s, &row);
    }
    if (rc == GDEFSTOR_OK && !row) {
      rc = GDEFSTOR_ERR_CORRUPT;
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefssql_st_int(&s, 0, &n, NULL);
    }
    gdefssql_st_close(&s);
    if (rc == GDEFSTOR_OK && n < 0) {
      rc = GDEFSTOR_ERR_CORRUPT;
    }
    out->total = (uint64_t)n;
  }
  if (rc == GDEFSTOR_OK && !plan->count_only) {
    rc = gdefssql_st_open(c, &page, &s);
    while (rc == GDEFSTOR_OK) {
      int64_t rid = 0;
      const uint8_t * p = NULL;
      size_t len = 0;
      rc = gdefssql_st_step(&s, &row);
      if (rc != GDEFSTOR_OK || !row) {
        break;
      }
      if ((uint64_t)out->count >= plan->limit) {
        /* More rows than the page: the engine ignored the LIMIT. */
        rc = GDEFSTOR_ERR_INTERNAL;
        break;
      }
      rc = gdefssql_st_int(&s, 0, &rid, NULL);
      if (rc == GDEFSTOR_OK) {
        rc = gdefssql_st_bytes(&s, 1, GDEFSSQL_DATUM_TEXT, &p, &len, NULL);
      }
      if (rc == GDEFSTOR_OK && rid <= 0) {
        rc = GDEFSTOR_ERR_CORRUPT;
      }
      if (rc == GDEFSTOR_OK && out->count == cap) {
        size_t grown = cap == 0 ? 16 : cap * 2;
        uint64_t * rows = (uint64_t *)gcu_allocator_realloc(alloc, out->rows,
            grown * sizeof *rows);
        char ** pids;
        size_t * lens;
        if (rows != NULL) {
          out->rows = rows;
        }
        pids = (char **)gcu_allocator_realloc(alloc, out->public_ids,
            grown * sizeof *pids);
        if (pids != NULL) {
          out->public_ids = pids;
        }
        lens = (size_t *)gcu_allocator_realloc(alloc, out->public_id_lens,
            grown * sizeof *lens);
        if (lens != NULL) {
          out->public_id_lens = lens;
        }
        if (rows == NULL || pids == NULL || lens == NULL) {
          rc = GDEFSTOR_ERR_OOM;
        } else {
          cap = grown;
        }
      }
      if (rc == GDEFSTOR_OK) {
        char * pid = dup(alloc, p, len);
        if (pid == NULL) {
          rc = GDEFSTOR_ERR_OOM;
        } else {
          out->rows[out->count] = (uint64_t)rid;
          out->public_ids[out->count] = pid;
          out->public_id_lens[out->count] = len;
          out->count += 1;
        }
      }
    }
    gdefssql_st_close(&s);
  }
  gdefssql_sb_free(&total);
  gdefssql_sb_free(&page);
  gcu_allocator_free(c->b->alloc, tnos);
  return rc;
}

static GDEFSTOR_Result m_query(void * ctx, const char * tenant,
    size_t tenant_len, const GDEFSTOR_ModelPlan * plan,
    GDEFSTOR_ModelHits * out) {
  Back * b = (Back *)ctx;
  Rd r;
  GDEFSTOR_Result rc;
  if (plan == NULL || out == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  memset(out, 0, sizeof *out);
  rc = rd_open(b, NULL, tenant, tenant_len, &r);
  if (rc != GDEFSTOR_OK || r.empty) {
    return rc;
  }
  rc = run_query(r.c, plan, out);
  rc = rd_close(&r, rc);
  if (rc != GDEFSTOR_OK) {
    gdefstor_model_hits_release(plan->alloc != NULL ? plan->alloc
                                                    : gcu_allocator_default(),
        out);
  }
  return rc;
}

/* ---- rebuild and dump ----------------------------------------------------- */

/* Every type number, ascending. */
static GDEFSTOR_Result type_numbers(Cx * c, uint32_t ** out, size_t * out_n) {
  Sb sb;
  St s;
  size_t cap = 0;
  int row = 0;
  GDEFSTOR_Result rc;
  *out = NULL;
  *out_n = 0;
  sb_open(&sb, c);
  gdefssql_sb_kw(&sb, "SELECT ");
  gdefssql_sb_col(&sb, "a0", "tno");
  from(&sb, "dss_type");
  gdefssql_sb_kw(&sb, " ORDER BY ");
  gdefssql_sb_col(&sb, "a0", "tno");
  rc = gdefssql_st_open(c, &sb, &s);
  while (rc == GDEFSTOR_OK) {
    int64_t v = 0;
    rc = gdefssql_st_step(&s, &row);
    if (rc != GDEFSTOR_OK || !row) {
      break;
    }
    rc = gdefssql_st_int(&s, 0, &v, NULL);
    if (rc == GDEFSTOR_OK && (v <= 0 || v > (int64_t)UINT32_MAX)) {
      rc = GDEFSTOR_ERR_CORRUPT;
    }
    if (rc == GDEFSTOR_OK && *out_n == cap) {
      size_t grown = cap == 0 ? 8 : cap * 2;
      uint32_t * next = (uint32_t *)gcu_allocator_realloc(c->b->alloc, *out,
          grown * sizeof *next);
      if (next == NULL) {
        rc = GDEFSTOR_ERR_OOM;
      } else {
        *out = next;
        cap = grown;
      }
    }
    if (rc == GDEFSTOR_OK) {
      (*out)[(*out_n)++] = (uint32_t)v;
    }
  }
  gdefssql_st_close(&s);
  gdefssql_sb_free(&sb);
  if (rc != GDEFSTOR_OK) {
    gcu_allocator_free(c->b->alloc, *out);
    *out = NULL;
    *out_n = 0;
  }
  return rc;
}

static GDEFSTOR_Result delete_all(Cx * c, const char * table) {
  Sb sb;
  GDEFSTOR_Result rc;
  sb_open(&sb, c);
  gdefssql_sb_kw(&sb, "DELETE FROM ");
  gdefssql_sb_table(&sb, table);
  rc = gdefssql_cx_run(c, &sb);
  gdefssql_sb_free(&sb);
  return rc;
}

/* What a rebuild collects of each entity: its row, public id and newest
 * state. */
typedef struct REnt {
  uint64_t rid;
  char * pid;
  size_t pid_len;
  int pub;
  int del;
} REnt;

typedef struct RCollect {
  const GDEFSTOR_Allocator * alloc;
  REnt * v;
  size_t n;
  size_t cap;
  GDEFSTOR_Result rc;
} RCollect;

static GDEFSTOR_Result r_entity(void * ctx, const GDEFSTOR_ModelEntity * e) {
  RCollect * r = (RCollect *)ctx;
  if (r->n == r->cap) {
    size_t grown = r->cap == 0 ? 16 : r->cap * 2;
    REnt * next = (REnt *)gcu_allocator_realloc(r->alloc, r->v,
        grown * sizeof *next);
    if (next == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    r->v = next;
    r->cap = grown;
  }
  memset(&r->v[r->n], 0, sizeof r->v[r->n]);
  r->v[r->n].rid = e->row;
  r->v[r->n].pid = dup(r->alloc, e->public_id, e->public_id_len);
  if (r->v[r->n].pid == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  r->v[r->n].pid_len = e->public_id_len;
  r->n += 1;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result r_state(void * ctx, uint64_t row,
    const GDEFSTOR_ModelState * s) {
  RCollect * r = (RCollect *)ctx;
  size_t lo = 0;
  size_t hi = r->n;
  /* Entities ascend by row, and states arrive by (row, seq): the last one
   * written for a row is its newest. */
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (r->v[mid].rid < row) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo == r->n || r->v[lo].rid != row) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  r->v[lo].pub = s->published != 0;
  r->v[lo].del = s->deleted != 0;
  return GDEFSTOR_OK;
}

static void collect_free(RCollect * r) {
  size_t i;
  for (i = 0; i < r->n; i++) {
    gcu_allocator_free(r->alloc, r->v[i].pid);
  }
  gcu_allocator_free(r->alloc, r->v);
  memset(r, 0, sizeof *r);
}

/* Rebuild tN_cG from tN_gG's open versions, in one statement. */
static GDEFSTOR_Result rebuild_group(Cx * c, uint32_t tno,
    const GDEFSSQL_CatGroup * g) {
  static const char * const fixed[] = {"ent", "item", "vfrom", "parent", "ord"};
  char t[GDEFSSQL_NAME_CAP];
  char col[GDEFSSQL_NAME_CAP];
  Sb sb;
  size_t i;
  int pass;
  GDEFSTOR_Result rc;
  gdefssql_name_table(t, tno, GDEFSSQL_ROLE_CURRENT, g->grp);
  rc = delete_all(c, t);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  sb_open(&sb, c);
  gdefssql_sb_kw(&sb, "INSERT INTO ");
  gdefssql_sb_table(&sb, t);
  gdefssql_sb_kw(&sb, " (");
  for (pass = 0; pass < 2; pass++) {
    if (pass == 1) {
      gdefssql_sb_kw(&sb, ") SELECT ");
    }
    for (i = 0; i < 5; i++) {
      gdefssql_sb_kw(&sb, i == 0 ? "" : ", ");
      if (pass == 0) {
        gdefssql_sb_ident(&sb, fixed[i]);
      } else {
        gdefssql_sb_col(&sb, "a0", fixed[i]);
      }
    }
    for (i = 0; i < g->ncols; i++) {
      int subject;
      for (subject = 0; subject <= g->cols[i].personal; subject++) {
        gdefssql_sb_kw(&sb, ", ");
        gdefssql_name_column(col, g->cols[i].col, subject);
        if (pass == 0) {
          gdefssql_sb_ident(&sb, col);
        } else {
          gdefssql_sb_col(&sb, "a0", col);
        }
      }
    }
  }
  gdefssql_name_table(t, tno, GDEFSSQL_ROLE_GROUP, g->grp);
  from(&sb, t);
  gdefssql_sb_kw(&sb, " WHERE ");
  gdefssql_sb_col(&sb, "a0", "vto");
  gdefssql_sb_kw(&sb, " IS NULL");
  rc = gdefssql_cx_run(c, &sb);
  gdefssql_sb_free(&sb);
  return rc;
}

static GDEFSTOR_Result rebuild_type(Cx * c, uint32_t tno) {
  RCollect col;
  GDEFSTOR_ModelVisitor v;
  GDEFSSQL_Cat cat;
  char t[GDEFSSQL_NAME_CAP];
  size_t i;
  GDEFSTOR_Result rc;
  memset(&col, 0, sizeof col);
  col.alloc = c->b->alloc;
  memset(&v, 0, sizeof v);
  v.ctx = &col;
  v.entity = r_entity;
  v.state = r_state;
  /* What the truth says, read before anything derived is touched: the
   * entity rows (their newest state is not read from tN_n) and states. */
  {
    Sb sb;
    St s;
    int row = 0;
    sb_open(&sb, c);
    gdefssql_sb_kw(&sb, "SELECT ");
    gdefssql_sb_col(&sb, "a0", "rid");
    gdefssql_sb_kw(&sb, ", ");
    gdefssql_sb_col(&sb, "a0", "pid");
    gdefssql_name_table(t, tno, GDEFSSQL_ROLE_ENTITY, 0);
    from(&sb, t);
    gdefssql_sb_kw(&sb, " ORDER BY ");
    gdefssql_sb_col(&sb, "a0", "rid");
    rc = gdefssql_st_open(c, &sb, &s);
    while (rc == GDEFSTOR_OK) {
      GDEFSTOR_ModelEntity e;
      int64_t rid = 0;
      const uint8_t * p = NULL;
      size_t n = 0;
      rc = gdefssql_st_step(&s, &row);
      if (rc != GDEFSTOR_OK || !row) {
        break;
      }
      rc = gdefssql_st_int(&s, 0, &rid, NULL);
      if (rc == GDEFSTOR_OK) {
        rc = gdefssql_st_bytes(&s, 1, GDEFSSQL_DATUM_TEXT, &p, &n, NULL);
      }
      if (rc == GDEFSTOR_OK && rid <= 0) {
        rc = GDEFSTOR_ERR_CORRUPT;
      }
      if (rc == GDEFSTOR_OK) {
        memset(&e, 0, sizeof e);
        e.row = (uint64_t)rid;
        e.public_id = p == NULL ? "" : (const char *)p;
        e.public_id_len = n;
        rc = r_entity(&col, &e);
      }
    }
    gdefssql_st_close(&s);
    gdefssql_sb_free(&sb);
  }
  if (rc == GDEFSTOR_OK) {
    rc = visit_states(c, tno, 0, &v);
  }
  if (rc == GDEFSTOR_OK) {
    gdefssql_name_table(t, tno, GDEFSSQL_ROLE_NOW, 0);
    rc = delete_all(c, t);
  }
  for (i = 0; rc == GDEFSTOR_OK && i < col.n; i++) {
    Kv kv[3];
    kv[0].col = "rid";
    kv[0].v = dint((int64_t)col.v[i].rid);
    kv[1].col = "pub";
    kv[1].v = dint(col.v[i].pub);
    kv[2].col = "del";
    kv[2].v = dint(col.v[i].del);
    gdefssql_name_table(t, tno, GDEFSSQL_ROLE_NOW, 0);
    rc = insert(c, t, kv, 3);
    if (rc == GDEFSTOR_OK) {
      kv[0].col = "pid";
      kv[0].v = dbytes(GDEFSSQL_DATUM_TEXT, col.v[i].pid, col.v[i].pid_len);
      kv[1].col = "rid";
      kv[1].v = dint((int64_t)col.v[i].rid);
      kv[2].col = "tno";
      kv[2].v = dint(tno);
      rc = insert(c, "dss_entity", kv, 3);
    }
  }
  collect_free(&col);
  if (rc == GDEFSTOR_OK) {
    rc = gdefssql_cat_load(c, tno, &cat);
    for (i = 0; rc == GDEFSTOR_OK && i < cat.ngroups; i++) {
      rc = rebuild_group(c, tno, &cat.groups[i]);
    }
    gdefssql_cat_free(&cat);
  }
  return rc;
}

static GDEFSTOR_Result m_rebuild(void * ctx, void * txn) {
  Txn * x = (Txn *)txn;
  uint32_t * tnos = NULL;
  size_t n = 0;
  size_t i;
  GDEFSTOR_Result rc;
  (void)ctx;
  if (x == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = type_numbers(&x->cx, &tnos, &n);
  if (rc == GDEFSTOR_OK) {
    rc = delete_all(&x->cx, "dss_entity");
  }
  for (i = 0; rc == GDEFSTOR_OK && i < n; i++) {
    rc = rebuild_type(&x->cx, tnos[i]);
  }
  gcu_allocator_free(x->b->alloc, tnos);
  return rc;
}

static GDEFSTOR_Result dump_types(Cx * c, const GDEFSTOR_ModelVisitor * v) {
  Sb sb;
  St s;
  int row = 0;
  GDEFSTOR_Result rc;
  if (v->type == NULL) {
    return GDEFSTOR_OK;
  }
  sb_open(&sb, c);
  gdefssql_sb_kw(&sb, "SELECT ");
  gdefssql_sb_col(&sb, "a0", "name");
  gdefssql_sb_kw(&sb, ", ");
  gdefssql_sb_col(&sb, "a0", "decl");
  from(&sb, "dss_type");
  gdefssql_sb_kw(&sb, " ORDER BY ");
  gdefssql_sb_col(&sb, "a0", "tno");
  rc = gdefssql_st_open(c, &sb, &s);
  while (rc == GDEFSTOR_OK) {
    const uint8_t * name = NULL;
    size_t name_len = 0;
    const uint8_t * text = NULL;
    size_t text_len = 0;
    rc = gdefssql_st_step(&s, &row);
    if (rc != GDEFSTOR_OK || !row) {
      break;
    }
    rc = gdefssql_st_bytes(&s, 0, GDEFSSQL_DATUM_TEXT, &name, &name_len, NULL);
    if (rc == GDEFSTOR_OK) {
      rc = gdefssql_st_bytes(&s, 1, GDEFSSQL_DATUM_BYTES, &text, &text_len,
          NULL);
    }
    if (rc == GDEFSTOR_OK) {
      rc = v->type(v->ctx, name == NULL ? "" : (const char *)name, name_len,
          text == NULL ? (const uint8_t *)"" : text, text_len);
    }
  }
  gdefssql_st_close(&s);
  gdefssql_sb_free(&sb);
  return rc;
}

static GDEFSTOR_Result m_dump(void * ctx, const char * tenant,
    size_t tenant_len, int which, const GDEFSTOR_ModelVisitor * visitor) {
  Back * b = (Back *)ctx;
  Rd r;
  uint32_t * tnos = NULL;
  size_t n = 0;
  size_t i;
  int truth = which == GDEFSTOR_MODEL_TRUTH;
  GDEFSTOR_Result rc;
  if (visitor == NULL ||
      (which != GDEFSTOR_MODEL_TRUTH && which != GDEFSTOR_MODEL_CURRENT)) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = rd_open(b, NULL, tenant, tenant_len, &r);
  if (rc != GDEFSTOR_OK || r.empty) {
    return rc;
  }
  if (truth) {
    int seen = 0;
    rc = dump_types(r.c, visitor);
    if (rc == GDEFSTOR_OK && visitor->draft != NULL) {
      rc = visit_drafts(r.c, NULL, 0, visitor, &seen);
    }
  }
  if (rc == GDEFSTOR_OK) {
    rc = type_numbers(r.c, &tnos, &n);
  }
  for (i = 0; rc == GDEFSTOR_OK && i < n; i++) {
    rc = visit_entities(r.c, tnos[i], 0, visitor);
    if (rc == GDEFSTOR_OK && truth) {
      rc = visit_revisions(r.c, tnos[i], 0, visitor);
    }
    if (rc == GDEFSTOR_OK && truth) {
      rc = visit_states(r.c, tnos[i], 0, visitor);
    }
    if (rc == GDEFSTOR_OK) {
      rc = visit_all_items(r.c, tnos[i], truth, 0, 0, visitor);
    }
  }
  gcu_allocator_free(b->alloc, tnos);
  return rd_close(&r, rc);
}

static void m_destroy(void * ctx) {
  Back * b = (Back *)ctx;
  if (b == NULL) {
    return;
  }
  while (b->open != NULL) {
    Txn * x = b->open;
    txn_sql(&x->cx, b->d.rollback);
    b->x.close(b->x.ctx, x->cx.conn, 1);
    txn_free(b, x);
  }
  while (b->ready != NULL) {
    Ready * r = b->ready;
    b->ready = r->next;
    gcu_allocator_free(b->alloc, r->tenant);
    gcu_allocator_free(b->alloc, r);
  }
  if (b->x.destroy != NULL) {
    b->x.destroy(b->x.ctx);
  }
  gcu_allocator_free(b->alloc, b);
}

GDEFSSQL_Result gdefssql_model_backend(const GDEFSSQL_Dialect * dialect,
    const GDEFSSQL_Executor * executor, const GDEFSSQL_Allocator * allocator,
    GDEFSTOR_ModelBackend * out) {
  const GDEFSTOR_Allocator * alloc =
      allocator != NULL ? allocator : gcu_allocator_default();
  GDEFSSQL_Result check;
  Back * b;
  if (executor == NULL || out == NULL) {
    return GDEFSSQL_ERR_INVALID;
  }
  check = gdefssql_dialect_check(dialect);
  if (check != GDEFSSQL_OK) {
    return check;
  }
  if (executor->size != sizeof *executor || executor->open == NULL ||
      executor->close == NULL || executor->exec == NULL ||
      executor->prepare == NULL || executor->bind == NULL ||
      executor->step == NULL || executor->column == NULL ||
      executor->finalize == NULL) {
    return GDEFSSQL_ERR_INVALID;
  }
  b = (Back *)gdefssql_mem0(alloc, sizeof *b);
  if (b == NULL) {
    return GDEFSSQL_ERR_OOM;
  }
  b->alloc = alloc;
  b->d = *dialect;
  b->x = *executor;
  memset(out, 0, sizeof *out);
  out->size = sizeof *out;
  out->ctx = b;
  out->destroy = m_destroy;
  out->begin = m_begin;
  out->commit = m_commit;
  out->abort = m_abort;
  out->type_load = m_type_load;
  out->type_store = m_type_store;
  out->entity_find = m_entity_find;
  out->entity_insert = m_entity_insert;
  out->state_append = m_state_append;
  out->apply = m_apply;
  out->history = m_history;
  out->items = m_items;
  out->draft_put = m_draft_put;
  out->draft_get = m_draft_get;
  out->draft_delete = m_draft_delete;
  out->query = m_query;
  out->rebuild = m_rebuild;
  out->dump = m_dump;
  return GDEFSSQL_OK;
}
