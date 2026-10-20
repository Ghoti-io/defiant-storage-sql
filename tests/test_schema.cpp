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
 * The physical schema (backend.h): what a declaration becomes, that every
 * step is additive and taken once, that no caller's name reaches SQL, the
 * dialect check, layout versions, and what an executor error does.
 */

#include <gtest/gtest.h>

#include "test_util.h"

#include <random>
#include <set>

using namespace tu;

namespace {

const int T = GDEFSTOR_FIELD_TEXT;
const int I = GDEFSTOR_FIELD_INTEGER;
const int D = GDEFSTOR_FIELD_DECIMAL;
const int FL = GDEFSTOR_FIELD_FLOAT;
const int DT = GDEFSTOR_FIELD_DATETIME;
const int R = GDEFSTOR_FIELD_REFERENCE;
const unsigned MUL = GDEFSTOR_FIELD_MULTIPLE;
const unsigned IDX = GDEFSTOR_FIELD_INDEXED;
const unsigned PER = GDEFSTOR_FIELD_PERSONAL;

/* The nested example of the report: groups in a multiple, in a multiple. */
std::vector<F> Order() {
  return {Field("number", T, IDX), Field("total", D, 0, 2),
      Field("placed", DT), Field("tags", T, MUL), Field("buyer", R),
      Field("email", T, PER),
      Group("lines", MUL, {Field("sku", T, IDX), Field("qty", I),
          Field("price", FL), Group("taxes", MUL, {Field("rate", D, 0, 4),
              Field("kind", T)})})};
}

struct Fixture : ::testing::Test {
  sqlstub::Rig rig;
  void Open(const GDEFSSQL_Dialect & d, bool qualified = false) {
    ASSERT_EQ(GDEFSTOR_OK, sqlstub::OpenRig(rig, d, qualified, nullptr));
  }
  void TearDown() override {
    gdefstor_store_destroy(rig.store);
  }
  size_t Mark() const {
    return rig.engine->log.size();
  }
};

class Schema : public Fixture {};

/* What a declaration becomes, under the SQLite-like dialect. Every table of
 * backend.h, a value column per scalar at its stable column, a subject column
 * beside the personal one, the indexes the query form uses: the tables in the
 * step's transaction, the indexes after it commits (AD-15), in the order of
 * their records. */
TEST_F(Schema, ANestedDeclarationYieldsTheDocumentedTables) {
  Open(sqlstub::StdDialect());
  ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, "shop.order", {Field("x", T)}));
  size_t from = Mark();
  ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, "shop.order2", Order()));
  std::vector<std::string> ddl = DdlSince(*rig.engine, from);
  const char * const want[] = {
      "CREATE TABLE IF NOT EXISTS \"t2_e\" (\"rid\" INTEGER NOT NULL, \"pid\" TEXT NOT NULL, \"creator\" TEXT, \"created\" INTEGER NOT NULL, \"rev\" INTEGER NOT NULL, \"last_item\" INTEGER NOT NULL, PRIMARY KEY (\"rid\"))",
      "CREATE TABLE IF NOT EXISTS \"t2_s\" (\"rid\" INTEGER NOT NULL, \"seq\" INTEGER NOT NULL, \"pub\" INTEGER NOT NULL, \"del\" INTEGER NOT NULL, \"actor\" TEXT NOT NULL, \"at\" INTEGER NOT NULL, PRIMARY KEY (\"rid\", \"seq\"))",
      "CREATE TABLE IF NOT EXISTS \"t2_r\" (\"rid\" INTEGER NOT NULL, \"rev\" INTEGER NOT NULL, \"cseq\" INTEGER NOT NULL, \"who\" TEXT NOT NULL, \"at\" INTEGER NOT NULL, \"why\" TEXT NOT NULL, \"langs\" TEXT NOT NULL, PRIMARY KEY (\"rid\", \"rev\"))",
      "CREATE TABLE IF NOT EXISTS \"t2_n\" (\"rid\" INTEGER NOT NULL, \"pub\" INTEGER NOT NULL, \"del\" INTEGER NOT NULL, PRIMARY KEY (\"rid\"))",
      "CREATE TABLE IF NOT EXISTS \"t2_g0\" (\"ent\" INTEGER NOT NULL, \"lang\" TEXT NOT NULL, \"item\" INTEGER NOT NULL, \"vfrom\" INTEGER NOT NULL, \"vto\" INTEGER, \"parent\" INTEGER NOT NULL, \"ord\" REAL NOT NULL, \"v0\" TEXT, \"v1\" INTEGER, \"v2\" INTEGER, \"v3\" TEXT, \"v4\" BLOB, \"v4s\" TEXT, PRIMARY KEY (\"ent\", \"lang\", \"item\", \"vfrom\"))",
      "CREATE TABLE IF NOT EXISTS \"t2_c0\" (\"ent\" INTEGER NOT NULL, \"lang\" TEXT NOT NULL, \"item\" INTEGER NOT NULL, \"vfrom\" INTEGER NOT NULL, \"parent\" INTEGER NOT NULL, \"ord\" REAL NOT NULL, \"v0\" TEXT, \"v1\" INTEGER, \"v2\" INTEGER, \"v3\" TEXT, \"v4\" BLOB, \"v4s\" TEXT, PRIMARY KEY (\"ent\", \"lang\", \"item\"))",
      "CREATE TABLE IF NOT EXISTS \"t2_g1\" (\"ent\" INTEGER NOT NULL, \"lang\" TEXT NOT NULL, \"item\" INTEGER NOT NULL, \"vfrom\" INTEGER NOT NULL, \"vto\" INTEGER, \"parent\" INTEGER NOT NULL, \"ord\" REAL NOT NULL, \"v0\" TEXT, PRIMARY KEY (\"ent\", \"lang\", \"item\", \"vfrom\"))",
      "CREATE TABLE IF NOT EXISTS \"t2_c1\" (\"ent\" INTEGER NOT NULL, \"lang\" TEXT NOT NULL, \"item\" INTEGER NOT NULL, \"vfrom\" INTEGER NOT NULL, \"parent\" INTEGER NOT NULL, \"ord\" REAL NOT NULL, \"v0\" TEXT, PRIMARY KEY (\"ent\", \"lang\", \"item\"))",
      "CREATE TABLE IF NOT EXISTS \"t2_g2\" (\"ent\" INTEGER NOT NULL, \"lang\" TEXT NOT NULL, \"item\" INTEGER NOT NULL, \"vfrom\" INTEGER NOT NULL, \"vto\" INTEGER, \"parent\" INTEGER NOT NULL, \"ord\" REAL NOT NULL, \"v0\" TEXT, \"v1\" INTEGER, \"v2\" REAL, PRIMARY KEY (\"ent\", \"lang\", \"item\", \"vfrom\"))",
      "CREATE TABLE IF NOT EXISTS \"t2_c2\" (\"ent\" INTEGER NOT NULL, \"lang\" TEXT NOT NULL, \"item\" INTEGER NOT NULL, \"vfrom\" INTEGER NOT NULL, \"parent\" INTEGER NOT NULL, \"ord\" REAL NOT NULL, \"v0\" TEXT, \"v1\" INTEGER, \"v2\" REAL, PRIMARY KEY (\"ent\", \"lang\", \"item\"))",
      "CREATE TABLE IF NOT EXISTS \"t2_g3\" (\"ent\" INTEGER NOT NULL, \"lang\" TEXT NOT NULL, \"item\" INTEGER NOT NULL, \"vfrom\" INTEGER NOT NULL, \"vto\" INTEGER, \"parent\" INTEGER NOT NULL, \"ord\" REAL NOT NULL, \"v0\" INTEGER, \"v1\" TEXT, PRIMARY KEY (\"ent\", \"lang\", \"item\", \"vfrom\"))",
      "CREATE TABLE IF NOT EXISTS \"t2_c3\" (\"ent\" INTEGER NOT NULL, \"lang\" TEXT NOT NULL, \"item\" INTEGER NOT NULL, \"vfrom\" INTEGER NOT NULL, \"parent\" INTEGER NOT NULL, \"ord\" REAL NOT NULL, \"v0\" INTEGER, \"v1\" TEXT, PRIMARY KEY (\"ent\", \"lang\", \"item\"))",
      /* email is personal: the blind table, and its two indexes below. */
      "CREATE TABLE IF NOT EXISTS \"t2_b\" (\"ent\" INTEGER NOT NULL, \"item\" INTEGER NOT NULL, \"vfrom\" INTEGER NOT NULL, \"fld\" INTEGER NOT NULL, \"subject\" TEXT NOT NULL, \"mac\" BLOB NOT NULL, \"kid\" INTEGER NOT NULL, \"norm\" INTEGER NOT NULL, \"lang\" TEXT NOT NULL, PRIMARY KEY (\"ent\", \"item\", \"vfrom\", \"fld\"))",
      "CREATE INDEX IF NOT EXISTS \"t2_i0_0\" ON \"t2_c0\" (\"v0\", \"lang\", \"ent\")",
      "CREATE UNIQUE INDEX IF NOT EXISTS \"t2p\" ON \"t2_e\" (\"pid\")",
      "CREATE INDEX IF NOT EXISTS \"t2_bm\" ON \"t2_b\" (\"fld\", \"mac\")",
      "CREATE INDEX IF NOT EXISTS \"t2_bs\" ON \"t2_b\" (\"subject\")",
      "CREATE INDEX IF NOT EXISTS \"t2_j0_4\" ON \"t2_g0\" (\"v4s\")",
      "CREATE INDEX IF NOT EXISTS \"t2_i2_0\" ON \"t2_c2\" (\"v0\", \"lang\", \"ent\")",
  };
  std::vector<std::string> expect(std::begin(want), std::end(want));
  EXPECT_EQ(expect, ddl);
  for (size_t i = from; i < rig.engine->log.size(); i++) {
    const sqlstub::LogEntry & l = rig.engine->log[i];
    if (IsIndexBuild(l.text)) {
      EXPECT_FALSE(l.in_txn) << l.text;
    } else if (IsDdl(l.text)) {
      EXPECT_TRUE(l.in_txn) << l.text;
    }
  }
}

/* The schema steps live in the backend's type_store; the port skips it for an
 * equal declaration, so it is called here directly, twice. */
TEST_F(Schema, RedeclaringTheSameTreeRunsNoStep) {
  Open(sqlstub::StdDialect());
  ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, "shop.order", Order()));
  GDEFSTOR_Type * type = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_type_get(rig.store, kTenant, 2, "shop.order",
      10, &type));
  uint8_t * text = nullptr;
  size_t len = 0;
  ASSERT_EQ(GDEFSTOR_OK, rig.model.type_load(rig.model.ctx, nullptr, kTenant, 2,
      "shop.order", 10, &text, &len));
  for (int round = 0; round < 2; round++) {
    size_t from = Mark();
    void * txn = nullptr;
    ASSERT_EQ(GDEFSTOR_OK, rig.model.begin(rig.model.ctx, kTenant, 2, &txn));
    ASSERT_EQ(GDEFSTOR_OK, rig.model.type_store(rig.model.ctx, txn, type, text,
        len));
    ASSERT_EQ(GDEFSTOR_OK, rig.model.commit(rig.model.ctx, txn));
    EXPECT_EQ(std::vector<std::string>(), DdlSince(*rig.engine, from)) << round;
  }
  gcu_allocator_free(nullptr, text);
  gdefstor_type_free(type);
}

TEST_F(Schema, AddingAFieldIsOneAddColumnStep) {
  Open(sqlstub::StdDialect());
  std::vector<F> v1 = Order();
  ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, "shop.order", v1));
  /* A field of the root group: one column, in the truth and current tables. */
  std::vector<F> v2 = v1;
  v2.push_back(Field("note", T));
  size_t from = Mark();
  ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, "shop.order", v2));
  std::vector<std::string> want = {
      "ALTER TABLE \"t1_g0\" ADD COLUMN \"v5\" TEXT",
      "ALTER TABLE \"t1_c0\" ADD COLUMN \"v5\" TEXT"};
  EXPECT_EQ(want, DdlSince(*rig.engine, from));
  /* Indexed, inside the nested group: the column and its index. */
  std::vector<F> v3 = v2;
  v3[6].kids[3].kids.push_back(Field("code", T, IDX));
  from = Mark();
  ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, "shop.order", v3));
  want = {"ALTER TABLE \"t1_g3\" ADD COLUMN \"v2\" TEXT",
      "ALTER TABLE \"t1_c3\" ADD COLUMN \"v2\" TEXT",
      "CREATE INDEX IF NOT EXISTS \"t1_i3_2\" ON \"t1_c3\" (\"v2\", \"lang\", \"ent\")"};
  EXPECT_EQ(want, DdlSince(*rig.engine, from));
  /* Retiring a field and indexing another that was not runs only the index:
   * the retired field's column stays. */
  std::vector<F> v4 = v3;
  v4.erase(v4.begin() + 1);
  v4[1].flags |= IDX;
  from = Mark();
  ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, "shop.order", v4));
  want = {"CREATE INDEX IF NOT EXISTS \"t1_i0_2\" ON \"t1_c0\" (\"v2\", \"lang\", \"ent\")"};
  EXPECT_EQ(want, DdlSince(*rig.engine, from));
  /* Unindexing writes nothing: an index is never dropped. */
  std::vector<F> v5 = v4;
  v5[1].flags &= ~IDX;
  from = Mark();
  ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, "shop.order", v5));
  EXPECT_EQ(std::vector<std::string>(), DdlSince(*rig.engine, from));
}

/* Random evolutions of a declaration: every DDL statement is one of the three
 * additive forms, and no record of a step is ever changed or removed. */
TEST_F(Schema, EveryStepIsAdditive) {
  Open(sqlstub::StdDialect());
  std::mt19937 rng(20261017);
  std::vector<F> decl = {Field("a", T)};
  int added = 0;
  int retired = 0;
  int indexed = 0;
  int unindexed = 0;
  std::vector<std::vector<sqlstub::Val>> names_before;
  /* One of the three additive forms, by the strict engine's own parser. */
  auto additive = [](const std::string & ddl) {
    try {
      sqlstub::Lexed lx = sqlstub::Lex(sqlstub::StdDialect(), ddl);
      sqlstub::Stmt st = sqlstub::Parser(lx, "").Statement();
      return st.k == sqlstub::Stmt::CREATE_TABLE ||
             st.k == sqlstub::Stmt::CREATE_INDEX ||
             st.k == sqlstub::Stmt::ADD_COLUMN;
    } catch (const std::exception &) {
      return false;
    }
  };
  int name_counter = 0;
  for (int step = 0; step < 120; step++) {
    std::vector<F> next = decl;
    int what = (int)(rng() % 5);
    std::vector<F> * at = &next;
    if (rng() % 2 == 0) {
      for (auto & f : next) {
        if (f.type == GDEFSTOR_FIELD_GROUP) {
          at = &f.kids;
        }
      }
    }
    if (what == 0 || at->size() < 2) {
      static const int types[] = {T, I, D, FL, DT, R};
      F f = Field("f" + std::to_string(name_counter++), types[rng() % 6]);
      if (rng() % 4 == 0) {
        f.flags |= MUL;
      }
      if (rng() % 4 == 0) {
        f.flags |= IDX;
      }
      if (rng() % 8 == 0 && (f.flags & MUL) == 0) {
        f.flags |= PER;
      }
      if (rng() % 6 == 0) {
        f = Group("g" + std::to_string(name_counter++), rng() % 2 ? MUL : 0,
            {Field("k" + std::to_string(name_counter++), I, IDX)});
      }
      at->push_back(f);
      added++;
    } else if (what == 1) {
      at->erase(at->begin() + (long)(rng() % at->size()));
      retired++;
    } else {
      F & f = (*at)[rng() % at->size()];
      if (f.type != GDEFSTOR_FIELD_GROUP && (f.flags & PER) == 0) {
        f.flags ^= IDX;
        ((f.flags & IDX) ? indexed : unindexed)++;
      }
    }
    size_t from = Mark();
    GDEFSTOR_Result rc = Declare(rig.store, "shop.order", next);
    /* A retired field cannot come back under its name with another shape;
     * the port refuses those, and this sweep does not make them. */
    ASSERT_EQ(GDEFSTOR_OK, rc) << "step " << step;
    decl = next;
    for (const auto & ddl : DdlSince(*rig.engine, from)) {
      EXPECT_TRUE(additive(ddl)) << ddl;
    }
    std::vector<std::vector<sqlstub::Val>> now =
        rig.engine->stores[kTenant].committed.tables["dss_name"]->rows;
    for (const auto & row : names_before) {
      EXPECT_NE(now.end(), std::find(now.begin(), now.end(), row))
          << "a step's record changed or went away at step " << step;
    }
    names_before = now;
  }
  EXPECT_GT(added, 20);
  EXPECT_GT(retired, 10);
  EXPECT_GT(indexed, 10);
  EXPECT_GT(unindexed, 5);
}

/* Type and field names at every edge of the port's validators never reach SQL
 * text, as an identifier or otherwise: tables and columns are numbers. */
TEST_F(Schema, HostileNamesNeverReachSqlText) {
  Open(sqlstub::FakeDialect(), true);
  const std::string longest(64, 'Q');
  const std::string type1 = "Robert_DROP_TABLE." + std::string(128 - 18, 'X');
  const std::string type2 = "M.SELECT-FROM-x";
  std::vector<F> fields = {Field(longest, T, IDX),
      Field("SELECT", I, MUL | IDX), Field("Zx_dss_type", T),
      Group("WHERE_1_EQ_1", MUL, {Field("OR_rid", I, IDX),
          Field("UNION", R)})};
  ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, type1, fields));
  ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, type2, {Field("Ref_To", R, IDX)}));
  Node n;
  n.Set(longest, Txt("v")).Set("Zx_dss_type", Txt("w")).Item("SELECT",
      Num(I, 3));
  n.Add("WHERE_1_EQ_1").Set("OR_rid", Num(I, 9)).Set("UNION", Ref("Pid_2"));
  ASSERT_EQ(GDEFSTOR_OK, Put(rig.store, type1, "Pid_1", n, 0, true));
  Node m;
  m.Set("Ref_To", Ref("Pid_1"));
  ASSERT_EQ(GDEFSTOR_OK, Put(rig.store, type2, "Pid_2", m, 0, true));
  /* A query naming every field, sorted on one, with a relation. */
  std::string path = "WHERE_1_EQ_1.OR_rid";
  GDEFSTOR_Value nine = ValueOf(Num(I, 9));
  GDEFSTOR_QueryNode leaf;
  std::memset(&leaf, 0, sizeof leaf);
  leaf.op = GDEFSTOR_QUERY_EQ;
  leaf.left.kind = GDEFSTOR_QUERY_FIELD;
  leaf.left.field = path.data();
  leaf.left.field_len = path.size();
  leaf.right.kind = GDEFSTOR_QUERY_VALUES;
  leaf.right.values = &nine;
  leaf.right.value_count = 1;
  GDEFSTOR_QueryNode rel;
  std::memset(&rel, 0, sizeof rel);
  rel.op = GDEFSTOR_QUERY_EQ;
  rel.left.kind = GDEFSTOR_QUERY_FIELD;
  rel.left.field = "Ref_To";
  rel.left.field_len = 6;
  rel.right.kind = GDEFSTOR_QUERY_ENTITY;
  rel.right.flags = GDEFSTOR_QUERY_OUTER;
  const GDEFSTOR_QueryNode * relk[] = {&rel};
  GDEFSTOR_QueryNode ex;
  std::memset(&ex, 0, sizeof ex);
  ex.op = GDEFSTOR_QUERY_EXISTS;
  ex.children = relk;
  ex.child_count = 1;
  ex.exists_type = type2.data();
  ex.exists_type_len = type2.size();
  const GDEFSTOR_QueryNode * kids[] = {&leaf, &ex};
  GDEFSTOR_QueryNode all;
  std::memset(&all, 0, sizeof all);
  all.op = GDEFSTOR_QUERY_AND;
  all.children = kids;
  all.child_count = 2;
  GDEFSTOR_QuerySort sort;
  std::memset(&sort, 0, sizeof sort);
  sort.key.kind = GDEFSTOR_QUERY_FIELD;
  sort.key.field = longest.data();
  sort.key.field_len = longest.size();
  GDEFSTOR_Query q;
  std::memset(&q, 0, sizeof q);
  q.size = sizeof q;
  q.language = "en";
  q.language_len = 2;
  q.where = &all;
  q.sorts = &sort;
  q.sort_count = 1;
  std::vector<std::string> ids;
  uint64_t total = 0;
  ASSERT_EQ(GDEFSTOR_OK, Ask(rig.store, type1, q, &ids, &total));
  EXPECT_EQ(std::vector<std::string>{"Pid_1"}, ids);
  EXPECT_EQ(0, rig.tee.disagreements);
  const std::vector<std::string> caller = {longest, "SELECT", "Zx_dss_type",
      "WHERE_1_EQ_1", "OR_rid", "UNION", "Ref_To", "Robert", "M.SELECT", type1,
      type2, path, "Pid_1", "Pid_2"};
  size_t seen = 0;
  for (const auto & e : rig.engine->log) {
    for (const auto & c : caller) {
      /* "SELECT" is also a keyword: the field is never one, but the keyword
       * is fine; look for it as a quoted identifier instead. */
      std::string needle = c == "SELECT" || c == "UNION"
                               ? std::string("[") + c + "]"
                               : c;
      EXPECT_EQ(std::string::npos, e.text.find(needle))
          << "caller text '" << c << "' in: " << e.text;
    }
    /* Every identifier is a generated name within the dialect's limit. */
    for (size_t at = e.text.find('['); at != std::string::npos;
         at = e.text.find('[', at + 1)) {
      size_t end = e.text.find(']', at);
      ASSERT_NE(std::string::npos, end) << e.text;
      std::string id = e.text.substr(at + 1, end - at - 1);
      EXPECT_LE(id.size(), 63u) << id;
      EXPECT_TRUE(sqlstub::KnownIdent(id)) << id;
    }
    seen++;
  }
  EXPECT_GT(seen, 40u);
}

TEST_F(Schema, AColumnLimitIsRefusedBeforeAnyStep) {
  GDEFSSQL_Dialect d = sqlstub::StdDialect();
  d.max_columns = 16;
  Open(d);
  /* Seven fixed columns of a group table (ent, lang, item, vfrom, vto,
   * parent, ord) and ten values is 17, one past the limit. */
  std::vector<F> wide;
  for (int i = 0; i < 10; i++) {
    wide.push_back(Field("c" + std::to_string(i), I));
  }
  size_t from = Mark();
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT, Declare(rig.store, "shop.wide", wide));
  for (const auto & s : DdlSince(*rig.engine, from)) {
    EXPECT_EQ(std::string::npos, s.find("t1_")) << s;
  }
  wide.pop_back();
  EXPECT_EQ(GDEFSTOR_OK, Declare(rig.store, "shop.wide", wide));
}

/* The same declaration under the fake dialect: every difference is one the
 * dialect names, and nothing else differs. */
TEST_F(Schema, AnotherDialectWritesTheSameSchemaInItsOwnWords) {
  Open(sqlstub::StdDialect());
  size_t from = Mark();
  ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, "shop.order", Order()));
  std::vector<std::string> std_ddl = DdlSince(*rig.engine, from);
  sqlstub::Rig other;
  ASSERT_EQ(GDEFSTOR_OK, sqlstub::OpenRig(other, sqlstub::FakeDialect(), true,
      nullptr));
  ASSERT_EQ(GDEFSTOR_OK, Declare(other.store, "shop.order", Order()));
  std::vector<std::string> fake_ddl = DdlSince(*other.engine, 0);
  /* Undo the fake dialect's words: its quotes, its schema, its types. */
  GDEFSSQL_Dialect s = sqlstub::StdDialect();
  GDEFSSQL_Dialect f = sqlstub::FakeDialect();
  auto normal = [&](std::string t) {
    auto swap = [&](const std::string & a, const std::string & b) {
      size_t at = 0;
      while ((at = t.find(a, at)) != std::string::npos) {
        t.replace(at, a.size(), b);
        at += b.size();
      }
    };
    swap("[s1].", "");
    swap("CONCURRENTLY ", "");
    swap("[", "\"");
    swap("]", "\"");
    swap(" COLLATE \"C\"", "");
    for (int k = 0; k < GDEFSSQL_COL_COUNT; k++) {
      swap(std::string(" ") + f.type_names[k], std::string(" ") + s.type_names[k]);
    }
    return t;
  };
  std::vector<std::string> raw;
  std::vector<std::string> mapped;
  for (const auto & t : fake_ddl) {
    if (t.find("dss_") == std::string::npos &&
        t.rfind("CREATE SCHEMA ", 0) != 0) {
      raw.push_back(t);
      mapped.push_back(normal(t));
    }
  }
  std::vector<std::string> std_types;
  for (const auto & t : std_ddl) {
    if (t.find("dss_") == std::string::npos) {
      std_types.push_back(t);
    }
  }
  EXPECT_EQ(std_types, mapped);
  ASSERT_EQ(std_types.size(), raw.size());
  for (size_t i = 0; i < raw.size(); i++) {
    EXPECT_NE(std_types[i], raw[i]) << "the dialect changed nothing in " << raw[i];
  }
  gdefstor_store_destroy(other.store);
}

TEST_F(Schema, AStoreOfAnotherLayoutIsRefused) {
  Open(sqlstub::StdDialect());
  ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, "shop.order", Order()));
  /* What a newer binary would have written. */
  auto & rows = rig.engine->stores[kTenant].committed.tables["dss_schema"]->rows;
  ASSERT_EQ(1u, rows.size());
  rows[0][1] = sqlstub::Val::Int(GDEFSSQL_SCHEMA_VERSION + 1);
  /* A backend that has not seen this tenant reads the version first. */
  GDEFSSQL_Dialect d = sqlstub::StdDialect();
  GDEFSSQL_Executor x = sqlstub::ExecutorOf(rig.engine.get());
  GDEFSTOR_ModelBackend fresh;
  ASSERT_EQ(GDEFSSQL_OK, gdefssql_model_backend(&d, &x, nullptr, &fresh));
  void * txn = nullptr;
  EXPECT_EQ(GDEFSTOR_ERR_UNSUPPORTED, fresh.begin(fresh.ctx, kTenant, 2, &txn));
  rows[0][1] = sqlstub::Val::Int(0);
  EXPECT_EQ(GDEFSTOR_ERR_FORMAT, fresh.begin(fresh.ctx, kTenant, 2, &txn));
  rows[0][1] = sqlstub::Val::Int(GDEFSSQL_SCHEMA_VERSION);
  ASSERT_EQ(GDEFSTOR_OK, fresh.begin(fresh.ctx, kTenant, 2, &txn));
  EXPECT_EQ(GDEFSTOR_OK, fresh.abort(fresh.ctx, txn));
  fresh.destroy(fresh.ctx);
  EXPECT_EQ(0, rig.engine->open_conns);
}

/* ---- index builds (AD-15) ------------------------------------------------ */

class Index : public Fixture {
 protected:
  int StateOf(const char * type, const char * path) {
    int st = -1;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_type_index_state(rig.store, kTenant, 2,
        type, std::strlen(type), path, std::strlen(path), &st));
    return st;
  }
  /* bucket = b: the public ids, in order. */
  std::vector<std::string> Bucket(const char * b) {
    V vb = Txt(b); /* ValueOf borrows its bytes: kept alive here */
    GDEFSTOR_Value v = ValueOf(vb);
    GDEFSTOR_QueryNode leaf;
    std::memset(&leaf, 0, sizeof leaf);
    leaf.op = GDEFSTOR_QUERY_EQ;
    leaf.left.kind = GDEFSTOR_QUERY_FIELD;
    leaf.left.field = "bucket";
    leaf.left.field_len = 6;
    leaf.right.kind = GDEFSTOR_QUERY_VALUES;
    leaf.right.values = &v;
    leaf.right.value_count = 1;
    GDEFSTOR_Query q;
    std::memset(&q, 0, sizeof q);
    q.size = sizeof q;
    q.language = "en";
    q.language_len = 2;
    q.where = &leaf;
    q.limit = 100;
    std::vector<std::string> ids;
    uint64_t total = 0;
    EXPECT_EQ(GDEFSTOR_OK, Ask(rig.store, "shop.box", q, &ids, &total));
    EXPECT_EQ(ids.size(), total);
    return ids;
  }
  void Fill3() {
    for (int i = 0; i < 6; i++) {
      Node n;
      n.Set("bucket", Txt(i % 2 ? "odd" : "even")).Set("n", Num(I, i));
      ASSERT_EQ(GDEFSTOR_OK, Put(rig.store, "shop.box", "b" + std::to_string(i),
          n, 0, true));
    }
  }
  size_t Builds(size_t from) const {
    size_t n = 0;
    for (size_t i = from; i < rig.engine->log.size(); i++) {
      n += IsIndexBuild(rig.engine->log[i].text);
    }
    return n;
  }
};

/* AD-15: a schema step's transaction never holds CREATE INDEX. Every index a
 * declaration needs, a new type's and a new field's, and the tenant tables'
 * own, is built outside every transaction, in both dialects; the engine
 * refuses one inside (and counts it), so this also holds for every other
 * test that declares a type. */
TEST_F(Index, NoIndexIsBuiltInsideAStepTransaction) {
  for (int fake = 0; fake < 2; fake++) {
    sqlstub::Rig r;
    ASSERT_EQ(GDEFSTOR_OK, sqlstub::OpenRig(r, fake ? sqlstub::FakeDialect()
        : sqlstub::StdDialect(), fake != 0, nullptr));
    ASSERT_EQ(GDEFSTOR_OK, Declare(r.store, "shop.order", Order()));
    std::vector<F> more = Order();
    more[1].flags |= IDX;
    more.push_back(Field("note", T, IDX));
    ASSERT_EQ(GDEFSTOR_OK, Declare(r.store, "shop.order", more));
    size_t builds = 0;
    for (const auto & l : r.engine->log) {
      if (IsIndexBuild(l.text)) {
        builds++;
        EXPECT_FALSE(l.in_txn) << l.text;
        EXPECT_EQ(fake != 0, l.text.find(" CONCURRENTLY ") != std::string::npos)
            << l.text;
      }
    }
    /* dss_type_tno, dss_entity_rid, t1p, number, lines.sku, email's
     * subject, the blind table's two (email is personal), then total and
     * note. */
    EXPECT_EQ(10u, builds) << fake;
    EXPECT_EQ(0, r.engine->index_builds_in_txn);
    EXPECT_EQ(10u, r.engine->stores[kTenant].committed.indexes.size());
    gdefstor_store_destroy(r.store);
  }
}

/* A field reads "indexing" from its declaration until its build is done; a
 * concurrent build that fails leaves an invalid index, which the next commit
 * drops and builds again; queries answer the same throughout. */
TEST_F(Index, AFailedConcurrentBuildIsDroppedAndBuiltAgain) {
  Open(sqlstub::FakeDialect(), true);
  ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, "shop.box",
      {Field("bucket", T), Field("n", I)}));
  Fill3();
  const std::vector<std::string> even = {"b0", "b2", "b4"};
  EXPECT_EQ(even, Bucket("even"));
  EXPECT_EQ(GDEFSTOR_INDEX_NONE, StateOf("shop.box", "bucket"));
  long discarded = rig.engine->discarded;
  rig.engine->fail_index_build = 1;
  ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, "shop.box",
      {Field("bucket", T, IDX), Field("n", I)}));
  EXPECT_EQ(0, rig.engine->fail_index_build) << "no build was tried";
  auto & ix = rig.engine->stores[kTenant].committed.indexes;
  ASSERT_EQ(1u, ix.count("t1_i0_0"));
  EXPECT_FALSE(ix["t1_i0_0"].valid);
  EXPECT_GT(rig.engine->discarded, discarded) << "the failed build's connection was kept";
  EXPECT_EQ(GDEFSTOR_INDEX_INDEXING, StateOf("shop.box", "bucket"));
  EXPECT_EQ(even, Bucket("even"));
  /* The next commit of the tenant, whatever it writes. */
  Node n;
  n.Set("bucket", Txt("even")).Set("n", Num(I, 6));
  ASSERT_EQ(GDEFSTOR_OK, Put(rig.store, "shop.box", "b6", n, 0, true));
  EXPECT_EQ(1, rig.engine->invalid_drops);
  ASSERT_EQ(1u, ix.count("t1_i0_0"));
  EXPECT_TRUE(ix["t1_i0_0"].valid);
  EXPECT_EQ(GDEFSTOR_INDEX_READY, StateOf("shop.box", "bucket"));
  EXPECT_EQ((std::vector<std::string>{"b0", "b2", "b4", "b6"}), Bucket("even"));
  EXPECT_EQ(0, rig.engine->index_builds_in_txn);
}

/* A plain build (SQLite's) that fails leaves no index: the field reads
 * "indexing" and the next commit builds it. Nothing is dropped. */
TEST_F(Index, AFailedPlainBuildIsBuiltAtTheNextCommit) {
  Open(sqlstub::StdDialect());
  ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, "shop.box",
      {Field("bucket", T), Field("n", I)}));
  Fill3();
  rig.engine->fail_index_build = 1;
  ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, "shop.box",
      {Field("bucket", T, IDX), Field("n", I)}));
  EXPECT_EQ(0u, rig.engine->stores[kTenant].committed.indexes.count("t1_i0_0"));
  EXPECT_EQ(GDEFSTOR_INDEX_INDEXING, StateOf("shop.box", "bucket"));
  EXPECT_EQ((std::vector<std::string>{"b1", "b3", "b5"}), Bucket("odd"));
  ASSERT_EQ(GDEFSTOR_OK, State(rig.store, "b1", 1, 0));
  EXPECT_EQ(1u, rig.engine->stores[kTenant].committed.indexes.count("t1_i0_0"));
  EXPECT_EQ(GDEFSTOR_INDEX_READY, StateOf("shop.box", "bucket"));
  EXPECT_EQ(0, rig.engine->invalid_drops);
}

/* A build left pending when a backend went away is taken up by the next
 * backend's first write of the tenant. */
TEST_F(Index, APendingBuildIsTakenUpByTheNextBackend) {
  Open(sqlstub::FakeDialect(), true);
  ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, "shop.box",
      {Field("bucket", T), Field("n", I)}));
  Fill3();
  rig.engine->fail_index_build = 1;
  ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, "shop.box",
      {Field("bucket", T, IDX), Field("n", I)}));
  EXPECT_EQ(GDEFSTOR_INDEX_INDEXING, StateOf("shop.box", "bucket"));
  GDEFSSQL_Dialect d = sqlstub::FakeDialect();
  GDEFSSQL_Executor x = sqlstub::ExecutorOf(rig.engine.get());
  GDEFSTOR_ModelBackend fresh;
  ASSERT_EQ(GDEFSSQL_OK, gdefssql_model_backend(&d, &x, nullptr, &fresh));
  int st = -1;
  GDEFSTOR_Type * type = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_type_get(rig.store, kTenant, 2, "shop.box",
      8, &type));
  size_t at = 0;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_type_find(type, "bucket", 6, &at));
  ASSERT_EQ(GDEFSTOR_OK, fresh.index_state(fresh.index_ctx, kTenant, 2, type,
      at, &st));
  EXPECT_EQ(GDEFSTOR_INDEX_INDEXING, st);
  size_t from = Mark();
  void * txn = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, fresh.begin(fresh.ctx, kTenant, 2, &txn));
  ASSERT_EQ(GDEFSTOR_OK, fresh.abort(fresh.ctx, txn));
  EXPECT_EQ(1u, Builds(from));
  ASSERT_EQ(GDEFSTOR_OK, fresh.index_state(fresh.index_ctx, kTenant, 2, type,
      at, &st));
  EXPECT_EQ(GDEFSTOR_INDEX_READY, st);
  EXPECT_EQ(1, rig.engine->invalid_drops);
  gdefstor_type_free(type);
  fresh.destroy(fresh.ctx);
  EXPECT_EQ(GDEFSTOR_INDEX_READY, StateOf("shop.box", "bucket"));
}

/* An index the engine holds, valid, whose record still says pending (a
 * backend that stopped between the build and its mark): the next sweep marks
 * it built, and neither drops it nor builds it again. Dropping is for an
 * invalid index only. */
TEST_F(Index, AnIndexAlreadyBuiltIsMarkedAndNotDroppedOrRebuilt) {
  Open(sqlstub::FakeDialect(), true);
  ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, "shop.box",
      {Field("bucket", T, IDX), Field("n", I)}));
  EXPECT_EQ(GDEFSTOR_INDEX_READY, StateOf("shop.box", "bucket"));
  /* What a backend that stopped after the build leaves: the record. */
  auto & rows = rig.engine->stores[kTenant].committed.tables["dss_name"]->rows;
  int reset = 0;
  for (auto & row : rows) {
    if (row[3] == sqlstub::Val::Int(9)) {
      row[5] = sqlstub::Val::Int(0x200);
      reset++;
    }
  }
  ASSERT_EQ(1, reset);
  EXPECT_EQ(GDEFSTOR_INDEX_INDEXING, StateOf("shop.box", "bucket"));
  GDEFSSQL_Dialect d = sqlstub::FakeDialect();
  GDEFSSQL_Executor x = sqlstub::ExecutorOf(rig.engine.get());
  GDEFSTOR_ModelBackend fresh;
  ASSERT_EQ(GDEFSSQL_OK, gdefssql_model_backend(&d, &x, nullptr, &fresh));
  size_t from = Mark();
  void * txn = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, fresh.begin(fresh.ctx, kTenant, 2, &txn));
  ASSERT_EQ(GDEFSTOR_OK, fresh.abort(fresh.ctx, txn));
  fresh.destroy(fresh.ctx);
  EXPECT_EQ(0u, Builds(from));
  EXPECT_EQ(0, rig.engine->invalid_drops);
  for (const auto & ddl : DdlSince(*rig.engine, from)) {
    EXPECT_EQ(std::string::npos, ddl.find("DROP")) << ddl;
  }
  EXPECT_EQ(GDEFSTOR_INDEX_READY, StateOf("shop.box", "bucket"));
}

/* Nothing pending, nothing built: an ordinary write after the declaration
 * runs no build at all, and the tenant tables' indexes are built once. */
TEST_F(Index, AWriteWithNothingPendingBuildsNothing) {
  Open(sqlstub::StdDialect());
  ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, "shop.box",
      {Field("bucket", T, IDX), Field("n", I)}));
  size_t from = Mark();
  Fill3();
  ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, "shop.box",
      {Field("bucket", T, IDX), Field("n", I), Field("x", T)}));
  EXPECT_EQ(0u, Builds(from));
}

/* gdefssql_store_exists(), for an executor whose missing store is a schema:
 * no store before the tenant's first write, a data store after it, and a
 * data store's schema refused as a key store. */
TEST_F(Schema, AnExecutorCanAskWhetherAStoreExists) {
  Open(sqlstub::FakeDialect(), true);
  GDEFSSQL_Dialect d = sqlstub::FakeDialect();
  GDEFSSQL_Executor x = sqlstub::ExecutorOf(rig.engine.get());
  void * conn = nullptr;
  const char * qual = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, x.open(x.ctx, kTenant, 2, 1, &conn, &qual));
  ASSERT_NE(nullptr, qual);
  int exists = 7;
  ASSERT_EQ(GDEFSTOR_OK, gdefssql_store_exists(&d, &x, conn, qual,
      GDEFSSQL_STORE_DATA, &exists));
  EXPECT_EQ(0, exists);
  x.close(x.ctx, conn, 0);
  ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, "shop.box", {Field("n", I)}));
  ASSERT_EQ(GDEFSTOR_OK, x.open(x.ctx, kTenant, 2, 0, &conn, &qual));
  ASSERT_EQ(GDEFSTOR_OK, gdefssql_store_exists(&d, &x, conn, qual,
      GDEFSSQL_STORE_DATA, &exists));
  EXPECT_EQ(1, exists);
  /* A data store's schema is not a key store, nor somewhere to make one. */
  exists = 7;
  EXPECT_EQ(GDEFSTOR_ERR_FORMAT, gdefssql_store_exists(&d, &x, conn, qual,
      GDEFSSQL_STORE_KEYS, &exists));
  EXPECT_EQ(7, exists);
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefssql_store_exists(&d, &x, conn,
      "Bad-Name", GDEFSSQL_STORE_DATA, &exists));
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefssql_store_exists(&d, &x, conn, qual, 9,
      &exists));
  GDEFSSQL_Dialect none = sqlstub::StdDialect();
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefssql_store_exists(&none, &x, conn, qual,
      GDEFSSQL_STORE_DATA, &exists));
  x.close(x.ctx, conn, 0);
  int64_t v = 0;
  ASSERT_EQ(GDEFSTOR_OK, x.open(x.ctx, kTenant, 2, 0, &conn, &qual));
  ASSERT_EQ(GDEFSTOR_OK, x.exec(x.ctx, conn, d.begin_read,
      std::strlen(d.begin_read)));
  EXPECT_EQ(GDEFSTOR_OK, gdefssql_store_version(&d, &x, conn, qual,
      GDEFSSQL_STORE_DATA, &v));
  EXPECT_EQ(GDEFSSQL_SCHEMA_VERSION, v);
  ASSERT_EQ(GDEFSTOR_OK, x.exec(x.ctx, conn, d.commit, std::strlen(d.commit)));
  x.close(x.ctx, conn, 0);
}

class Executor : public Fixture {};

/* An engine error part way through a write: the write fails, nothing it did
 * is visible, and the connection it ran on is not used again. */
TEST_F(Executor, AFailedStatementRollsBackAndDiscardsTheConnection) {
  Open(sqlstub::StdDialect());
  ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, "shop.order", Order()));
  Node n;
  n.Set("number", Txt("A1")).Item("tags", Txt("x"));
  n.Add("lines").Set("sku", Txt("s")).Add("taxes").Set("kind", Txt("vat"));
  ASSERT_EQ(GDEFSTOR_OK, Put(rig.store, "shop.order", "o1", n, 0, true));
  uint8_t * before = nullptr;
  size_t before_len = 0;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_model_dump(rig.store, kTenant, 2,
      GDEFSTOR_MODEL_TRUTH, &before, &before_len));
  int failures = 0;
  for (long k = 1; k < 60; k++) {
    long discarded = rig.engine->discarded;
    rig.engine->fail_in = k;
    Node m = n;
    m.Set("number", Txt("B" + std::to_string(k)));
    m.kids[0].second.Add("taxes").Set("kind", Txt("k"));
    GDEFSTOR_Result rc = Put(rig.store, "shop.order", "o1", m, 1, false);
    bool fired = rig.engine->fail_in == 0;
    rig.engine->fail_in = 0;
    if (!fired) {
      /* The write needs fewer statements than k: done. */
      ASSERT_EQ(GDEFSTOR_OK, rc);
      break;
    }
    failures++;
    EXPECT_NE(GDEFSTOR_OK, rc) << k;
    EXPECT_GT(rig.engine->discarded, discarded) << k;
    EXPECT_EQ(0, rig.engine->open_conns) << k;
    uint8_t * after = nullptr;
    size_t after_len = 0;
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_model_dump(rig.store, kTenant, 2,
        GDEFSTOR_MODEL_TRUTH, &after, &after_len));
    EXPECT_EQ(std::string((char *)before, before_len),
        std::string((char *)after, after_len)) << k;
    gcu_allocator_free(nullptr, after);
  }
  EXPECT_GT(failures, 10);
  gcu_allocator_free(nullptr, before);
}

/* The backend on its own: a write one of whose statements failed is rolled
 * back at commit and reports it, whatever the caller does next. */
TEST_F(Executor, ACommitAfterAFailedStatementCommitsNothing) {
  Open(sqlstub::StdDialect());
  ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, "shop.order", Order()));
  GDEFSTOR_ModelEntity e;
  std::memset(&e, 0, sizeof e);
  e.type = "shop.order";
  e.type_len = 10;
  e.public_id = "x1";
  e.public_id_len = 2;
  e.creator = "";
  void * txn = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, rig.model.begin(rig.model.ctx, kTenant, 2, &txn));
  uint64_t row = 0;
  /* The fifth statement of the insert (after the type number, the counter's
   * read and write, and the entity row): the entity row went in, the rest
   * did not. */
  rig.engine->fail_in = 5;
  EXPECT_NE(GDEFSTOR_OK, rig.model.entity_insert(rig.model.ctx, txn, &e, &row));
  EXPECT_EQ(0, rig.engine->fail_in);
  EXPECT_EQ(GDEFSTOR_ERR_IO, rig.model.commit(rig.model.ctx, txn));
  EXPECT_EQ(0u, rig.engine->stores[kTenant].committed.tables["t1_e"]->rows.size());
  EXPECT_EQ(0, rig.engine->open_conns);
}

/* The test engine's own instrument: what it takes for a generated name. */
TEST(Stub, OnlyGeneratedNamesAreIdentifiers) {
  for (const char * ok : {"t1_e", "t12_n", "t3p", "t2_g0", "t2_c14", "t1_i2_0",
           "t9_j0_4", "v0", "v13s", "a7", "s2", "rid", "dss_name"}) {
    EXPECT_TRUE(sqlstub::KnownIdent(ok)) << ok;
  }
  for (const char * bad : {"", "t", "t1", "t1_", "t1_x", "t1_g", "t1_gx",
           "t1_i2", "t1_i2_", "t1_ex", "v", "vs", "v1x", "a", "title", "QQ",
           "t1p2", "dss_namex", "lines.sku", "SELECT"}) {
    EXPECT_FALSE(sqlstub::KnownIdent(bad)) << bad;
  }
}

TEST(Dialect, CheckRefusesWhatCouldCorruptTheText) {
  GDEFSSQL_Dialect d = sqlstub::StdDialect();
  EXPECT_EQ(GDEFSSQL_OK, gdefssql_dialect_check(&d));
  EXPECT_EQ(GDEFSSQL_OK, gdefssql_dialect_check(&(d = sqlstub::FakeDialect())));
  struct Bad {
    const char * why;
    std::function<void(GDEFSSQL_Dialect &)> edit;
    GDEFSSQL_Result want;
  };
  const Bad bad[] = {
      {"size", [](GDEFSSQL_Dialect & x) { x.size = 1; }, GDEFSSQL_ERR_INVALID},
      {"no quote", [](GDEFSSQL_Dialect & x) { x.quote_open = ""; }, GDEFSSQL_ERR_INVALID},
      {"a letter in a quote", [](GDEFSSQL_Dialect & x) { x.quote_close = "q"; }, GDEFSSQL_ERR_INVALID},
      {"a string quote", [](GDEFSSQL_Dialect & x) { x.quote_open = "'"; }, GDEFSSQL_ERR_INVALID},
      {"a digit placeholder", [](GDEFSSQL_Dialect & x) { x.placeholder = "1"; }, GDEFSSQL_ERR_INVALID},
      {"a word placeholder", [](GDEFSSQL_Dialect & x) { x.placeholder = ":p"; }, GDEFSSQL_ERR_INVALID},
      {"a quote placeholder", [](GDEFSSQL_Dialect & x) { x.placeholder = "\""; }, GDEFSSQL_ERR_INVALID},
      {"no type", [](GDEFSSQL_Dialect & x) { x.type_names[GDEFSSQL_COL_BYTES] = nullptr; }, GDEFSSQL_ERR_INVALID},
      {"no commit", [](GDEFSSQL_Dialect & x) { x.commit = nullptr; }, GDEFSSQL_ERR_INVALID},
      {"upsert", [](GDEFSSQL_Dialect & x) { x.upsert = 9; }, GDEFSSQL_ERR_INVALID},
      {"identifiers", [](GDEFSSQL_Dialect & x) { x.max_identifier = 31; }, GDEFSSQL_ERR_INVALID},
      {"binds", [](GDEFSSQL_Dialect & x) { x.max_binds = 998; }, GDEFSSQL_ERR_INVALID},
      {"columns", [](GDEFSSQL_Dialect & x) { x.max_columns = 15; }, GDEFSSQL_ERR_INVALID},
      {"DDL outside", [](GDEFSSQL_Dialect & x) { x.ddl_transactional = 0; }, GDEFSSQL_ERR_UNSUPPORTED},
      {"a validity query and no drop", [](GDEFSSQL_Dialect & x) { x.index_valid = "SELECT"; x.index_drop = nullptr; }, GDEFSSQL_ERR_INVALID},
      {"an empty drop", [](GDEFSSQL_Dialect & x) { x.index_valid = "SELECT"; x.index_drop = ""; }, GDEFSSQL_ERR_INVALID},
      {"an empty schema statement", [](GDEFSSQL_Dialect & x) { x.qualifier_create = ""; }, GDEFSSQL_ERR_INVALID},
      {"an empty store query", [](GDEFSSQL_Dialect & x) { x.store_exists = ""; }, GDEFSSQL_ERR_INVALID},
      {"a build word that runs into a name", [](GDEFSSQL_Dialect & x) { x.index_build = "concurrently "; }, GDEFSSQL_ERR_INVALID},
  };
  for (const auto & b : bad) {
    GDEFSSQL_Dialect x = sqlstub::StdDialect();
    b.edit(x);
    EXPECT_EQ(b.want, gdefssql_dialect_check(&x)) << b.why;
  }
  EXPECT_EQ(GDEFSSQL_ERR_INVALID, gdefssql_dialect_check(nullptr));
}

/* Story S6a: records, counters and the outbox are four tenant tables, each a
 * step recorded in dss_name (role 16) like every other, made by the tenant's
 * first write and never by a declaration. */
TEST_F(Schema, TheSystemTablesAreRecordedStepsOfTheTenant) {
  Open(sqlstub::StdDialect());
  ASSERT_EQ(GDEFSTOR_OK, InWrite(rig.store, [](GDEFSTOR_Write * w) {
    return gdefstor_record_put(w, "core.m", 6, "k", 1,
        reinterpret_cast<const uint8_t *>("v"), 1);
  }));
  const sqlstub::Db & db = rig.engine->stores[kTenant].committed;
  std::set<std::string> recorded;
  const sqlstub::Table & names = *db.tables.at("dss_name");
  for (const auto & row : names.rows) {
    if (row[(size_t)names.Find("role")].i == 16) {
      EXPECT_EQ(0, row[(size_t)names.Find("tno")].i);
      recorded.insert(row[(size_t)names.Find("name")].s);
    }
  }
  const std::set<std::string> want = {"dss_record", "dss_seq", "dss_outbox",
      "dss_outbox_id"};
  EXPECT_EQ(want, recorded);
  for (const std::string & t : want) {
    EXPECT_EQ(1u, db.tables.count(t)) << t;
  }
  /* A declaration takes no step of these. */
  size_t from = Mark();
  ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, "shop.order", {Field("x", T)}));
  for (const std::string & ddl : DdlSince(*rig.engine, from)) {
    EXPECT_EQ(std::string::npos, ddl.find("\"dss_")) << ddl;
  }
}

/* An abort of a write that took a counter goes back to the write's savepoint,
 * keeps the counter's floor and commits that alone; an abort that took none
 * is a plain rollback. Every write sets the savepoint right after BEGIN. */
TEST_F(Schema, AnAbortKeepsCounterFloorsThroughTheWritesSavepoint) {
  Open(sqlstub::StdDialect());
  ASSERT_EQ(GDEFSTOR_OK, InWrite(rig.store, [](GDEFSTOR_Write * w) {
    return gdefstor_record_put(w, "core.m", 6, "k", 1,
        reinterpret_cast<const uint8_t *>("v"), 1);
  }));
  for (int take = 0; take < 2; take++) {
    size_t from = Mark();
    GDEFSTOR_Write * w = nullptr;
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_begin(rig.store, kTenant, 2, nullptr,
        &w));
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_record_put(w, "core.m", 6, "k", 1,
        reinterpret_cast<const uint8_t *>("x"), 1));
    uint64_t v = 0;
    if (take) {
      ASSERT_EQ(GDEFSTOR_OK, gdefstor_counter_next(w, "core.epoch", 10, &v));
    }
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_abort(w));
    std::vector<std::string> txn;
    for (size_t i = from; i < rig.engine->log.size(); i++) {
      const std::string & t = rig.engine->log[i].text;
      if (t == "BEGIN IMMEDIATE" || t == "COMMIT" || t == "ROLLBACK" ||
          t.rfind("SAVEPOINT", 0) == 0 || t.rfind("ROLLBACK TO", 0) == 0) {
        txn.push_back(t);
      }
    }
    std::vector<std::string> want = {"BEGIN IMMEDIATE", "SAVEPOINT \"dss_w\""};
    if (take) {
      want.push_back("ROLLBACK TO SAVEPOINT \"dss_w\"");
      want.push_back("COMMIT");
    } else {
      want.push_back("ROLLBACK");
    }
    EXPECT_EQ(want, txn) << take;
    const sqlstub::Table & rec = *rig.engine->stores[kTenant].committed.tables.at(
        "dss_record");
    ASSERT_EQ(1u, rec.rows.size());
    EXPECT_EQ("v", rec.rows[0][(size_t)rec.Find("body")].s);
  }
  uint64_t peek = 9;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_counter_peek(rig.store, kTenant, 2,
      "core.epoch", 10, &peek));
  EXPECT_EQ(0u, peek);
  ASSERT_EQ(GDEFSTOR_OK, InWrite(rig.store, [](GDEFSTOR_Write * w) {
    uint64_t v = 0;
    GDEFSTOR_Result rc = gdefstor_counter_next(w, "core.epoch", 10, &v);
    return rc == GDEFSTOR_OK && v != 2 ? GDEFSTOR_ERR_INTERNAL : rc;
  }));
}

TEST(Dialect, ABackendNeedsACompleteExecutor) {
  GDEFSSQL_Dialect d = sqlstub::StdDialect();
  sqlstub::Engine e(d);
  GDEFSSQL_Executor x = sqlstub::ExecutorOf(&e);
  GDEFSTOR_ModelBackend out;
  x.finalize = nullptr;
  EXPECT_EQ(GDEFSSQL_ERR_INVALID, gdefssql_model_backend(&d, &x, nullptr, &out));
  x = sqlstub::ExecutorOf(&e);
  x.size = 0;
  EXPECT_EQ(GDEFSSQL_ERR_INVALID, gdefssql_model_backend(&d, &x, nullptr, &out));
  x = sqlstub::ExecutorOf(&e);
  d.ddl_transactional = 0;
  EXPECT_EQ(GDEFSSQL_ERR_UNSUPPORTED, gdefssql_model_backend(&d, &x, nullptr, &out));
}

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
