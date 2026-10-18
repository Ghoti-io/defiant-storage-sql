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
 * The key store backend over a dialect and an executor (keystore.h): subject
 * keys and the erasure ledger in four tables of their own, written with the
 * same statement builder as the entity backend, so every value is bound and
 * every identifier is a fixed name.
 */

#include <ghoti.io/defiant-storage-sql/macros.h>
#include <ghoti.io/defiant-storage-sql/keystore.h>
#include "backend_internal.h"

#include <ghoti.io/cutil/allocator.h>
#include <string.h>

typedef GDEFSSQL_Cx Cx;
typedef GDEFSSQL_Sb Sb;
typedef GDEFSSQL_St St;

/* A tenant whose four tables are known made in this backend's life. */
typedef struct KReady {
  char * tenant;
  size_t len;
  struct KReady * next;
} KReady;

typedef struct KBack {
  GDEFSSQL_Back b; /* alloc, dialect and executor, for the statement runner */
  GDEFSTOR_KeyCipher cipher;
  KReady * ready;
} KBack;

static int same(const char * a, size_t an, const char * b, size_t bn) {
  return an == bn && (an == 0 || memcmp(a, b, an) == 0);
}

static void wipe(uint8_t * p, size_t n) {
  volatile uint8_t * v = p;
  size_t i;
  for (i = 0; i < n; i++) {
    v[i] = 0;
  }
}

static GDEFSSQL_Datum dint(int64_t v) {
  GDEFSSQL_Datum d;
  memset(&d, 0, sizeof d);
  d.kind = GDEFSSQL_DATUM_INT;
  d.integer = v;
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

static GDEFSTOR_Result txn_sql(Cx * c, const char * sql) {
  GDEFSTOR_Result rc = c->b->x.exec(c->b->x.ctx, c->conn, sql, strlen(sql));
  if (rc != GDEFSTOR_OK) {
    c->failed = 1;
  }
  return rc;
}

/* ---- the tables ------------------------------------------------------------ */

typedef struct KCol {
  const char * name;
  int coltype;
} KCol;

static GDEFSTOR_Result create_table(Cx * c, const char * table,
    const KCol * cols, size_t n, const char * key) {
  Sb sb;
  GDEFSTOR_Result rc;
  size_t i;
  gdefssql_sb_init(&sb, c->b->alloc, &c->b->d, c->qual);
  gdefssql_sb_kw(&sb, "CREATE TABLE IF NOT EXISTS ");
  gdefssql_sb_table(&sb, table);
  gdefssql_sb_kw(&sb, " (");
  for (i = 0; i < n; i++) {
    gdefssql_sb_kw(&sb, i == 0 ? "" : ", ");
    gdefssql_sb_ident(&sb, cols[i].name);
    gdefssql_sb_kw(&sb, " ");
    gdefssql_sb_type(&sb, cols[i].coltype);
    gdefssql_sb_kw(&sb, " NOT NULL");
  }
  gdefssql_sb_kw(&sb, ", PRIMARY KEY (");
  gdefssql_sb_ident(&sb, key);
  gdefssql_sb_kw(&sb, "))");
  rc = gdefssql_cx_exec(c, &sb);
  gdefssql_sb_free(&sb);
  return rc;
}

/* SELECT "a0"."<col>" FROM <table> "a0" WHERE "a0"."<kcol>" = ?: one row by
 * its key, or *out_found 0. The column is read as @p kind into @p out (an
 * int64) or, for bytes, copied into @p buf of @p cap bytes (*out_len). */
typedef struct KRead {
  int kind;
  int64_t integer;
  uint8_t * buf;
  size_t cap;
  size_t len;
} KRead;

static GDEFSTOR_Result read_by(Cx * c, const char * table, const char * col,
    const char * kcol, const GDEFSSQL_Datum * key, KRead * out,
    int * out_found) {
  Sb sb;
  St s;
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
  gdefssql_sb_col(&sb, "a0", kcol);
  gdefssql_sb_kw(&sb, " = ");
  gdefssql_sb_param(&sb, key);
  rc = gdefssql_st_open(c, &sb, &s);
  if (rc == GDEFSTOR_OK) {
    rc = gdefssql_st_step(&s, &row);
  }
  if (rc == GDEFSTOR_OK && row) {
    if (out->kind == GDEFSSQL_DATUM_INT) {
      rc = gdefssql_st_int(&s, 0, &out->integer, NULL);
    } else {
      const uint8_t * p = NULL;
      size_t n = 0;
      rc = gdefssql_st_bytes(&s, 0, out->kind, &p, &n, NULL);
      if (rc == GDEFSTOR_OK && (out->buf == NULL || n != out->cap)) {
        /* A key of another length than the cipher's is not this store's. */
        rc = GDEFSTOR_ERR_CORRUPT;
      }
      if (rc == GDEFSTOR_OK) {
        memcpy(out->buf, p, n);
        out->len = n;
      }
    }
    *out_found = rc == GDEFSTOR_OK;
  }
  gdefssql_st_close(&s);
  gdefssql_sb_free(&sb);
  return rc;
}

/* An insert of fixed columns. */
typedef struct KKv {
  const char * col;
  GDEFSSQL_Datum v;
} KKv;

static GDEFSTOR_Result insert(Cx * c, const char * table, const KKv * kv,
    size_t n) {
  Sb sb;
  GDEFSTOR_Result rc;
  size_t i;
  gdefssql_sb_init(&sb, c->b->alloc, &c->b->d, c->qual);
  gdefssql_sb_kw(&sb, "INSERT INTO ");
  gdefssql_sb_table(&sb, table);
  gdefssql_sb_kw(&sb, " (");
  for (i = 0; i < n; i++) {
    gdefssql_sb_kw(&sb, i == 0 ? "" : ", ");
    gdefssql_sb_ident(&sb, kv[i].col);
  }
  gdefssql_sb_kw(&sb, ") VALUES (");
  for (i = 0; i < n; i++) {
    gdefssql_sb_kw(&sb, i == 0 ? "" : ", ");
    gdefssql_sb_param(&sb, &kv[i].v);
  }
  gdefssql_sb_kw(&sb, ")");
  rc = gdefssql_cx_run(c, &sb);
  gdefssql_sb_free(&sb);
  return rc;
}

/* The four tables, their index and their rows, if missing (with the
 * dialect's qualifier_create, the tenant's schema first), and the version
 * check. Runs inside a write transaction.
 *
 * The one index here, dsk_ledger_seq, is made in this transaction, beside
 * its table: the key store is a port of its own, not a data store, this is
 * its first transaction, and the table is empty and new in it. AD-15's rule
 * (no index built inside a step) is about a data store's steps, where a
 * table may be large; that is backend.h's "Index builds". */
static GDEFSTOR_Result tenant_init(Cx * c) {
  static const KCol schema_cols[] = {{"id", GDEFSSQL_COL_ID},
      {"version", GDEFSSQL_COL_ID}};
  static const KCol counter_cols[] = {{"id", GDEFSSQL_COL_ID},
      {"last_seq", GDEFSSQL_COL_ID}};
  static const KCol key_cols[] = {{"subject", GDEFSSQL_COL_REFERENCE},
      {"secret", GDEFSSQL_COL_BYTES}};
  static const KCol ledger_cols[] = {{"subject", GDEFSSQL_COL_REFERENCE},
      {"seq", GDEFSSQL_COL_ID}, {"at", GDEFSSQL_COL_DATETIME}};
  GDEFSSQL_Datum one = dint(1);
  KRead r;
  GDEFSTOR_Result rc;
  int found = 0;
  rc = GDEFSTOR_OK;
  if (c->qual != NULL && c->b->d.qualifier_create != NULL) {
    /* The tenant's schema, in the same transaction as its tables. */
    Sb sb;
    gdefssql_sb_init(&sb, c->b->alloc, &c->b->d, NULL);
    gdefssql_sb_kw(&sb, c->b->d.qualifier_create);
    gdefssql_sb_ident(&sb, c->qual);
    rc = gdefssql_cx_exec(c, &sb);
    gdefssql_sb_free(&sb);
  }
  if (rc == GDEFSTOR_OK) {
    rc = create_table(c, "dsk_schema", schema_cols, 2, "id");
  }
  if (rc == GDEFSTOR_OK) {
    rc = create_table(c, "dsk_counter", counter_cols, 2, "id");
  }
  if (rc == GDEFSTOR_OK) {
    rc = create_table(c, "dsk_key", key_cols, 2, "subject");
  }
  if (rc == GDEFSTOR_OK) {
    rc = create_table(c, "dsk_ledger", ledger_cols, 3, "subject");
  }
  if (rc == GDEFSTOR_OK) {
    Sb sb;
    gdefssql_sb_init(&sb, c->b->alloc, &c->b->d, c->qual);
    gdefssql_sb_kw(&sb, "CREATE UNIQUE INDEX IF NOT EXISTS ");
    gdefssql_sb_ident(&sb, "dsk_ledger_seq");
    gdefssql_sb_kw(&sb, " ON ");
    gdefssql_sb_table(&sb, "dsk_ledger");
    gdefssql_sb_kw(&sb, " (");
    gdefssql_sb_ident(&sb, "seq");
    gdefssql_sb_kw(&sb, ")");
    rc = gdefssql_cx_exec(c, &sb);
    gdefssql_sb_free(&sb);
  }
  memset(&r, 0, sizeof r);
  r.kind = GDEFSSQL_DATUM_INT;
  if (rc == GDEFSTOR_OK) {
    rc = read_by(c, "dsk_schema", "version", "id", &one, &r, &found);
  }
  if (rc == GDEFSTOR_OK && found && r.integer != GDEFSSQL_KEYSTORE_VERSION) {
    /* Another layout: refused, never read as this one. */
    return r.integer > GDEFSSQL_KEYSTORE_VERSION ? GDEFSTOR_ERR_UNSUPPORTED
                                                 : GDEFSTOR_ERR_FORMAT;
  }
  if (rc == GDEFSTOR_OK && !found) {
    KKv kv[2];
    kv[0].col = "id";
    kv[0].v = dint(1);
    kv[1].col = "version";
    kv[1].v = dint(GDEFSSQL_KEYSTORE_VERSION);
    rc = insert(c, "dsk_schema", kv, 2);
  }
  if (rc == GDEFSTOR_OK) {
    rc = read_by(c, "dsk_counter", "last_seq", "id", &one, &r, &found);
  }
  if (rc == GDEFSTOR_OK && !found) {
    KKv kv[2];
    kv[0].col = "id";
    kv[0].v = dint(1);
    kv[1].col = "last_seq";
    kv[1].v = dint(0);
    rc = insert(c, "dsk_counter", kv, 2);
  }
  return rc;
}

/* dsk_counter.last_seq, advanced by one: the value it has now. */
static GDEFSTOR_Result next_seq(Cx * c, int64_t * out) {
  Sb sb;
  St s;
  GDEFSTOR_Result rc;
  int row = 0;
  int64_t v = 0;
  GDEFSSQL_Datum one = dint(1);
  gdefssql_sb_init(&sb, c->b->alloc, &c->b->d, c->qual);
  if (c->b->d.returning) {
    gdefssql_sb_kw(&sb, "UPDATE ");
    gdefssql_sb_table(&sb, "dsk_counter");
    gdefssql_sb_kw(&sb, " SET ");
    gdefssql_sb_ident(&sb, "last_seq");
    gdefssql_sb_kw(&sb, " = ");
    gdefssql_sb_ident(&sb, "last_seq");
    gdefssql_sb_kw(&sb, " + ");
    gdefssql_sb_int(&sb, 1);
    gdefssql_sb_kw(&sb, " WHERE ");
    gdefssql_sb_ident(&sb, "id");
    gdefssql_sb_kw(&sb, " = ");
    gdefssql_sb_int(&sb, 1);
    gdefssql_sb_kw(&sb, " RETURNING ");
    gdefssql_sb_ident(&sb, "last_seq");
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
    while (rc == GDEFSTOR_OK && row) {
      rc = gdefssql_st_step(&s, &row);
    }
    gdefssql_st_close(&s);
  } else {
    KRead r;
    int found = 0;
    memset(&r, 0, sizeof r);
    r.kind = GDEFSSQL_DATUM_INT;
    rc = read_by(c, "dsk_counter", "last_seq", "id", &one, &r, &found);
    if (rc == GDEFSTOR_OK && (!found || r.integer < 0)) {
      rc = GDEFSTOR_ERR_CORRUPT;
    }
    if (rc == GDEFSTOR_OK) {
      v = r.integer + 1;
      gdefssql_sb_kw(&sb, "UPDATE ");
      gdefssql_sb_table(&sb, "dsk_counter");
      gdefssql_sb_kw(&sb, " SET ");
      gdefssql_sb_ident(&sb, "last_seq");
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
    *out = v;
  }
  return rc;
}

/* ---- connections ------------------------------------------------------------ */

static KReady * ready_find(KBack * k, const char * t, size_t n) {
  KReady * r;
  for (r = k->ready; r != NULL; r = r->next) {
    if (same(r->tenant, r->len, t, n)) {
      return r;
    }
  }
  return NULL;
}

static void ready_add(KBack * k, const char * t, size_t n) {
  KReady * r = (KReady *)gdefssql_mem0(k->b.alloc, sizeof *r);
  if (r == NULL) {
    return; /* not remembering it costs only doing it again */
  }
  r->tenant = (char *)gcu_allocator_malloc(k->b.alloc, n + 1);
  if (r->tenant == NULL) {
    gcu_allocator_free(k->b.alloc, r);
    return;
  }
  if (n > 0) {
    memcpy(r->tenant, t, n);
  }
  r->tenant[n] = '\0';
  r->len = n;
  r->next = k->ready;
  k->ready = r;
}

static GDEFSTOR_Result checked_qualifier(const KBack * k, const char * q) {
  if (q == NULL || q[0] == '\0' || gdefssql_name_ok(q, k->b.d.max_identifier)) {
    return GDEFSTOR_OK;
  }
  return GDEFSTOR_ERR_INTERNAL;
}

/* Open the tenant's write connection and its write transaction; the four
 * tables first, in a transaction of their own, the first time. */
static GDEFSTOR_Result w_open(KBack * k, const char * t, size_t n, Cx * c) {
  const char * qual = NULL;
  GDEFSTOR_Result rc;
  memset(c, 0, sizeof *c);
  c->b = &k->b;
  rc = k->b.x.open(k->b.x.ctx, t, n, 1, &c->conn, &qual);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (c->conn == NULL) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  c->qual = qual != NULL && qual[0] != '\0' ? qual : NULL;
  rc = checked_qualifier(k, qual);
  if (rc == GDEFSTOR_OK && ready_find(k, t, n) == NULL) {
    rc = txn_sql(c, k->b.d.begin_write);
    if (rc == GDEFSTOR_OK) {
      rc = tenant_init(c);
      if (rc == GDEFSTOR_OK) {
        rc = txn_sql(c, k->b.d.commit);
      } else {
        txn_sql(c, k->b.d.rollback);
      }
    }
    if (rc == GDEFSTOR_OK) {
      ready_add(k, t, n);
    }
  }
  if (rc == GDEFSTOR_OK) {
    rc = txn_sql(c, k->b.d.begin_write);
  }
  if (rc != GDEFSTOR_OK) {
    k->b.x.close(k->b.x.ctx, c->conn, 1);
    c->conn = NULL;
  }
  return rc;
}

/* Commit (or, after an error, roll back) and return the connection. */
static GDEFSTOR_Result w_close(KBack * k, Cx * c, GDEFSTOR_Result rc) {
  if (rc == GDEFSTOR_OK && !c->failed) {
    rc = txn_sql(c, k->b.d.commit);
    if (rc != GDEFSTOR_OK) {
      txn_sql(c, k->b.d.rollback);
    }
  } else {
    txn_sql(c, k->b.d.rollback);
    c->failed = 1;
    if (rc == GDEFSTOR_OK) {
      rc = GDEFSTOR_ERR_IO;
    }
  }
  k->b.x.close(k->b.x.ctx, c->conn, c->failed);
  c->conn = NULL;
  return rc;
}

/* A read connection in one read transaction; *out_empty when the tenant has
 * no key store at all. */
static GDEFSTOR_Result r_open(KBack * k, const char * t, size_t n, Cx * c,
    int * out_empty) {
  const char * qual = NULL;
  GDEFSTOR_Result rc;
  memset(c, 0, sizeof *c);
  c->b = &k->b;
  *out_empty = 0;
  rc = k->b.x.open(k->b.x.ctx, t, n, 0, &c->conn, &qual);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (c->conn == NULL) {
    *out_empty = 1;
    return GDEFSTOR_OK;
  }
  c->qual = qual != NULL && qual[0] != '\0' ? qual : NULL;
  rc = checked_qualifier(k, qual);
  if (rc == GDEFSTOR_OK) {
    rc = txn_sql(c, k->b.d.begin_read);
  }
  if (rc != GDEFSTOR_OK) {
    k->b.x.close(k->b.x.ctx, c->conn, 1);
    c->conn = NULL;
  }
  return rc;
}

static GDEFSTOR_Result r_close(KBack * k, Cx * c, GDEFSTOR_Result rc) {
  if (c->conn == NULL) {
    return rc;
  }
  if (rc == GDEFSTOR_OK && !c->failed) {
    rc = txn_sql(c, k->b.d.commit);
  } else {
    txn_sql(c, k->b.d.rollback);
    c->failed = 1;
  }
  k->b.x.close(k->b.x.ctx, c->conn, c->failed);
  c->conn = NULL;
  return rc;
}

/* ---- the members ------------------------------------------------------------ */

static GDEFSTOR_Result erased_or_key(Cx * c, const GDEFSSQL_Datum * subject,
    int * out_erased, KRead * key, int * out_has_key) {
  KRead r;
  int found = 0;
  GDEFSTOR_Result rc;
  memset(&r, 0, sizeof r);
  r.kind = GDEFSSQL_DATUM_INT;
  *out_erased = 0;
  *out_has_key = 0;
  rc = read_by(c, "dsk_ledger", "seq", "subject", subject, &r, &found);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (found) {
    *out_erased = 1;
    return GDEFSTOR_OK;
  }
  return read_by(c, "dsk_key", "secret", "subject", subject, key,
      out_has_key);
}

static GDEFSTOR_Result k_create(void * ctx, const char * t, size_t tl,
    const char * s, size_t sl) {
  KBack * k = (KBack *)ctx;
  GDEFSSQL_Datum subject = dbytes(GDEFSSQL_DATUM_TEXT, s, sl);
  uint8_t * secret;
  KRead key;
  Cx c;
  int erased = 0;
  int has = 0;
  GDEFSTOR_Result rc;
  secret = (uint8_t *)gcu_allocator_malloc(k->b.alloc, k->cipher.key_bytes);
  if (secret == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  memset(&key, 0, sizeof key);
  key.kind = GDEFSSQL_DATUM_BYTES;
  key.buf = secret;
  key.cap = k->cipher.key_bytes;
  rc = w_open(k, t, tl, &c);
  if (rc != GDEFSTOR_OK) {
    gcu_allocator_free(k->b.alloc, secret);
    return rc;
  }
  rc = erased_or_key(&c, &subject, &erased, &key, &has);
  if (rc == GDEFSTOR_OK && (erased || has)) {
    /* It has a key, or was erased: nothing written, and not a failed write. */
    wipe(secret, k->cipher.key_bytes);
    rc = w_close(k, &c, GDEFSTOR_OK);
    gcu_allocator_free(k->b.alloc, secret);
    return rc == GDEFSTOR_OK ? GDEFSTOR_ERR_INVALID : rc;
  }
  if (rc == GDEFSTOR_OK) {
    rc = k->cipher.random(k->cipher.ctx, secret, k->cipher.key_bytes);
  }
  if (rc == GDEFSTOR_OK) {
    KKv kv[2];
    kv[0].col = "subject";
    kv[0].v = subject;
    kv[1].col = "secret";
    kv[1].v = dbytes(GDEFSSQL_DATUM_BYTES, secret, k->cipher.key_bytes);
    rc = insert(&c, "dsk_key", kv, 2);
  }
  if (rc != GDEFSTOR_OK) {
    c.failed = 1;
  }
  rc = w_close(k, &c, rc);
  wipe(secret, k->cipher.key_bytes);
  gcu_allocator_free(k->b.alloc, secret);
  return rc;
}

static GDEFSTOR_Result k_seal_or_open(KBack * k, int seal, const char * t,
    size_t tl, const char * s, size_t sl, const uint8_t * in, size_t n,
    int * out_erased, uint8_t ** out, size_t * out_len) {
  GDEFSSQL_Datum subject = dbytes(GDEFSSQL_DATUM_TEXT, s, sl);
  uint8_t * secret;
  KRead key;
  Cx c;
  int empty = 0;
  int erased = 0;
  int has = 0;
  GDEFSTOR_Result rc;
  *out_erased = 0;
  secret = (uint8_t *)gcu_allocator_malloc(k->b.alloc, k->cipher.key_bytes);
  if (secret == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  memset(&key, 0, sizeof key);
  key.kind = GDEFSSQL_DATUM_BYTES;
  key.buf = secret;
  key.cap = k->cipher.key_bytes;
  rc = r_open(k, t, tl, &c, &empty);
  if (rc == GDEFSTOR_OK && !empty) {
    rc = erased_or_key(&c, &subject, &erased, &key, &has);
    rc = r_close(k, &c, rc);
  }
  if (rc == GDEFSTOR_OK && erased) {
    *out_erased = 1;
  } else if (rc == GDEFSTOR_OK && !has) {
    rc = GDEFSTOR_ERR_INVALID;
  } else if (rc == GDEFSTOR_OK) {
    rc = (seal ? k->cipher.seal : k->cipher.open)(k->cipher.ctx, k->b.alloc,
        secret, in, n, out, out_len);
  }
  wipe(secret, k->cipher.key_bytes);
  gcu_allocator_free(k->b.alloc, secret);
  return rc;
}

static GDEFSTOR_Result k_wrap(void * ctx, const char * t, size_t tl,
    const char * s, size_t sl, const uint8_t * in, size_t n, int * out_erased,
    uint8_t ** out, size_t * out_len) {
  return k_seal_or_open((KBack *)ctx, 1, t, tl, s, sl, in, n, out_erased, out,
      out_len);
}

static GDEFSTOR_Result k_unwrap(void * ctx, const char * t, size_t tl,
    const char * s, size_t sl, const uint8_t * in, size_t n, int * out_erased,
    uint8_t ** out, size_t * out_len) {
  return k_seal_or_open((KBack *)ctx, 0, t, tl, s, sl, in, n, out_erased, out,
      out_len);
}

static GDEFSTOR_Result k_erase(void * ctx, const char * t, size_t tl,
    const char * s, size_t sl, int64_t when) {
  KBack * k = (KBack *)ctx;
  GDEFSSQL_Datum subject = dbytes(GDEFSSQL_DATUM_TEXT, s, sl);
  KRead r;
  Cx c;
  int found = 0;
  int64_t seq = 0;
  GDEFSTOR_Result rc = w_open(k, t, tl, &c);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  memset(&r, 0, sizeof r);
  r.kind = GDEFSSQL_DATUM_INT;
  rc = read_by(&c, "dsk_ledger", "seq", "subject", &subject, &r, &found);
  if (rc == GDEFSTOR_OK && found) {
    return w_close(k, &c, GDEFSTOR_OK); /* once is enough */
  }
  if (rc == GDEFSTOR_OK) {
    /* The key first: once this commits the data is unreadable. */
    Sb sb;
    gdefssql_sb_init(&sb, c.b->alloc, &c.b->d, c.qual);
    gdefssql_sb_kw(&sb, "DELETE FROM ");
    gdefssql_sb_table(&sb, "dsk_key");
    gdefssql_sb_kw(&sb, " WHERE ");
    gdefssql_sb_ident(&sb, "subject");
    gdefssql_sb_kw(&sb, " = ");
    gdefssql_sb_param(&sb, &subject);
    rc = gdefssql_cx_run(&c, &sb);
    gdefssql_sb_free(&sb);
  }
  if (rc == GDEFSTOR_OK) {
    rc = next_seq(&c, &seq);
  }
  if (rc == GDEFSTOR_OK) {
    KKv kv[3];
    kv[0].col = "subject";
    kv[0].v = subject;
    kv[1].col = "seq";
    kv[1].v = dint(seq);
    kv[2].col = "at";
    kv[2].v = dint(when);
    rc = insert(&c, "dsk_ledger", kv, 3);
  }
  if (rc != GDEFSTOR_OK) {
    c.failed = 1;
  }
  return w_close(k, &c, rc);
}

static GDEFSTOR_Result k_ledger(void * ctx, const char * t, size_t tl,
    GDEFSTOR_Result (*visit)(void * vctx, const char * subject,
        size_t subject_len, int64_t when_ms),
    void * vctx) {
  KBack * k = (KBack *)ctx;
  Cx c;
  Sb sb;
  St s;
  int empty = 0;
  int row = 0;
  GDEFSTOR_Result rc;
  if (visit == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = r_open(k, t, tl, &c, &empty);
  if (rc != GDEFSTOR_OK || empty) {
    return rc;
  }
  gdefssql_sb_init(&sb, c.b->alloc, &c.b->d, c.qual);
  gdefssql_sb_kw(&sb, "SELECT ");
  gdefssql_sb_col(&sb, "a0", "subject");
  gdefssql_sb_kw(&sb, ", ");
  gdefssql_sb_col(&sb, "a0", "at");
  gdefssql_sb_kw(&sb, " FROM ");
  gdefssql_sb_table(&sb, "dsk_ledger");
  gdefssql_sb_kw(&sb, " ");
  gdefssql_sb_ident(&sb, "a0");
  gdefssql_sb_kw(&sb, " ORDER BY ");
  gdefssql_sb_col(&sb, "a0", "seq");
  rc = gdefssql_st_open(&c, &sb, &s);
  while (rc == GDEFSTOR_OK) {
    const uint8_t * p = NULL;
    size_t n = 0;
    int64_t at = 0;
    rc = gdefssql_st_step(&s, &row);
    if (rc != GDEFSTOR_OK || !row) {
      break;
    }
    rc = gdefssql_st_bytes(&s, 0, GDEFSSQL_DATUM_TEXT, &p, &n, NULL);
    if (rc == GDEFSTOR_OK) {
      rc = gdefssql_st_int(&s, 1, &at, NULL);
    }
    if (rc == GDEFSTOR_OK) {
      rc = visit(vctx, (const char *)p, n, at);
    }
  }
  gdefssql_st_close(&s);
  gdefssql_sb_free(&sb);
  return r_close(k, &c, rc);
}

static void k_destroy(void * ctx) {
  KBack * k = (KBack *)ctx;
  if (k == NULL) {
    return;
  }
  while (k->ready != NULL) {
    KReady * r = k->ready;
    k->ready = r->next;
    gcu_allocator_free(k->b.alloc, r->tenant);
    gcu_allocator_free(k->b.alloc, r);
  }
  if (k->b.x.destroy != NULL) {
    k->b.x.destroy(k->b.x.ctx);
  }
  gcu_allocator_free(k->b.alloc, k);
}

GDEFSSQL_Result gdefssql_keystore_backend(const GDEFSSQL_Dialect * dialect,
    const GDEFSSQL_Executor * executor, const GDEFSTOR_KeyCipher * cipher,
    const GDEFSSQL_Allocator * allocator, GDEFSTOR_KeyStoreBackend * out) {
  const GDEFSTOR_Allocator * alloc =
      allocator != NULL ? allocator : gcu_allocator_default();
  GDEFSSQL_Result check;
  KBack * k;
  if (executor == NULL || cipher == NULL || out == NULL) {
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
  if (cipher->size != sizeof *cipher || cipher->random == NULL ||
      cipher->seal == NULL || cipher->open == NULL || cipher->key_bytes == 0 ||
      cipher->key_bytes > GDEFSSQL_KEYSTORE_MAX_KEY_BYTES) {
    return GDEFSSQL_ERR_INVALID;
  }
  k = (KBack *)gdefssql_mem0(alloc, sizeof *k);
  if (k == NULL) {
    return GDEFSSQL_ERR_OOM;
  }
  k->b.alloc = alloc;
  k->b.d = *dialect;
  k->b.x = *executor;
  k->cipher = *cipher;
  memset(out, 0, sizeof *out);
  out->size = sizeof *out;
  out->ctx = k;
  out->destroy = k_destroy;
  out->create = k_create;
  out->wrap = k_wrap;
  out->unwrap = k_unwrap;
  out->erase = k_erase;
  out->ledger = k_ledger;
  return GDEFSSQL_OK;
}
