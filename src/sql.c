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
 * Generated names, the statement builder, values and the dialect check.
 */

#include <ghoti.io/defiant-storage-sql/macros.h>
#include "sql_internal.h"

#include <ghoti.io/cutil/allocator.h>
#include <stdio.h>
#include <string.h>

/* ---- generated names ------------------------------------------------------ */

int gdefssql_name_ok(const char * name, size_t cap) {
  size_t i;
  if (name == NULL || name[0] < 'a' || name[0] > 'z') {
    return 0;
  }
  for (i = 0; name[i] != '\0'; i++) {
    char c = name[i];
    if (i >= cap || !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                         c == '_')) {
      return 0;
    }
  }
  return 1;
}

void gdefssql_name_table(char out[GDEFSSQL_NAME_CAP], uint32_t tno, int role,
    uint32_t group) {
  switch (role) {
  case GDEFSSQL_ROLE_ENTITY:
    snprintf(out, GDEFSSQL_NAME_CAP, "t%lu_e", (unsigned long)tno);
    return;
  case GDEFSSQL_ROLE_STATE:
    snprintf(out, GDEFSSQL_NAME_CAP, "t%lu_s", (unsigned long)tno);
    return;
  case GDEFSSQL_ROLE_REVISION:
    snprintf(out, GDEFSSQL_NAME_CAP, "t%lu_r", (unsigned long)tno);
    return;
  case GDEFSSQL_ROLE_NOW:
    snprintf(out, GDEFSSQL_NAME_CAP, "t%lu_n", (unsigned long)tno);
    return;
  case GDEFSSQL_ROLE_GROUP:
    snprintf(out, GDEFSSQL_NAME_CAP, "t%lu_g%lu", (unsigned long)tno,
        (unsigned long)group);
    return;
  case GDEFSSQL_ROLE_CURRENT:
    snprintf(out, GDEFSSQL_NAME_CAP, "t%lu_c%lu", (unsigned long)tno,
        (unsigned long)group);
    return;
  case GDEFSSQL_ROLE_BLIND:
    snprintf(out, GDEFSSQL_NAME_CAP, "t%lu_b", (unsigned long)tno);
    return;
  default:
    /* Not a table: an empty name, which the builder refuses. */
    out[0] = '\0';
    return;
  }
}

void gdefssql_name_column(char out[GDEFSSQL_NAME_CAP], uint32_t col,
    int subject) {
  snprintf(out, GDEFSSQL_NAME_CAP, subject ? "v%lus" : "v%lu",
      (unsigned long)col);
}

void gdefssql_name_index(char out[GDEFSSQL_NAME_CAP], uint32_t tno, int role,
    uint32_t group, uint32_t col) {
  if (role == GDEFSSQL_ROLE_BLIND_INDEX || role == GDEFSSQL_ROLE_BLIND_SUBJECT) {
    snprintf(out, GDEFSSQL_NAME_CAP,
        role == GDEFSSQL_ROLE_BLIND_INDEX ? "t%lu_bm" : "t%lu_bs",
        (unsigned long)tno);
    return;
  }
  snprintf(out, GDEFSSQL_NAME_CAP,
      role == GDEFSSQL_ROLE_SUBJECT_INDEX ? "t%lu_j%lu_%lu" : "t%lu_i%lu_%lu",
      (unsigned long)tno, (unsigned long)group, (unsigned long)col);
}

void gdefssql_name_alias(char out[GDEFSSQL_NAME_CAP], unsigned n) {
  snprintf(out, GDEFSSQL_NAME_CAP, "a%u", n);
}

/* ---- the builder ---------------------------------------------------------- */

void * gdefssql_mem0(const GDEFSTOR_Allocator * alloc, size_t n) {
  void * p = gcu_allocator_malloc(alloc, n == 0 ? 1 : n);
  if (p != NULL) {
    memset(p, 0, n == 0 ? 1 : n);
  }
  return p;
}

void gdefssql_sb_init(GDEFSSQL_Sb * b, const GDEFSTOR_Allocator * alloc,
    const GDEFSSQL_Dialect * d, const char * qual) {
  memset(b, 0, sizeof *b);
  b->alloc = alloc;
  b->d = d;
  b->qual = (qual != NULL && qual[0] != '\0') ? qual : NULL;
}

void gdefssql_sb_free(GDEFSSQL_Sb * b) {
  gcu_allocator_free(b->alloc, b->text);
  gcu_allocator_free(b->alloc, b->binds);
  b->text = NULL;
  b->binds = NULL;
  b->len = b->cap = b->nbinds = b->capbinds = 0;
}

void gdefssql_sb_reset(GDEFSSQL_Sb * b) {
  b->len = 0;
  b->nbinds = 0;
  if (b->text != NULL) {
    b->text[0] = '\0';
  }
}

static void put(GDEFSSQL_Sb * b, const char * s, size_t n) {
  if (b->rc != GDEFSTOR_OK) {
    return;
  }
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap == 0 ? 256 : b->cap;
    char * next;
    while (cap < b->len + n + 1) {
      cap *= 2;
    }
    next = (char *)gcu_allocator_realloc(b->alloc, b->text, cap);
    if (next == NULL) {
      b->rc = GDEFSTOR_ERR_OOM;
      return;
    }
    b->text = next;
    b->cap = cap;
  }
  memcpy(b->text + b->len, s, n);
  b->len += n;
  b->text[b->len] = '\0';
}

void gdefssql_sb_kw(GDEFSSQL_Sb * b, const char * kw) {
  put(b, kw, strlen(kw));
}

void gdefssql_sb_ident(GDEFSSQL_Sb * b, const char * name) {
  if (b->rc != GDEFSTOR_OK) {
    return;
  }
  /* The one gate on identifiers: only a generated name may be quoted into
   * the text, so no caller byte can be. */
  if (!gdefssql_name_ok(name, b->d->max_identifier)) {
    b->rc = GDEFSTOR_ERR_INTERNAL;
    return;
  }
  gdefssql_sb_kw(b, b->d->quote_open);
  gdefssql_sb_kw(b, name);
  gdefssql_sb_kw(b, b->d->quote_close);
}

void gdefssql_sb_table(GDEFSSQL_Sb * b, const char * name) {
  if (b->qual != NULL) {
    gdefssql_sb_ident(b, b->qual);
    gdefssql_sb_kw(b, ".");
  }
  gdefssql_sb_ident(b, name);
}

void gdefssql_sb_col(GDEFSSQL_Sb * b, const char * alias, const char * col) {
  gdefssql_sb_ident(b, alias);
  gdefssql_sb_kw(b, ".");
  gdefssql_sb_ident(b, col);
}

void gdefssql_sb_param(GDEFSSQL_Sb * b, const GDEFSSQL_Datum * v) {
  if (b->rc != GDEFSTOR_OK) {
    return;
  }
  if (v->kind == GDEFSSQL_DATUM_NULL) {
    /* A NULL is never bound: a column left out of an INSERT is NULL. */
    b->rc = GDEFSTOR_ERR_INTERNAL;
    return;
  }
  if (b->nbinds == b->capbinds) {
    size_t cap = b->capbinds == 0 ? 16 : b->capbinds * 2;
    GDEFSSQL_Datum * next = (GDEFSSQL_Datum *)gcu_allocator_realloc(b->alloc,
        b->binds, cap * sizeof *next);
    if (next == NULL) {
      b->rc = GDEFSTOR_ERR_OOM;
      return;
    }
    b->binds = next;
    b->capbinds = cap;
  }
  b->binds[b->nbinds] = *v;
  b->nbinds += 1;
  gdefssql_sb_kw(b, b->d->placeholder);
  if (b->d->placeholder_numbered) {
    char tmp[24];
    snprintf(tmp, sizeof tmp, "%lu", (unsigned long)b->nbinds);
    gdefssql_sb_kw(b, tmp);
  }
}

void gdefssql_sb_int(GDEFSSQL_Sb * b, int64_t v) {
  GDEFSSQL_Datum d;
  memset(&d, 0, sizeof d);
  d.kind = GDEFSSQL_DATUM_INT;
  d.integer = v;
  gdefssql_sb_param(b, &d);
}

void gdefssql_sb_text(GDEFSSQL_Sb * b, const void * bytes, size_t len) {
  GDEFSSQL_Datum d;
  memset(&d, 0, sizeof d);
  d.kind = GDEFSSQL_DATUM_TEXT;
  d.bytes = (const uint8_t *)bytes;
  d.len = len;
  gdefssql_sb_param(b, &d);
}

void gdefssql_sb_type(GDEFSSQL_Sb * b, int coltype) {
  if (coltype < 0 || coltype >= GDEFSSQL_COL_COUNT) {
    if (b->rc == GDEFSTOR_OK) {
      b->rc = GDEFSTOR_ERR_INTERNAL;
    }
    return;
  }
  gdefssql_sb_kw(b, b->d->type_names[coltype]);
  if ((coltype == GDEFSSQL_COL_TEXT || coltype == GDEFSSQL_COL_REFERENCE) &&
      b->d->text_collation != NULL && b->d->text_collation[0] != '\0') {
    gdefssql_sb_kw(b, " ");
    gdefssql_sb_kw(b, b->d->text_collation);
  }
}

/* ---- values --------------------------------------------------------------- */

int gdefssql_value_coltype(int field_type, int personal) {
  if (personal) {
    return GDEFSSQL_COL_BYTES;
  }
  switch (field_type) {
  case GDEFSTOR_FIELD_TEXT:
    return GDEFSSQL_COL_TEXT;
  case GDEFSTOR_FIELD_INTEGER:
    return GDEFSSQL_COL_INTEGER;
  case GDEFSTOR_FIELD_DECIMAL:
    return GDEFSSQL_COL_DECIMAL;
  case GDEFSTOR_FIELD_FLOAT:
    return GDEFSSQL_COL_FLOAT;
  case GDEFSTOR_FIELD_BOOLEAN:
    return GDEFSSQL_COL_BOOLEAN;
  case GDEFSTOR_FIELD_DATE:
    return GDEFSSQL_COL_DATE;
  case GDEFSTOR_FIELD_DATETIME:
    return GDEFSSQL_COL_DATETIME;
  case GDEFSTOR_FIELD_REFERENCE:
    return GDEFSSQL_COL_REFERENCE;
  default:
    return -1;
  }
}

void gdefssql_value_datum(const GDEFSTOR_Value * v, GDEFSSQL_Datum * out) {
  memset(out, 0, sizeof *out);
  switch (v->type) {
  case GDEFSTOR_FIELD_TEXT:
  case GDEFSTOR_FIELD_REFERENCE:
    out->kind = GDEFSSQL_DATUM_TEXT;
    out->bytes = v->bytes;
    out->len = v->len;
    return;
  case GDEFSTOR_FIELD_FLOAT:
    out->kind = GDEFSSQL_DATUM_REAL;
    /* -0.0 and 0.0 are one value to the port; store the one it keeps. */
    out->real = v->real == 0.0 ? 0.0 : v->real;
    return;
  case GDEFSTOR_FIELD_INTEGER:
  case GDEFSTOR_FIELD_DECIMAL:
  case GDEFSTOR_FIELD_BOOLEAN:
  case GDEFSTOR_FIELD_DATE:
  case GDEFSTOR_FIELD_DATETIME:
    out->kind = GDEFSSQL_DATUM_INT;
    out->integer = v->integer;
    return;
  default:
    out->kind = GDEFSSQL_DATUM_NULL;
    return;
  }
}

static int bytes_cmp(const uint8_t * a, size_t an, const uint8_t * b,
    size_t bn) {
  size_t n = an < bn ? an : bn;
  int c = n == 0 ? 0 : memcmp(a, b, n);
  if (c != 0) {
    return c < 0 ? -1 : 1;
  }
  return an < bn ? -1 : (an > bn ? 1 : 0);
}

int gdefssql_value_compare(const GDEFSTOR_Value * a, const GDEFSTOR_Value * b) {
  if (a->type == 0 || b->type == 0) {
    return (a->type != 0) - (b->type != 0);
  }
  switch (a->type) {
  case GDEFSTOR_FIELD_TEXT:
  case GDEFSTOR_FIELD_REFERENCE:
    return bytes_cmp(a->bytes, a->len, b->bytes, b->len);
  case GDEFSTOR_FIELD_FLOAT:
    return a->real < b->real ? -1 : (a->real > b->real ? 1 : 0);
  default:
    return a->integer < b->integer ? -1 : (a->integer > b->integer ? 1 : 0);
  }
}

/* ---- the dialect ---------------------------------------------------------- */

static int filled(const char * s) {
  return s != NULL && s[0] != '\0';
}

/* No byte of @p s could continue or start a generated name, and none is a
 * quote that could end a string: the text around it cannot be misread. */
static int inert(const char * s) {
  size_t i;
  for (i = 0; s[i] != '\0'; i++) {
    char c = s[i];
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
        c == '\'') {
      return 0;
    }
  }
  return 1;
}

GDEFSSQL_Result gdefssql_dialect_check(const GDEFSSQL_Dialect * d) {
  size_t i;
  if (d == NULL || d->size != sizeof *d || !filled(d->name) ||
      !filled(d->quote_open) || !filled(d->quote_close) ||
      !filled(d->placeholder) || !inert(d->quote_open) ||
      !inert(d->quote_close) || !inert(d->placeholder) ||
      strchr(d->placeholder, '"') != NULL ||
      strcmp(d->placeholder, d->quote_open) == 0 ||
      !filled(d->begin_read) || !filled(d->begin_write) ||
      !filled(d->commit) || !filled(d->rollback) ||
      (d->upsert != GDEFSSQL_UPSERT_ON_CONFLICT &&
          d->upsert != GDEFSSQL_UPSERT_ON_DUPLICATE_KEY) ||
      d->max_identifier < 32 || d->max_binds < 999 || d->max_columns < 16) {
    return GDEFSSQL_ERR_INVALID;
  }
  for (i = 0; i < GDEFSSQL_COL_COUNT; i++) {
    if (!filled(d->type_names[i])) {
      return GDEFSSQL_ERR_INVALID;
    }
  }
  /* The optional members: absent, or a string of the dialect's own. The
   * build word sits between generated words, so it must not run into one. */
  if ((d->index_valid != NULL && !filled(d->index_valid)) ||
      (d->index_drop != NULL && !filled(d->index_drop)) ||
      (d->qualifier_create != NULL && !filled(d->qualifier_create)) ||
      (d->store_exists != NULL && !filled(d->store_exists)) ||
      (d->index_build != NULL && !inert(d->index_build)) ||
      (d->index_valid != NULL && d->index_drop == NULL)) {
    return GDEFSSQL_ERR_INVALID;
  }
  if (!d->ddl_transactional) {
    return GDEFSSQL_ERR_UNSUPPORTED;
  }
  return GDEFSSQL_OK;
}
