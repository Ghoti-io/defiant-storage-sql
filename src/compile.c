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
 * The query compiler (compile.h): the port's query form over entities,
 * written as SQL that means exactly what gdefstor_model_scan() answers.
 *
 * Two properties carry the whole correctness argument, and each comparison
 * form below keeps both:
 *
 * - **Some pair.** A comparison holds when some value of the left set and
 *   some value of the right stand in the relation. A FIELD's set is the
 *   values of its column over the entity's current items; an empty set (no
 *   item, or every item without a value) makes every comparison false.
 * - **Two-valued.** Every comparison is TRUE or FALSE, never SQL's UNKNOWN,
 *   so `NOT` is the port's `not`. An `IN (subquery)` whose left side is a
 *   key and whose subquery selects keys cannot be UNKNOWN; neither can
 *   `EXISTS`; a nullable column is guarded with `IS NOT NULL`.
 *
 * And one rule of scope: **one language.** Every read of a current table, in
 * a predicate, an EXISTS or the sort's join, names the query's language
 * (`c.lang = ?`), so a field reads that language's items and no other's, and
 * an entity with no tree in it has no item to match and sorts as no value.
 */

#include <ghoti.io/defiant-storage-sql/macros.h>
#include "sql_internal.h"

#include <ghoti.io/cutil/allocator.h>
#include <string.h>

typedef struct Scope {
  const GDEFSTOR_Type * type;
  uint32_t tno;
  char e[GDEFSSQL_NAME_CAP]; /* the entity table's alias */
  char n[GDEFSSQL_NAME_CAP]; /* the newest state's alias */
} Scope;

typedef struct Qc {
  const GDEFSSQL_Dialect * d;
  const GDEFSTOR_ModelPlan * plan;
  const uint32_t * tnos;
  size_t ntnos;
  GDEFSSQL_Sb * sb;
  unsigned alias;
  size_t exists_at;
  GDEFSTOR_Result rc;
} Qc;

enum { SRC_FIELD = 1, SRC_SCALAR, SRC_VALUES };

typedef struct Src {
  int kind;
  int type;  /* the class: a GDEFSTOR_FieldType, or 0 for an empty list */
  int scale; /* DECIMAL: the field's, or -1 for constants */
  /* FIELD */
  const Scope * owner;
  uint32_t grp;
  uint32_t col;
  /* SCALAR */
  const char * alias;
  const char * column;
  int nullable;
  /* VALUES */
  const GDEFSTOR_Value * values;
  size_t nvalues;
} Src;

static void refuse(Qc * q, GDEFSTOR_Result rc) {
  if (q->rc == GDEFSTOR_OK) {
    q->rc = rc;
  }
}

static int failed(const Qc * q) {
  return q->rc != GDEFSTOR_OK || q->sb->rc != GDEFSTOR_OK;
}

static void next_alias(Qc * q, char out[GDEFSSQL_NAME_CAP]) {
  gdefssql_name_alias(out, q->alias);
  q->alias += 1;
}

static void scope_tables(Qc * q, const Scope * s) {
  char t[GDEFSSQL_NAME_CAP];
  gdefssql_name_table(t, s->tno, GDEFSSQL_ROLE_ENTITY, 0);
  gdefssql_sb_table(q->sb, t);
  gdefssql_sb_kw(q->sb, " ");
  gdefssql_sb_ident(q->sb, s->e);
  gdefssql_sb_kw(q->sb, " JOIN ");
  gdefssql_name_table(t, s->tno, GDEFSSQL_ROLE_NOW, 0);
  gdefssql_sb_table(q->sb, t);
  gdefssql_sb_kw(q->sb, " ");
  gdefssql_sb_ident(q->sb, s->n);
  gdefssql_sb_kw(q->sb, " ON ");
  gdefssql_sb_col(q->sb, s->n, "rid");
  gdefssql_sb_kw(q->sb, " = ");
  gdefssql_sb_col(q->sb, s->e, "rid");
}

/* ---- operands ------------------------------------------------------------- */

static int resolve_field(const GDEFSTOR_Type * t, const char * path,
    size_t path_len, GDEFSTOR_FieldInfo * out) {
  size_t at = 0;
  if (path == NULL || path_len == 0 ||
      gdefstor_type_find(t, path, path_len, &at) != GDEFSTOR_OK) {
    return 0;
  }
  memset(out, 0, sizeof *out);
  out->size = sizeof *out;
  if (gdefstor_type_field(t, at, out) != GDEFSTOR_OK) {
    return 0;
  }
  /* A group holds no value, a retired field cannot be asked, and a personal
   * value is ciphertext only a blind index could match. */
  return out->type != GDEFSTOR_FIELD_GROUP && !out->retired &&
         (out->flags & GDEFSTOR_FIELD_PERSONAL) == 0;
}

static void resolve(Qc * q, const GDEFSTOR_QueryOperand * o,
    const Scope * here, const Scope * outer, Src * out) {
  int is_outer = (o->flags & GDEFSTOR_QUERY_OUTER) != 0;
  const Scope * s = is_outer ? outer : here;
  memset(out, 0, sizeof *out);
  if ((o->flags & ~GDEFSTOR_QUERY_OUTER) != 0 || (is_outer && outer == NULL)) {
    refuse(q, GDEFSTOR_ERR_UNSUPPORTED);
    return;
  }
  switch (o->kind) {
  case GDEFSTOR_QUERY_FIELD: {
    GDEFSTOR_FieldInfo f;
    if (!resolve_field(s->type, o->field, o->field_len, &f)) {
      refuse(q, GDEFSTOR_ERR_UNSUPPORTED);
      return;
    }
    out->kind = SRC_FIELD;
    out->owner = s;
    out->type = f.type;
    out->scale = f.scale;
    if ((f.flags & GDEFSTOR_FIELD_MULTIPLE) != 0) {
      out->grp = f.group;
      out->col = 0;
    } else {
      out->grp = f.in_group;
      out->col = f.column;
    }
    return;
  }
  case GDEFSTOR_QUERY_VALUES: {
    size_t i;
    if (is_outer || (o->values == NULL && o->value_count != 0) ||
        o->value_count > GDEFSTOR_QUERY_MAX_TEXTS) {
      refuse(q, GDEFSTOR_ERR_UNSUPPORTED);
      return;
    }
    out->kind = SRC_VALUES;
    out->values = o->values;
    out->nvalues = o->value_count;
    out->scale = -1;
    for (i = 0; i < o->value_count; i++) {
      const GDEFSTOR_Value * v = &o->values[i];
      if (v->type < GDEFSTOR_FIELD_TEXT || v->type >= GDEFSTOR_FIELD_GROUP ||
          (out->type != 0 && v->type != out->type) || v->subject_len != 0 ||
          (v->bytes == NULL && v->len != 0)) {
        refuse(q, GDEFSTOR_ERR_UNSUPPORTED);
        return;
      }
      out->type = v->type;
    }
    return;
  }
  case GDEFSTOR_QUERY_META:
    out->kind = SRC_SCALAR;
    switch (o->meta) {
    case GDEFSTOR_META_PUBLISHED:
      out->alias = s->n;
      out->column = "pub";
      out->type = GDEFSTOR_FIELD_BOOLEAN;
      return;
    case GDEFSTOR_META_DELETED:
      out->alias = s->n;
      out->column = "del";
      out->type = GDEFSTOR_FIELD_BOOLEAN;
      return;
    case GDEFSTOR_META_CREATOR:
      /* The system created it: no value. */
      out->alias = s->e;
      out->column = "creator";
      out->type = GDEFSTOR_FIELD_REFERENCE;
      out->nullable = 1;
      return;
    case GDEFSTOR_META_CREATED:
      out->alias = s->e;
      out->column = "created";
      out->type = GDEFSTOR_FIELD_DATETIME;
      return;
    case GDEFSTOR_META_REVISION:
      out->alias = s->e;
      out->column = "rev";
      out->type = GDEFSTOR_FIELD_INTEGER;
      return;
    default:
      refuse(q, GDEFSTOR_ERR_UNSUPPORTED);
      return;
    }
  case GDEFSTOR_QUERY_ENTITY:
    out->kind = SRC_SCALAR;
    out->alias = s->e;
    out->column = "pid";
    out->type = GDEFSTOR_FIELD_REFERENCE;
    return;
  default:
    /* COLUMN and CONST are the projection form's. */
    refuse(q, GDEFSTOR_ERR_UNSUPPORTED);
    return;
  }
}

static int ordered(int type) {
  return type == 0 || type == GDEFSTOR_FIELD_INTEGER ||
         type == GDEFSTOR_FIELD_DECIMAL || type == GDEFSTOR_FIELD_FLOAT ||
         type == GDEFSTOR_FIELD_DATE || type == GDEFSTOR_FIELD_DATETIME;
}

static int same_class(const Src * a, const Src * b) {
  if (a->type == 0 || b->type == 0) {
    return 1;
  }
  if (a->type != b->type) {
    return 0;
  }
  return a->type != GDEFSTOR_FIELD_DECIMAL || a->scale < 0 || b->scale < 0 ||
         a->scale == b->scale;
}

/* ---- comparisons ---------------------------------------------------------- */

static const char * sql_op(int op) {
  switch (op) {
  case GDEFSTOR_QUERY_LT:
    return " < ";
  case GDEFSTOR_QUERY_LE:
    return " <= ";
  case GDEFSTOR_QUERY_GT:
    return " > ";
  case GDEFSTOR_QUERY_GE:
    return " >= ";
  default:
    return " = ";
  }
}

static int holds(int op, int c) {
  switch (op) {
  case GDEFSTOR_QUERY_LT:
    return c < 0;
  case GDEFSTOR_QUERY_LE:
    return c <= 0;
  case GDEFSTOR_QUERY_GT:
    return c > 0;
  case GDEFSTOR_QUERY_GE:
    return c >= 0;
  default:
    return c == 0;
  }
}

static void truth(Qc * q, int value) {
  /* No literal: a constant truth is written without a constant. */
  gdefssql_sb_kw(q->sb, value ? "(NULL IS NULL)" : "(NULL IS NOT NULL)");
}

static void value_param(Qc * q, const GDEFSTOR_Value * v) {
  GDEFSSQL_Datum d;
  gdefssql_value_datum(v, &d);
  gdefssql_sb_param(q->sb, &d);
}

/* `alias.lang = ?`, the query's language: what scopes every current-table
 * read to one language's items. */
static void in_language(Qc * q, const char * alias) {
  GDEFSSQL_Datum d;
  memset(&d, 0, sizeof d);
  d.kind = GDEFSSQL_DATUM_TEXT;
  d.bytes = (const uint8_t *)q->plan->query->language;
  d.len = q->plan->query->language_len;
  gdefssql_sb_col(q->sb, alias, "lang");
  gdefssql_sb_kw(q->sb, " = ");
  gdefssql_sb_param(q->sb, &d);
}

/* The column of a FIELD source, under alias @p alias. */
static void field_col(Qc * q, const char * alias, const Src * f) {
  char col[GDEFSSQL_NAME_CAP];
  gdefssql_name_column(col, f->col, 0);
  gdefssql_sb_col(q->sb, alias, col);
}

static void scalar(Qc * q, const Src * s) {
  gdefssql_sb_col(q->sb, s->alias, s->column);
}

/* One side of a comparison, as an expression: the field's column under
 * @p alias, a scalar, or (VALUES) value @p k. */
static void side(Qc * q, const Src * s, const char * alias, size_t k) {
  if (s->kind == SRC_FIELD) {
    field_col(q, alias, s);
  } else if (s->kind == SRC_SCALAR) {
    scalar(q, s);
  } else {
    value_param(q, &s->values[k]);
  }
}

/* `l op r` for every value of the one VALUES side, joined by OR; `x IN (?,
 * ...)` for equality against several. @p alias is the FIELD side's alias. */
static void against_values(Qc * q, int op, const Src * l, const Src * r,
    const char * alias) {
  const Src * vals = l->kind == SRC_VALUES ? l : r;
  const Src * other = l->kind == SRC_VALUES ? r : l;
  size_t i;
  if (op == GDEFSTOR_QUERY_EQ && vals->nvalues > 1) {
    side(q, other, alias, 0);
    gdefssql_sb_kw(q->sb, " IN (");
    for (i = 0; i < vals->nvalues; i++) {
      gdefssql_sb_kw(q->sb, i == 0 ? "" : ", ");
      value_param(q, &vals->values[i]);
    }
    gdefssql_sb_kw(q->sb, ")");
    return;
  }
  gdefssql_sb_kw(q->sb, "(");
  for (i = 0; i < vals->nvalues; i++) {
    gdefssql_sb_kw(q->sb, i == 0 ? "" : " OR ");
    side(q, l, alias, i);
    gdefssql_sb_kw(q->sb, sql_op(op));
    side(q, r, alias, i);
  }
  gdefssql_sb_kw(q->sb, ")");
}

/* owner.rid IN (SELECT a.ent FROM tN_cG a WHERE <cond>): some current item
 * of the field's group has a value standing in the relation. */
static void field_in(Qc * q, int op, const Src * l, const Src * r) {
  const Src * f = l->kind == SRC_FIELD ? l : r;
  char a[GDEFSSQL_NAME_CAP];
  char t[GDEFSSQL_NAME_CAP];
  next_alias(q, a);
  gdefssql_sb_kw(q->sb, "(");
  gdefssql_sb_col(q->sb, f->owner->e, "rid");
  gdefssql_sb_kw(q->sb, " IN (SELECT ");
  gdefssql_sb_col(q->sb, a, "ent");
  gdefssql_sb_kw(q->sb, " FROM ");
  gdefssql_name_table(t, f->owner->tno, GDEFSSQL_ROLE_CURRENT, f->grp);
  gdefssql_sb_table(q->sb, t);
  gdefssql_sb_kw(q->sb, " ");
  gdefssql_sb_ident(q->sb, a);
  gdefssql_sb_kw(q->sb, " WHERE ");
  in_language(q, a);
  gdefssql_sb_kw(q->sb, " AND ");
  if (l->kind == SRC_VALUES || r->kind == SRC_VALUES) {
    against_values(q, op, l, r, a);
  } else {
    /* A scalar is one value: a NULL one compares UNKNOWN, which selects
     * no item, so the IN is false, as an empty set is. */
    side(q, l, a, 0);
    gdefssql_sb_kw(q->sb, sql_op(op));
    side(q, r, a, 0);
  }
  gdefssql_sb_kw(q->sb, "))");
}

/* Two fields: some item of each, a pair in the relation. */
static void field_field(Qc * q, int op, const Src * l, const Src * r) {
  char a[GDEFSSQL_NAME_CAP];
  char b[GDEFSSQL_NAME_CAP];
  char t[GDEFSSQL_NAME_CAP];
  next_alias(q, a);
  next_alias(q, b);
  gdefssql_sb_kw(q->sb, "EXISTS (SELECT NULL FROM ");
  gdefssql_name_table(t, l->owner->tno, GDEFSSQL_ROLE_CURRENT, l->grp);
  gdefssql_sb_table(q->sb, t);
  gdefssql_sb_kw(q->sb, " ");
  gdefssql_sb_ident(q->sb, a);
  gdefssql_sb_kw(q->sb, " JOIN ");
  gdefssql_name_table(t, r->owner->tno, GDEFSSQL_ROLE_CURRENT, r->grp);
  gdefssql_sb_table(q->sb, t);
  gdefssql_sb_kw(q->sb, " ");
  gdefssql_sb_ident(q->sb, b);
  gdefssql_sb_kw(q->sb, " ON ");
  gdefssql_sb_col(q->sb, b, "ent");
  gdefssql_sb_kw(q->sb, " = ");
  gdefssql_sb_col(q->sb, r->owner->e, "rid");
  gdefssql_sb_kw(q->sb, " AND ");
  in_language(q, b);
  gdefssql_sb_kw(q->sb, " WHERE ");
  gdefssql_sb_col(q->sb, a, "ent");
  gdefssql_sb_kw(q->sb, " = ");
  gdefssql_sb_col(q->sb, l->owner->e, "rid");
  gdefssql_sb_kw(q->sb, " AND ");
  in_language(q, a);
  gdefssql_sb_kw(q->sb, " AND ");
  field_col(q, a, l);
  gdefssql_sb_kw(q->sb, sql_op(op));
  field_col(q, b, r);
  gdefssql_sb_kw(q->sb, ")");
}

/* Scalars and constants: a guarded expression. */
static void scalar_cmp(Qc * q, int op, const Src * l, const Src * r) {
  gdefssql_sb_kw(q->sb, "(");
  if (l->kind == SRC_SCALAR && l->nullable) {
    scalar(q, l);
    gdefssql_sb_kw(q->sb, " IS NOT NULL AND ");
  }
  if (r->kind == SRC_SCALAR && r->nullable) {
    scalar(q, r);
    gdefssql_sb_kw(q->sb, " IS NOT NULL AND ");
  }
  if (l->kind == SRC_VALUES || r->kind == SRC_VALUES) {
    against_values(q, op, l, r, NULL);
  } else {
    scalar(q, l);
    gdefssql_sb_kw(q->sb, sql_op(op));
    scalar(q, r);
  }
  gdefssql_sb_kw(q->sb, ")");
}

static void compare(Qc * q, int op, const Src * l, const Src * r) {
  if (l->kind == SRC_VALUES && r->kind == SRC_VALUES) {
    /* Two constant lists: decided here, with the port's order. */
    size_t a;
    size_t b;
    int found = 0;
    for (a = 0; a < l->nvalues && !found; a++) {
      for (b = 0; b < r->nvalues && !found; b++) {
        found = holds(op, gdefssql_value_compare(&l->values[a],
                              &r->values[b]));
      }
    }
    truth(q, found);
    return;
  }
  if ((l->kind == SRC_VALUES && l->nvalues == 0) ||
      (r->kind == SRC_VALUES && r->nvalues == 0)) {
    /* An empty set has no pair. */
    truth(q, 0);
    return;
  }
  if (l->kind == SRC_FIELD && r->kind == SRC_FIELD) {
    field_field(q, op, l, r);
  } else if (l->kind == SRC_FIELD || r->kind == SRC_FIELD) {
    field_in(q, op, l, r);
  } else {
    scalar_cmp(q, op, l, r);
  }
}

/* ---- blind-index equality ------------------------------------------------- */

/* owner.rid IN (SELECT b.ent FROM tN_b b JOIN tN_cG c ON c.ent = b.ent AND
 * c.item = b.item AND c.vfrom = b.vfrom WHERE b.fld = ? AND b.mac = ?): a
 * blind row of this field with this MAC whose version is an item's current
 * one. The join on vfrom is what keeps an older version's MAC from matching;
 * the (fld, mac) index drives it. Two-valued: keys IN keys. */
static void blind_in(Qc * q, const GDEFSTOR_QueryNode * n, const Scope * here,
    const Scope * outer) {
  const GDEFSTOR_QueryOperand * l = &n->left;
  const GDEFSTOR_QueryOperand * r = &n->right;
  GDEFSTOR_FieldInfo f;
  size_t at = 0;
  char a[GDEFSSQL_NAME_CAP];
  char c[GDEFSSQL_NAME_CAP];
  char t[GDEFSSQL_NAME_CAP];
  GDEFSSQL_Datum mac;
  uint32_t grp;
  (void)outer;
  if (n->child_count != 0 || l->kind != GDEFSTOR_QUERY_FIELD ||
      l->flags != 0 || r->kind != GDEFSTOR_QUERY_CONST || r->flags != 0 ||
      r->text_count != 1 || r->texts == NULL || r->texts[0].bytes == NULL ||
      r->texts[0].len != GDEFSTOR_BLIND_MAC_BYTES || l->field == NULL ||
      gdefstor_type_find(here->type, l->field, l->field_len, &at) !=
          GDEFSTOR_OK) {
    refuse(q, GDEFSTOR_ERR_UNSUPPORTED);
    return;
  }
  memset(&f, 0, sizeof f);
  f.size = sizeof f;
  if (gdefstor_type_field(here->type, at, &f) != GDEFSTOR_OK ||
      f.type == GDEFSTOR_FIELD_GROUP || f.retired ||
      (f.flags & GDEFSTOR_FIELD_PERSONAL) == 0) {
    refuse(q, GDEFSTOR_ERR_UNSUPPORTED);
    return;
  }
  grp = (f.flags & GDEFSTOR_FIELD_MULTIPLE) != 0 ? f.group : f.in_group;
  next_alias(q, a);
  next_alias(q, c);
  gdefssql_sb_kw(q->sb, "(");
  gdefssql_sb_col(q->sb, here->e, "rid");
  gdefssql_sb_kw(q->sb, " IN (SELECT ");
  gdefssql_sb_col(q->sb, a, "ent");
  gdefssql_sb_kw(q->sb, " FROM ");
  gdefssql_name_table(t, here->tno, GDEFSSQL_ROLE_BLIND, 0);
  gdefssql_sb_table(q->sb, t);
  gdefssql_sb_kw(q->sb, " ");
  gdefssql_sb_ident(q->sb, a);
  gdefssql_sb_kw(q->sb, " JOIN ");
  gdefssql_name_table(t, here->tno, GDEFSSQL_ROLE_CURRENT, grp);
  gdefssql_sb_table(q->sb, t);
  gdefssql_sb_kw(q->sb, " ");
  gdefssql_sb_ident(q->sb, c);
  gdefssql_sb_kw(q->sb, " ON ");
  gdefssql_sb_col(q->sb, c, "ent");
  gdefssql_sb_kw(q->sb, " = ");
  gdefssql_sb_col(q->sb, a, "ent");
  gdefssql_sb_kw(q->sb, " AND ");
  gdefssql_sb_col(q->sb, c, "item");
  gdefssql_sb_kw(q->sb, " = ");
  gdefssql_sb_col(q->sb, a, "item");
  gdefssql_sb_kw(q->sb, " AND ");
  gdefssql_sb_col(q->sb, c, "vfrom");
  gdefssql_sb_kw(q->sb, " = ");
  gdefssql_sb_col(q->sb, a, "vfrom");
  gdefssql_sb_kw(q->sb, " AND ");
  gdefssql_sb_col(q->sb, c, "lang");
  gdefssql_sb_kw(q->sb, " = ");
  gdefssql_sb_col(q->sb, a, "lang");
  gdefssql_sb_kw(q->sb, " WHERE ");
  in_language(q, a);
  gdefssql_sb_kw(q->sb, " AND ");
  gdefssql_sb_col(q->sb, a, "fld");
  gdefssql_sb_kw(q->sb, " = ");
  gdefssql_sb_int(q->sb, f.id);
  gdefssql_sb_kw(q->sb, " AND ");
  gdefssql_sb_col(q->sb, a, "mac");
  gdefssql_sb_kw(q->sb, " = ");
  memset(&mac, 0, sizeof mac);
  mac.kind = GDEFSSQL_DATUM_BYTES;
  mac.bytes = r->texts[0].bytes;
  mac.len = GDEFSTOR_BLIND_MAC_BYTES;
  gdefssql_sb_param(q->sb, &mac);
  gdefssql_sb_kw(q->sb, "))");
}

/* ---- the predicate -------------------------------------------------------- */

static void node(Qc * q, const GDEFSTOR_QueryNode * n, const Scope * here,
    const Scope * outer, size_t depth);

static void exists(Qc * q, const GDEFSTOR_QueryNode * n, const Scope * here,
    const Scope * outer, size_t depth) {
  Scope inner;
  const char * name = NULL;
  size_t name_len = 0;
  if (outer != NULL || n->child_count != 1 || n->children == NULL ||
      n->children[0] == NULL || n->exists_type == NULL) {
    /* An EXISTS inside an EXISTS is outside the form. */
    refuse(q, GDEFSTOR_ERR_UNSUPPORTED);
    return;
  }
  if (q->exists_at >= q->plan->exists_count ||
      q->exists_at + 1 >= q->ntnos ||
      gdefstor_type_name(q->plan->exists_types[q->exists_at], &name,
          &name_len) != GDEFSTOR_OK ||
      name_len != n->exists_type_len ||
      memcmp(name, n->exists_type, name_len) != 0) {
    /* The plan's types are one per EXISTS, depth first. */
    refuse(q, GDEFSTOR_ERR_INVALID);
    return;
  }
  memset(&inner, 0, sizeof inner);
  inner.type = q->plan->exists_types[q->exists_at];
  inner.tno = q->tnos[q->exists_at + 1];
  q->exists_at += 1;
  next_alias(q, inner.e);
  next_alias(q, inner.n);
  gdefssql_sb_kw(q->sb, "EXISTS (SELECT NULL FROM ");
  scope_tables(q, &inner);
  gdefssql_sb_kw(q->sb, " WHERE ");
  node(q, n->children[0], &inner, here, depth + 1);
  gdefssql_sb_kw(q->sb, ")");
}

static void node(Qc * q, const GDEFSTOR_QueryNode * n, const Scope * here,
    const Scope * outer, size_t depth) {
  size_t i;
  if (failed(q)) {
    return;
  }
  if (n == NULL || depth > GDEFSTOR_QUERY_MAX_DEPTH) {
    refuse(q, GDEFSTOR_ERR_UNSUPPORTED);
    return;
  }
  switch (n->op) {
  case GDEFSTOR_QUERY_TRUE:
  case GDEFSTOR_QUERY_FALSE:
    if (n->child_count != 0) {
      refuse(q, GDEFSTOR_ERR_UNSUPPORTED);
      return;
    }
    truth(q, n->op == GDEFSTOR_QUERY_TRUE);
    return;
  case GDEFSTOR_QUERY_AND:
  case GDEFSTOR_QUERY_OR:
    if (n->child_count > 0 && n->children == NULL) {
      refuse(q, GDEFSTOR_ERR_UNSUPPORTED);
      return;
    }
    if (n->child_count == 0) {
      /* An empty and is true, an empty or false. */
      truth(q, n->op == GDEFSTOR_QUERY_AND);
      return;
    }
    gdefssql_sb_kw(q->sb, "(");
    for (i = 0; i < n->child_count; i++) {
      if (i > 0) {
        gdefssql_sb_kw(q->sb, n->op == GDEFSTOR_QUERY_AND ? " AND " : " OR ");
      }
      node(q, n->children[i], here, outer, depth + 1);
    }
    gdefssql_sb_kw(q->sb, ")");
    return;
  case GDEFSTOR_QUERY_NOT:
    if (n->child_count != 1 || n->children == NULL) {
      refuse(q, GDEFSTOR_ERR_UNSUPPORTED);
      return;
    }
    gdefssql_sb_kw(q->sb, "(NOT ");
    node(q, n->children[0], here, outer, depth + 1);
    gdefssql_sb_kw(q->sb, ")");
    return;
  case GDEFSTOR_QUERY_EXISTS:
    exists(q, n, here, outer, depth);
    return;
  case GDEFSTOR_QUERY_BLIND:
    blind_in(q, n, here, outer);
    return;
  case GDEFSTOR_QUERY_EQ:
  case GDEFSTOR_QUERY_NE:
  case GDEFSTOR_QUERY_IN:
  case GDEFSTOR_QUERY_LT:
  case GDEFSTOR_QUERY_LE:
  case GDEFSTOR_QUERY_GT:
  case GDEFSTOR_QUERY_GE: {
    Src l;
    Src r;
    int op = n->op;
    if (n->child_count != 0 ||
        (op == GDEFSTOR_QUERY_IN && n->right.kind != GDEFSTOR_QUERY_VALUES)) {
      refuse(q, GDEFSTOR_ERR_UNSUPPORTED);
      return;
    }
    resolve(q, &n->left, here, outer, &l);
    resolve(q, &n->right, here, outer, &r);
    if (failed(q)) {
      return;
    }
    if (!same_class(&l, &r) ||
        (op >= GDEFSTOR_QUERY_LT && op <= GDEFSTOR_QUERY_GE &&
            (!ordered(l.type) || !ordered(r.type)))) {
      refuse(q, GDEFSTOR_ERR_UNSUPPORTED);
      return;
    }
    if (op == GDEFSTOR_QUERY_NE) {
      /* No pair is equal: the negation of some pair being equal. */
      gdefssql_sb_kw(q->sb, "(NOT ");
      compare(q, GDEFSTOR_QUERY_EQ, &l, &r);
      gdefssql_sb_kw(q->sb, ")");
      return;
    }
    compare(q, op == GDEFSTOR_QUERY_IN ? GDEFSTOR_QUERY_EQ : op, &l, &r);
    return;
  }
  default:
    refuse(q, GDEFSTOR_ERR_UNSUPPORTED);
    return;
  }
}

/* ---- the statements ------------------------------------------------------- */

/* The sort key's expression; *nullable when it can be no value. */
static void sort_key(Qc * q, const GDEFSTOR_QuerySort * s, const Scope * main,
    int * nullable) {
  const GDEFSTOR_QueryOperand * o = &s->key;
  *nullable = 0;
  if (s->numeric != 0 || o->flags != 0) {
    refuse(q, GDEFSTOR_ERR_UNSUPPORTED);
    return;
  }
  if (o->kind == GDEFSTOR_QUERY_FIELD) {
    GDEFSTOR_FieldInfo f;
    char col[GDEFSSQL_NAME_CAP];
    /* One value per entity: a single field of the root group. */
    if (!resolve_field(main->type, o->field, o->field_len, &f) ||
        f.parent != GDEFSTOR_TYPE_ROOT ||
        (f.flags & GDEFSTOR_FIELD_MULTIPLE) != 0 || f.in_group != 0) {
      refuse(q, GDEFSTOR_ERR_UNSUPPORTED);
      return;
    }
    gdefssql_name_column(col, f.column, 0);
    gdefssql_sb_col(q->sb, "a2", col);
    *nullable = 1;
    return;
  }
  if (o->kind == GDEFSTOR_QUERY_META || o->kind == GDEFSTOR_QUERY_ENTITY) {
    Src k;
    resolve(q, o, main, NULL, &k);
    if (failed(q)) {
      return;
    }
    scalar(q, &k);
    *nullable = k.nullable;
    return;
  }
  refuse(q, GDEFSTOR_ERR_UNSUPPORTED);
}

static int needs_root(const GDEFSTOR_Query * query) {
  size_t i;
  for (i = 0; i < query->sort_count; i++) {
    if (query->sorts[i].key.kind == GDEFSTOR_QUERY_FIELD) {
      return 1;
    }
  }
  return 0;
}

static void statement(Qc * q, const Scope * main, int page) {
  const GDEFSTOR_Query * query = q->plan->query;
  size_t i;
  /* a0 and a1 are the queried entity and its state, a2 its root item; the
   * predicate's aliases start at a3 in both statements. */
  q->alias = 3;
  q->exists_at = 0;
  gdefssql_sb_kw(q->sb, page ? "SELECT " : "SELECT COUNT(*) FROM ");
  if (page) {
    gdefssql_sb_col(q->sb, main->e, "rid");
    gdefssql_sb_kw(q->sb, ", ");
    gdefssql_sb_col(q->sb, main->e, "pid");
    gdefssql_sb_kw(q->sb, " FROM ");
  }
  scope_tables(q, main);
  if (page && needs_root(query)) {
    char t[GDEFSSQL_NAME_CAP];
    gdefssql_sb_kw(q->sb, " LEFT JOIN ");
    gdefssql_name_table(t, main->tno, GDEFSSQL_ROLE_CURRENT, 0);
    gdefssql_sb_table(q->sb, t);
    gdefssql_sb_kw(q->sb, " ");
    gdefssql_sb_ident(q->sb, "a2");
    gdefssql_sb_kw(q->sb, " ON ");
    gdefssql_sb_col(q->sb, "a2", "ent");
    gdefssql_sb_kw(q->sb, " = ");
    gdefssql_sb_col(q->sb, main->e, "rid");
    /* In the ON, not the WHERE: an entity with no root in the language
     * stays, with no value to sort by. */
    gdefssql_sb_kw(q->sb, " AND ");
    in_language(q, "a2");
  }
  if (query->where != NULL) {
    gdefssql_sb_kw(q->sb, " WHERE ");
    node(q, query->where, main, NULL, 1);
  }
  if (!failed(q) && q->exists_at != q->plan->exists_count) {
    refuse(q, GDEFSTOR_ERR_INVALID);
  }
  if (!page || failed(q)) {
    return;
  }
  gdefssql_sb_kw(q->sb, " ORDER BY ");
  for (i = 0; i < query->sort_count; i++) {
    const GDEFSTOR_QuerySort * s = &query->sorts[i];
    int nullable = 0;
    sort_key(q, s, main, &nullable);
    if (s->descending) {
      gdefssql_sb_kw(q->sb, " DESC");
    }
    if (nullable) {
      /* No value first ascending, last descending: as the port's order. */
      int want_first = !s->descending;
      int engine_first = q->d->nulls_low ? !s->descending : s->descending;
      if (want_first != engine_first) {
        gdefssql_sb_kw(q->sb, want_first ? " NULLS FIRST" : " NULLS LAST");
      }
    }
    gdefssql_sb_kw(q->sb, ", ");
  }
  /* The final tiebreak, ascending under any sort: the row. */
  gdefssql_sb_col(q->sb, main->e, "rid");
  gdefssql_sb_kw(q->sb, " LIMIT ");
  gdefssql_sb_int(q->sb, q->plan->limit > (uint64_t)INT64_MAX
                             ? INT64_MAX
                             : (int64_t)q->plan->limit);
  gdefssql_sb_kw(q->sb, " OFFSET ");
  gdefssql_sb_int(q->sb, query->offset > (uint64_t)INT64_MAX
                             ? INT64_MAX
                             : (int64_t)query->offset);
}

GDEFSTOR_Result gdefssql_compile_plan(const GDEFSSQL_Dialect * d,
    const char * qual, const GDEFSTOR_ModelPlan * plan,
    const uint32_t * tnos, size_t ntnos, GDEFSSQL_Sb * total,
    GDEFSSQL_Sb * page) {
  const GDEFSTOR_Query * query;
  Scope main;
  Qc q;
  int pass;
  (void)qual;
  if (d == NULL || plan == NULL || plan->query == NULL || plan->type == NULL ||
      tnos == NULL || ntnos != 1 + plan->exists_count ||
      (plan->exists_count > 0 && plan->exists_types == NULL) ||
      plan->query->size != sizeof(GDEFSTOR_Query) ||
      plan->query->language == NULL || plan->query->language_len == 0) {
    return GDEFSTOR_ERR_INVALID;
  }
  query = plan->query;
  if (query->select_count != 0 || query->entity_numeric != 0 ||
      query->sort_count > GDEFSTOR_QUERY_MAX_SORTS ||
      (query->sorts == NULL && query->sort_count != 0)) {
    return GDEFSTOR_ERR_UNSUPPORTED;
  }
  memset(&main, 0, sizeof main);
  main.type = plan->type;
  main.tno = tnos[0];
  gdefssql_name_alias(main.e, 0);
  gdefssql_name_alias(main.n, 1);
  for (pass = 0; pass < (plan->count_only ? 1 : 2); pass++) {
    memset(&q, 0, sizeof q);
    q.d = d;
    q.plan = plan;
    q.tnos = tnos;
    q.ntnos = ntnos;
    q.sb = pass == 0 ? total : page;
    statement(&q, &main, pass == 1);
    /* A builder that ran out of memory stopped the walk wherever it was, so
     * its error comes first: what the walk concluded after it means nothing. */
    if (q.sb->rc != GDEFSTOR_OK) {
      return q.sb->rc;
    }
    if (q.rc != GDEFSTOR_OK) {
      return q.rc;
    }
    if (q.sb->nbinds > d->max_binds) {
      return GDEFSTOR_ERR_LIMIT;
    }
  }
  return GDEFSTOR_OK;
}

/* ---- the public form ------------------------------------------------------ */

struct GDEFSSQL_Compiled {
  const GDEFSTOR_Allocator * alloc;
  GDEFSSQL_Dialect d;
  size_t count;
  GDEFSSQL_Sb sb[2];
};

GDEFSSQL_Result gdefssql_query_compile(const GDEFSSQL_Dialect * dialect,
    const char * qualifier, const GDEFSTOR_ModelPlan * plan,
    const uint32_t * type_numbers, size_t type_number_count,
    const GDEFSSQL_Allocator * allocator, GDEFSSQL_Compiled ** out) {
  const GDEFSTOR_Allocator * alloc =
      allocator != NULL ? allocator : gcu_allocator_default();
  GDEFSSQL_Compiled * c;
  GDEFSTOR_Result rc;
  GDEFSSQL_Result check;
  if (out == NULL || plan == NULL) {
    return GDEFSSQL_ERR_INVALID;
  }
  check = gdefssql_dialect_check(dialect);
  if (check != GDEFSSQL_OK) {
    return check;
  }
  if (qualifier != NULL && qualifier[0] != '\0' &&
      !gdefssql_name_ok(qualifier, dialect->max_identifier)) {
    return GDEFSSQL_ERR_INVALID;
  }
  c = (GDEFSSQL_Compiled *)gdefssql_mem0(alloc, sizeof *c);
  if (c == NULL) {
    return GDEFSSQL_ERR_OOM;
  }
  c->alloc = alloc;
  c->d = *dialect;
  c->count = plan->count_only ? 1 : 2;
  gdefssql_sb_init(&c->sb[0], alloc, &c->d, qualifier);
  gdefssql_sb_init(&c->sb[1], alloc, &c->d, qualifier);
  rc = gdefssql_compile_plan(&c->d, qualifier, plan, type_numbers,
      type_number_count, &c->sb[0], &c->sb[1]);
  if (rc != GDEFSTOR_OK) {
    gdefssql_compiled_free(c);
    return gdefssql_result_from_port(rc);
  }
  *out = c;
  return GDEFSSQL_OK;
}

size_t gdefssql_compiled_count(const GDEFSSQL_Compiled * c) {
  return c == NULL ? 0 : c->count;
}

GDEFSSQL_Result gdefssql_compiled_text(const GDEFSSQL_Compiled * c,
    size_t index, const char ** out_text, size_t * out_len) {
  if (c == NULL || index >= c->count || out_text == NULL || out_len == NULL) {
    return GDEFSSQL_ERR_INVALID;
  }
  *out_text = c->sb[index].text != NULL ? c->sb[index].text : "";
  *out_len = c->sb[index].len;
  return GDEFSSQL_OK;
}

size_t gdefssql_compiled_bind_count(const GDEFSSQL_Compiled * c,
    size_t index) {
  return (c == NULL || index >= c->count) ? 0 : c->sb[index].nbinds;
}

GDEFSSQL_Result gdefssql_compiled_bind(const GDEFSSQL_Compiled * c,
    size_t index, size_t k, GDEFSSQL_Datum * out) {
  if (c == NULL || index >= c->count || out == NULL ||
      k >= c->sb[index].nbinds) {
    return GDEFSSQL_ERR_INVALID;
  }
  *out = c->sb[index].binds[k];
  return GDEFSSQL_OK;
}

void gdefssql_compiled_free(GDEFSSQL_Compiled * c) {
  if (c == NULL) {
    return;
  }
  gdefssql_sb_free(&c->sb[0]);
  gdefssql_sb_free(&c->sb[1]);
  gcu_allocator_free(c->alloc, c);
}
