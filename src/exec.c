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
 * Running statements through the executor, the counters, and a type's
 * catalog.
 */

#include <ghoti.io/defiant-storage-sql/macros.h>
#include "backend_internal.h"

#include <ghoti.io/cutil/allocator.h>
#include <string.h>

/* An executor's error marks the connection, whatever it was. */
static GDEFSTOR_Result mark(GDEFSSQL_Cx * c, GDEFSTOR_Result rc) {
  if (rc != GDEFSTOR_OK) {
    c->failed = 1;
  }
  return rc;
}

GDEFSTOR_Result gdefssql_cx_exec(GDEFSSQL_Cx * c, const GDEFSSQL_Sb * sb) {
  if (sb->rc != GDEFSTOR_OK) {
    return sb->rc;
  }
  if (sb->nbinds != 0) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  return mark(c, c->b->x.exec(c->b->x.ctx, c->conn, sb->text, sb->len));
}

GDEFSTOR_Result gdefssql_st_open(GDEFSSQL_Cx * c, const GDEFSSQL_Sb * sb,
    GDEFSSQL_St * out) {
  GDEFSTOR_Result rc;
  size_t i;
  out->c = c;
  out->stmt = NULL;
  if (sb->rc != GDEFSTOR_OK) {
    return sb->rc;
  }
  if (sb->nbinds > c->b->d.max_binds) {
    return GDEFSTOR_ERR_LIMIT;
  }
  rc = mark(c, c->b->x.prepare(c->b->x.ctx, c->conn, sb->text, sb->len,
      &out->stmt));
  if (rc != GDEFSTOR_OK) {
    out->stmt = NULL;
    return rc;
  }
  if (out->stmt == NULL) {
    return mark(c, GDEFSTOR_ERR_INTERNAL);
  }
  for (i = 0; i < sb->nbinds; i++) {
    rc = mark(c, c->b->x.bind(c->b->x.ctx, out->stmt, (unsigned)(i + 1),
        &sb->binds[i]));
    if (rc != GDEFSTOR_OK) {
      gdefssql_st_close(out);
      return rc;
    }
  }
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefssql_st_step(GDEFSSQL_St * s, int * out_row) {
  *out_row = 0;
  return mark(s->c, s->c->b->x.step(s->c->b->x.ctx, s->stmt, out_row));
}

static GDEFSTOR_Result column(GDEFSSQL_St * s, unsigned col, int kind,
    GDEFSSQL_Datum * d, int * out_null) {
  GDEFSTOR_Result rc;
  memset(d, 0, sizeof *d);
  rc = mark(s->c, s->c->b->x.column(s->c->b->x.ctx, s->stmt, col, kind, d));
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (d->kind == GDEFSSQL_DATUM_NULL) {
    if (out_null == NULL) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    *out_null = 1;
    return GDEFSTOR_OK;
  }
  if (d->kind != kind) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  if (out_null != NULL) {
    *out_null = 0;
  }
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefssql_st_int(GDEFSSQL_St * s, unsigned col, int64_t * out,
    int * out_null) {
  GDEFSSQL_Datum d;
  GDEFSTOR_Result rc = column(s, col, GDEFSSQL_DATUM_INT, &d, out_null);
  *out = rc == GDEFSTOR_OK ? d.integer : 0;
  return rc;
}

GDEFSTOR_Result gdefssql_st_real(GDEFSSQL_St * s, unsigned col, double * out,
    int * out_null) {
  GDEFSSQL_Datum d;
  GDEFSTOR_Result rc = column(s, col, GDEFSSQL_DATUM_REAL, &d, out_null);
  *out = rc == GDEFSTOR_OK ? d.real : 0.0;
  return rc;
}

GDEFSTOR_Result gdefssql_st_bytes(GDEFSSQL_St * s, unsigned col, int kind,
    const uint8_t ** out, size_t * out_len, int * out_null) {
  GDEFSSQL_Datum d;
  GDEFSTOR_Result rc = column(s, col, kind, &d, out_null);
  *out = NULL;
  *out_len = 0;
  if (rc == GDEFSTOR_OK) {
    if (d.bytes == NULL && d.len != 0) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    *out = d.bytes;
    *out_len = d.len;
  }
  return rc;
}

void gdefssql_st_close(GDEFSSQL_St * s) {
  if (s->stmt != NULL) {
    s->c->b->x.finalize(s->c->b->x.ctx, s->stmt);
    s->stmt = NULL;
  }
}

GDEFSTOR_Result gdefssql_cx_run(GDEFSSQL_Cx * c, const GDEFSSQL_Sb * sb) {
  GDEFSSQL_St s;
  int row = 1;
  GDEFSTOR_Result rc = gdefssql_st_open(c, sb, &s);
  while (rc == GDEFSTOR_OK && row) {
    rc = gdefssql_st_step(&s, &row);
  }
  gdefssql_st_close(&s);
  return rc;
}

/* ---- counters ------------------------------------------------------------- */

GDEFSTOR_Result gdefssql_cx_counter(GDEFSSQL_Cx * c, int which,
    uint64_t * out) {
  const char * col = which == GDEFSSQL_COUNTER_ROW ? "last_row"
                     : which == GDEFSSQL_COUNTER_TYPE ? "last_type"
                                                      : "last_cseq";
  GDEFSSQL_Sb sb;
  GDEFSSQL_St s;
  GDEFSTOR_Result rc;
  int row = 0;
  int64_t v = 0;
  gdefssql_sb_init(&sb, c->b->alloc, &c->b->d, c->qual);
  if (c->b->d.returning) {
    gdefssql_sb_kw(&sb, "UPDATE ");
    gdefssql_sb_table(&sb, "dss_counter");
    gdefssql_sb_kw(&sb, " SET ");
    gdefssql_sb_ident(&sb, col);
    gdefssql_sb_kw(&sb, " = ");
    gdefssql_sb_ident(&sb, col);
    gdefssql_sb_kw(&sb, " + ");
    gdefssql_sb_int(&sb, 1);
    gdefssql_sb_kw(&sb, " WHERE ");
    gdefssql_sb_ident(&sb, "id");
    gdefssql_sb_kw(&sb, " = ");
    gdefssql_sb_int(&sb, 1);
    gdefssql_sb_kw(&sb, " RETURNING ");
    gdefssql_sb_ident(&sb, col);
    rc = gdefssql_st_open(c, &sb, &s);
    if (rc == GDEFSTOR_OK) {
      rc = gdefssql_st_step(&s, &row);
    }
    if (rc == GDEFSTOR_OK && !row) {
      rc = GDEFSTOR_ERR_CORRUPT;
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefssql_st_int(&s, 0, &v, NULL);
    }
    /* Drain it: a RETURNING statement has finished its write only then. */
    while (rc == GDEFSTOR_OK && row) {
      rc = gdefssql_st_step(&s, &row);
    }
    gdefssql_st_close(&s);
  } else {
    gdefssql_sb_kw(&sb, "SELECT ");
    gdefssql_sb_col(&sb, "a0", col);
    gdefssql_sb_kw(&sb, " FROM ");
    gdefssql_sb_table(&sb, "dss_counter");
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
    if (rc == GDEFSTOR_OK && !row) {
      rc = GDEFSTOR_ERR_CORRUPT;
    }
    if (rc == GDEFSTOR_OK) {
      rc = gdefssql_st_int(&s, 0, &v, NULL);
    }
    gdefssql_st_close(&s);
    if (rc == GDEFSTOR_OK && v < 0) {
      rc = GDEFSTOR_ERR_CORRUPT;
    }
    if (rc == GDEFSTOR_OK) {
      v += 1;
      gdefssql_sb_reset(&sb);
      gdefssql_sb_kw(&sb, "UPDATE ");
      gdefssql_sb_table(&sb, "dss_counter");
      gdefssql_sb_kw(&sb, " SET ");
      gdefssql_sb_ident(&sb, col);
      gdefssql_sb_kw(&sb, " = ");
      gdefssql_sb_int(&sb, v);
      gdefssql_sb_kw(&sb, " WHERE ");
      gdefssql_sb_ident(&sb, "id");
      gdefssql_sb_kw(&sb, " = ");
      gdefssql_sb_int(&sb, 1);
      rc = gdefssql_cx_run(c, &sb);
    }
  }
  gdefssql_sb_free(&sb);
  if (rc == GDEFSTOR_OK && v <= 0) {
    rc = GDEFSTOR_ERR_CORRUPT;
  }
  if (rc == GDEFSTOR_OK) {
    *out = (uint64_t)v;
  }
  return rc;
}

/* ---- the catalog ---------------------------------------------------------- */

void gdefssql_cat_free(GDEFSSQL_Cat * cat) {
  size_t i;
  for (i = 0; i < cat->ngroups; i++) {
    gcu_allocator_free(cat->alloc, cat->groups[i].cols);
  }
  gcu_allocator_free(cat->alloc, cat->groups);
  gcu_allocator_free(cat->alloc, cat->rows);
  cat->groups = NULL;
  cat->rows = NULL;
  cat->ngroups = 0;
  cat->nrows = 0;
}

const GDEFSSQL_CatGroup * gdefssql_cat_group(const GDEFSSQL_Cat * cat,
    uint32_t grp) {
  size_t i;
  for (i = 0; i < cat->ngroups; i++) {
    if (cat->groups[i].grp == grp) {
      return &cat->groups[i];
    }
  }
  return NULL;
}

static GDEFSTOR_Result cat_index(GDEFSSQL_Cat * cat) {
  size_t i;
  size_t k;
  for (i = 0; i < cat->nrows; i++) {
    if (cat->rows[i].role == GDEFSSQL_ROLE_GROUP) {
      cat->ngroups += 1;
    }
  }
  if (cat->ngroups == 0) {
    return GDEFSTOR_OK;
  }
  cat->groups = (GDEFSSQL_CatGroup *)gdefssql_mem0(cat->alloc,
      cat->ngroups * sizeof *cat->groups);
  if (cat->groups == NULL) {
    cat->ngroups = 0;
    return GDEFSTOR_ERR_OOM;
  }
  k = 0;
  /* Rows arrive ordered by (grp, col, role), so groups ascend. */
  for (i = 0; i < cat->nrows; i++) {
    if (cat->rows[i].role == GDEFSSQL_ROLE_GROUP) {
      cat->groups[k].grp = cat->rows[i].grp;
      k += 1;
    }
  }
  for (k = 0; k < cat->ngroups; k++) {
    GDEFSSQL_CatGroup * g = &cat->groups[k];
    size_t n = 0;
    for (i = 0; i < cat->nrows; i++) {
      if (cat->rows[i].role == GDEFSSQL_ROLE_VALUE &&
          cat->rows[i].grp == g->grp) {
        n += 1;
      }
    }
    if (n == 0) {
      continue;
    }
    g->cols = (GDEFSSQL_CatCol *)gdefssql_mem0(cat->alloc, n * sizeof *g->cols);
    if (g->cols == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    for (i = 0; i < cat->nrows; i++) {
      const GDEFSSQL_CatRow * r = &cat->rows[i];
      if (r->role == GDEFSSQL_ROLE_VALUE && r->grp == g->grp) {
        GDEFSSQL_CatCol * col = &g->cols[g->ncols];
        col->col = r->col;
        col->type = r->kind & 0xff;
        col->personal = (r->kind & GDEFSSQL_KIND_PERSONAL) != 0;
        if (gdefssql_value_coltype(col->type, col->personal) < 0) {
          return GDEFSTOR_ERR_CORRUPT;
        }
        g->ncols += 1;
      }
    }
  }
  return GDEFSTOR_OK;
}

GDEFSTOR_Result gdefssql_cat_load(GDEFSSQL_Cx * c, uint32_t tno,
    GDEFSSQL_Cat * out) {
  GDEFSSQL_Sb sb;
  GDEFSSQL_St s;
  GDEFSTOR_Result rc;
  size_t cap = 0;
  int row = 0;
  memset(out, 0, sizeof *out);
  out->alloc = c->b->alloc;
  out->tno = tno;
  gdefssql_sb_init(&sb, c->b->alloc, &c->b->d, c->qual);
  gdefssql_sb_kw(&sb, "SELECT ");
  gdefssql_sb_col(&sb, "a0", "grp");
  gdefssql_sb_kw(&sb, ", ");
  gdefssql_sb_col(&sb, "a0", "col");
  gdefssql_sb_kw(&sb, ", ");
  gdefssql_sb_col(&sb, "a0", "role");
  gdefssql_sb_kw(&sb, ", ");
  gdefssql_sb_col(&sb, "a0", "kind");
  gdefssql_sb_kw(&sb, " FROM ");
  gdefssql_sb_table(&sb, "dss_name");
  gdefssql_sb_kw(&sb, " ");
  gdefssql_sb_ident(&sb, "a0");
  gdefssql_sb_kw(&sb, " WHERE ");
  gdefssql_sb_col(&sb, "a0", "tno");
  gdefssql_sb_kw(&sb, " = ");
  gdefssql_sb_int(&sb, tno);
  gdefssql_sb_kw(&sb, " ORDER BY ");
  gdefssql_sb_col(&sb, "a0", "grp");
  gdefssql_sb_kw(&sb, ", ");
  gdefssql_sb_col(&sb, "a0", "col");
  gdefssql_sb_kw(&sb, ", ");
  gdefssql_sb_col(&sb, "a0", "role");
  rc = gdefssql_st_open(c, &sb, &s);
  while (rc == GDEFSTOR_OK) {
    int64_t v[4];
    unsigned k;
    rc = gdefssql_st_step(&s, &row);
    if (rc != GDEFSTOR_OK || !row) {
      break;
    }
    for (k = 0; rc == GDEFSTOR_OK && k < 4; k++) {
      rc = gdefssql_st_int(&s, k, &v[k], NULL);
    }
    if (rc != GDEFSTOR_OK) {
      break;
    }
    if (v[0] < 0 || v[0] > (int64_t)UINT32_MAX || v[1] < 0 ||
        v[1] > (int64_t)UINT32_MAX || v[2] < GDEFSSQL_ROLE_ENTITY ||
        v[2] > GDEFSSQL_ROLE_BLIND_SUBJECT || v[3] < 0 || v[3] > 0xffff) {
      rc = GDEFSTOR_ERR_CORRUPT;
      break;
    }
    if (out->nrows == cap) {
      size_t grown = cap == 0 ? 32 : cap * 2;
      GDEFSSQL_CatRow * next = (GDEFSSQL_CatRow *)gcu_allocator_realloc(
          out->alloc, out->rows, grown * sizeof *next);
      if (next == NULL) {
        rc = GDEFSTOR_ERR_OOM;
        break;
      }
      out->rows = next;
      cap = grown;
    }
    out->rows[out->nrows].grp = (uint32_t)v[0];
    out->rows[out->nrows].col = (uint32_t)v[1];
    out->rows[out->nrows].role = (int)v[2];
    out->rows[out->nrows].kind = (int)v[3];
    out->nrows += 1;
  }
  gdefssql_st_close(&s);
  gdefssql_sb_free(&sb);
  if (rc == GDEFSTOR_OK) {
    rc = cat_index(out);
  }
  if (rc != GDEFSTOR_OK) {
    gdefssql_cat_free(out);
  }
  return rc;
}
