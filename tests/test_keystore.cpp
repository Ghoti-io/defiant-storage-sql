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
 * The key store backend (keystore.h) on the test engine: the port's key store
 * conformance suite (keystore.inc), what the tables hold after each step, and
 * gdefssql_store_version(), which an engine's executor uses to refuse a store
 * whose layout it does not know.
 */

#include <gtest/gtest.h>

#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/defiant-storage/defiant-storage.h>

#include "sql_stub.h"

#include <cstring>
#include <map>
#include <memory>
#include <string>

namespace {

/* Every key store the suite opens, by its port handle, so a test can look at
 * the tables behind it. The engine is owned by the backend (destroy). */
std::map<GDEFSTOR_KeyStore *, sqlstub::Engine *> & Engines() {
  static std::map<GDEFSTOR_KeyStore *, sqlstub::Engine *> m;
  return m;
}

void DestroyEngine(void * ctx) {
  delete static_cast<sqlstub::Engine *>(ctx);
}

GDEFSTOR_Result OpenOn(const GDEFSSQL_Dialect & d, bool qualified,
    const GDEFSTOR_KeyCipher * cipher, GDEFSTOR_KeyStore ** out) {
  sqlstub::Engine * e = new sqlstub::Engine(d, qualified);
  GDEFSSQL_Executor x = sqlstub::ExecutorOf(e);
  x.destroy = DestroyEngine;
  GDEFSTOR_KeyStoreBackend b;
  GDEFSSQL_Result rc = gdefssql_keystore_backend(&d, &x, cipher, nullptr, &b);
  if (rc != GDEFSSQL_OK) {
    delete e;
    return gdefssql_result_to_port(rc);
  }
  GDEFSTOR_Result prc = gdefstor_keystore_create(&b, nullptr, out);
  if (prc != GDEFSTOR_OK) {
    b.destroy(b.ctx);
    return prc;
  }
  Engines()[*out] = e;
  return GDEFSTOR_OK;
}

}  // namespace

GDEFSTOR_Result KeyStoreOpen(const GDEFSTOR_KeyCipher * cipher,
    GDEFSTOR_KeyStore ** out) {
  return OpenOn(sqlstub::StdDialect(), false, cipher, out);
}

#include "keystore.inc"

namespace {

using keystore_conformance::KeyStore;

const sqlstub::Table * TableOf(GDEFSTOR_KeyStore * ks, const char * tenant,
    const std::string & name) {
  sqlstub::Engine * e = Engines()[ks];
  auto s = e->stores.find(tenant);
  if (s == e->stores.end()) {
    return nullptr;
  }
  for (auto & t : s->second.committed.tables) {
    /* A qualified engine names its tables after the schema. */
    if (t.first == name || (t.first.size() > name.size() &&
        t.first.compare(t.first.size() - name.size(), name.size(), name) == 0 &&
        t.first[t.first.size() - name.size() - 1] == '.')) {
      return t.second.get();
    }
  }
  return nullptr;
}

bool HasSubject(const sqlstub::Table * t, const std::string & subject) {
  int c = t->Find("subject");
  for (const auto & r : t->rows) {
    if (c >= 0 && r[(size_t)c].s == subject) {
      return true;
    }
  }
  return false;
}

/* Erasure deletes the key row and writes the ledger row, in one write: the
 * key is gone from the table, not only hidden behind the ledger. */
TEST_F(KeyStore, ErasureDeletesTheKeyRowAndWritesTheLedgerRow) {
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_subject_create(ks, "t1", 2, "s1", 2));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_subject_create(ks, "t1", 2, "s2", 2));
  const sqlstub::Table * keys = TableOf(ks, "t1", "dsk_key");
  ASSERT_NE(nullptr, keys);
  EXPECT_TRUE(HasSubject(keys, "s1"));
  EXPECT_EQ(2u, keys->rows.size());
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_erase(ks, "t1", 2, "s1", 2, 77));
  keys = TableOf(ks, "t1", "dsk_key");
  const sqlstub::Table * ledger = TableOf(ks, "t1", "dsk_ledger");
  ASSERT_NE(nullptr, keys);
  ASSERT_NE(nullptr, ledger);
  EXPECT_FALSE(HasSubject(keys, "s1"));
  EXPECT_TRUE(HasSubject(keys, "s2"));
  EXPECT_TRUE(HasSubject(ledger, "s1"));
  EXPECT_EQ(1u, ledger->rows.size());
}

/* Sealed material is a key like the others: the erasure deletes its row in
 * the same write as the ledger row, and a restore replaying the ledger does
 * the same. */
TEST_F(KeyStore, ErasureDeletesTheSealedRowToo) {
  const uint8_t m[3] = {7, 8, 9};
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_subject_put(ks, "t1", 2, "s1", 2, m,
      3));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_subject_put(ks, "t1", 2, "s2", 2, m,
      3));
  const sqlstub::Table * sealed = TableOf(ks, "t1", "dsk_sealed");
  ASSERT_NE(nullptr, sealed);
  EXPECT_EQ(2u, sealed->rows.size());
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_erase(ks, "t1", 2, "s1", 2, 5));
  sealed = TableOf(ks, "t1", "dsk_sealed");
  ASSERT_NE(nullptr, sealed);
  EXPECT_FALSE(HasSubject(sealed, "s1"));
  EXPECT_TRUE(HasSubject(sealed, "s2"));
  const std::string set =
      "defiant-keyset/1\ntaken 1\nledger s2 6\n";
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_import(ks, "t1", 2,
      reinterpret_cast<const uint8_t *>(set.data()), set.size()));
  sealed = TableOf(ks, "t1", "dsk_sealed");
  ASSERT_NE(nullptr, sealed);
  EXPECT_EQ(0u, sealed->rows.size());
  EXPECT_EQ(2u, TableOf(ks, "t1", "dsk_ledger")->rows.size());
}

/* The key the cipher's randomness made is the bytes stored, and a tenant's
 * keys are its own: another tenant has no key for the same subject. */
TEST_F(KeyStore, KeysAreKeptPerTenant) {
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_subject_create(ks, "t1", 2, "s1", 2));
  uint8_t * out = nullptr;
  size_t len = 0;
  int erased = 0;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_keystore_wrap(ks, "t2", 2, "s1", 2,
      reinterpret_cast<const uint8_t *>("x"), 1, &erased, &out, &len));
  EXPECT_EQ(nullptr, out);
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_subject_create(ks, "t2", 2, "s1", 2));
  const sqlstub::Table * a = TableOf(ks, "t1", "dsk_key");
  const sqlstub::Table * b = TableOf(ks, "t2", "dsk_key");
  ASSERT_NE(nullptr, a);
  ASSERT_NE(nullptr, b);
  int ca = a->Find("secret");
  int cb = b->Find("secret");
  ASSERT_GE(ca, 0);
  ASSERT_GE(cb, 0);
  ASSERT_EQ(1u, a->rows.size());
  ASSERT_EQ(1u, b->rows.size());
  EXPECT_EQ(16u, a->rows[0][(size_t)ca].s.size());
  EXPECT_NE(a->rows[0][(size_t)ca].s, b->rows[0][(size_t)cb].s);
}

TEST(KeyStoreSql, ACipherAndAnExecutorAreRequired) {
  sqlstub::Engine e(sqlstub::StdDialect());
  GDEFSSQL_Dialect d = sqlstub::StdDialect();
  GDEFSSQL_Executor x = sqlstub::ExecutorOf(&e);
  GDEFSTOR_KeyCipher c;
  std::memset(&c, 0, sizeof c);
  c.size = sizeof c;
  c.key_bytes = 16;
  GDEFSTOR_KeyStoreBackend b;
  EXPECT_EQ(GDEFSSQL_ERR_INVALID, gdefssql_keystore_backend(&d, &x, &c, nullptr, &b));
  EXPECT_EQ(GDEFSSQL_ERR_INVALID, gdefssql_keystore_backend(&d, &x, nullptr, nullptr, &b));
  EXPECT_EQ(GDEFSSQL_ERR_INVALID, gdefssql_keystore_backend(&d, nullptr, &c, nullptr, &b));
}

/* The other dialect: numbered parameters, its own quotes, a schema per
 * tenant, no RETURNING. Its counter is a SELECT and an UPDATE. */
TEST(KeyStoreSql, TheOtherDialectServesTheSameSteps) {
  keystore_conformance::ToyCipher toy;
  GDEFSTOR_KeyCipher cipher;
  std::memset(&cipher, 0, sizeof cipher);
  cipher.size = sizeof cipher;
  cipher.ctx = &toy;
  cipher.key_bytes = 16;
  cipher.random = keystore_conformance::toy_random;
  cipher.seal = keystore_conformance::toy_seal;
  cipher.open = keystore_conformance::toy_open;
  GDEFSSQL_Dialect d = sqlstub::FakeDialect();
  d.returning = 0;
  GDEFSTOR_KeyStore * ks = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, OpenOn(d, true, &cipher, &ks));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_subject_create(ks, "t1", 2, "s1", 2));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_erase(ks, "t1", 2, "s2", 2, 5));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_erase(ks, "t1", 2, "s1", 2, 6));
  GDEFSTOR_Ledger * l = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_ledger(ks, "t1", 2, &l));
  ASSERT_EQ(2u, gdefstor_ledger_count(l));
  const char * s = nullptr;
  size_t n = 0;
  int64_t when = 0;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_ledger_row(l, 0, &s, &n, &when));
  EXPECT_EQ("s2", std::string(s, n));
  EXPECT_EQ(5, when);
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_ledger_row(l, 1, &s, &n, &when));
  EXPECT_EQ("s1", std::string(s, n));
  gdefstor_ledger_free(l);
  gdefstor_keystore_destroy(ks);
}

/* ---- gdefssql_store_version ---------------------------------------------- */

struct StubConn {
  sqlstub::Engine * e;
  GDEFSSQL_Executor x;
  void * conn = nullptr;
  const char * qual = nullptr;
  /* The test engine runs nothing outside a transaction, so the read is
   * inside one; on an engine with autocommit it need not be. */
  StubConn(sqlstub::Engine * engine, const char * tenant) : e(engine) {
    x = sqlstub::ExecutorOf(e);
    x.open(x.ctx, tenant, std::strlen(tenant), 0, &conn, &qual);
    if (conn != nullptr) {
      const char * b = e->d.begin_read;
      x.exec(x.ctx, conn, b, std::strlen(b));
    }
  }
  ~StubConn() {
    if (conn != nullptr) {
      const char * c = e->d.commit;
      x.exec(x.ctx, conn, c, std::strlen(c));
      x.close(x.ctx, conn, 0);
    }
  }
};

/* What the data backend records is version 1, and the key store's likewise;
 * each is read from its own table. */
TEST(StoreVersion, ReadsTheVersionEachLayoutRecords) {
  GDEFSSQL_Dialect d = sqlstub::StdDialect();
  sqlstub::Engine e(d);
  GDEFSSQL_Executor x = sqlstub::ExecutorOf(&e);
  GDEFSTOR_ModelBackend model;
  ASSERT_EQ(GDEFSSQL_OK, gdefssql_model_backend(&d, &x, nullptr, &model));
  void * txn = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, model.begin(model.ctx, "t1", 2, &txn));
  ASSERT_EQ(GDEFSTOR_OK, model.commit(model.ctx, txn));
  model.destroy(model.ctx);
  {
    StubConn c(&e, "t1");
    ASSERT_NE(nullptr, c.conn);
    int64_t v = -1;
    EXPECT_EQ(GDEFSTOR_OK, gdefssql_store_version(&d, &c.x, c.conn, c.qual,
        GDEFSSQL_STORE_DATA, &v));
    EXPECT_EQ(GDEFSSQL_SCHEMA_VERSION, v);
  }
  GDEFSTOR_KeyCipher cipher;
  keystore_conformance::ToyCipher toy;
  std::memset(&cipher, 0, sizeof cipher);
  cipher.size = sizeof cipher;
  cipher.ctx = &toy;
  cipher.key_bytes = 16;
  cipher.random = keystore_conformance::toy_random;
  cipher.seal = keystore_conformance::toy_seal;
  cipher.open = keystore_conformance::toy_open;
  GDEFSTOR_KeyStoreBackend kb;
  ASSERT_EQ(GDEFSSQL_OK, gdefssql_keystore_backend(&d, &x, &cipher, nullptr, &kb));
  ASSERT_EQ(GDEFSTOR_OK, kb.create(kb.ctx, "t2", 2, "s1", 2));
  kb.destroy(kb.ctx);
  {
    StubConn c(&e, "t2");
    ASSERT_NE(nullptr, c.conn);
    int64_t v = -1;
    EXPECT_EQ(GDEFSTOR_OK, gdefssql_store_version(&d, &c.x, c.conn, c.qual,
        GDEFSSQL_STORE_KEYS, &v));
    EXPECT_EQ(GDEFSSQL_KEYSTORE_VERSION, v);
  }
  EXPECT_EQ(0, e.open_conns);
  EXPECT_EQ(0, e.open_stmts);
}

/* A newer layout records a larger number, and the executor sees it. */
TEST(StoreVersion, ANewerVersionIsReadAsItIs) {
  GDEFSSQL_Dialect d = sqlstub::StdDialect();
  sqlstub::Engine e(d);
  GDEFSSQL_Executor x = sqlstub::ExecutorOf(&e);
  GDEFSTOR_ModelBackend model;
  ASSERT_EQ(GDEFSSQL_OK, gdefssql_model_backend(&d, &x, nullptr, &model));
  void * txn = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, model.begin(model.ctx, "t1", 2, &txn));
  ASSERT_EQ(GDEFSTOR_OK, model.commit(model.ctx, txn));
  model.destroy(model.ctx);
  auto & t = e.stores["t1"].committed.tables["dss_schema"];
  ASSERT_TRUE(t != nullptr);
  t = std::make_shared<sqlstub::Table>(*t); /* snapshots share tables */
  ASSERT_EQ(1u, t->rows.size());
  t->rows[0][(size_t)t->Find("version")] = sqlstub::Val::Int(2);
  StubConn c(&e, "t1");
  int64_t v = -1;
  EXPECT_EQ(GDEFSTOR_OK, gdefssql_store_version(&d, &c.x, c.conn, c.qual,
      GDEFSSQL_STORE_DATA, &v));
  EXPECT_EQ(2, v);
}

TEST(StoreVersion, ArgumentsAreChecked) {
  GDEFSSQL_Dialect d = sqlstub::StdDialect();
  sqlstub::Engine e(d);
  GDEFSSQL_Executor x = sqlstub::ExecutorOf(&e);
  int64_t v = 7;
  int dummy = 0;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefssql_store_version(&d, &x, &dummy,
      nullptr, 3, &v));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefssql_store_version(&d, &x, nullptr,
      nullptr, GDEFSSQL_STORE_DATA, &v));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefssql_store_version(&d, &x, &dummy,
      "Not A Name", GDEFSSQL_STORE_DATA, &v));
  EXPECT_EQ(7, v);
}

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
