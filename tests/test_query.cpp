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
 * The query compiler (compile.h): golden texts for the form, every constant
 * bound, what is refused, the parameter limit, NULL placement per dialect,
 * the tiebreak and the total.
 */

#include <gtest/gtest.h>

#include "query_gen.h"
#include "test_util.h"

#include <random>

using namespace tu;

namespace {

const int I = GDEFSTOR_FIELD_INTEGER;
const int D = GDEFSTOR_FIELD_DECIMAL;

/* A query tree built in the test's memory. */
struct Q {
  std::vector<std::unique_ptr<GDEFSTOR_QueryNode>> nodes;
  std::vector<std::unique_ptr<std::vector<const GDEFSTOR_QueryNode *>>> lists;
  std::vector<std::unique_ptr<std::vector<GDEFSTOR_Value>>> values;
  std::vector<std::unique_ptr<std::string>> strings;
  const char * Str(const std::string & s) {
    strings.push_back(std::make_unique<std::string>(s));
    return strings.back()->c_str();
  }
  GDEFSTOR_QueryOperand FieldOp(const std::string & path, int flags = 0) {
    GDEFSTOR_QueryOperand o;
    std::memset(&o, 0, sizeof o);
    o.kind = GDEFSTOR_QUERY_FIELD;
    o.field = Str(path);
    o.field_len = path.size();
    o.flags = flags;
    return o;
  }
  GDEFSTOR_QueryOperand Vals(const std::vector<V> & vs) {
    values.push_back(std::make_unique<std::vector<GDEFSTOR_Value>>());
    for (const auto & v : vs) {
      strings.push_back(std::make_unique<std::string>(v.s));
      V copy = v;
      GDEFSTOR_Value x = ValueOf(copy);
      x.bytes = (const uint8_t *)strings.back()->data();
      values.back()->push_back(x);
    }
    GDEFSTOR_QueryOperand o;
    std::memset(&o, 0, sizeof o);
    o.kind = GDEFSTOR_QUERY_VALUES;
    o.values = values.back()->data();
    o.value_count = values.back()->size();
    return o;
  }
  GDEFSTOR_QueryOperand MetaOp(int m, int flags = 0) {
    GDEFSTOR_QueryOperand o;
    std::memset(&o, 0, sizeof o);
    o.kind = GDEFSTOR_QUERY_META;
    o.meta = m;
    o.flags = flags;
    return o;
  }
  GDEFSTOR_QueryOperand EntityOp(int flags = 0) {
    GDEFSTOR_QueryOperand o;
    std::memset(&o, 0, sizeof o);
    o.kind = GDEFSTOR_QUERY_ENTITY;
    o.flags = flags;
    return o;
  }
  const GDEFSTOR_QueryNode * Cmp(int op, GDEFSTOR_QueryOperand l,
      GDEFSTOR_QueryOperand r) {
    nodes.push_back(std::make_unique<GDEFSTOR_QueryNode>());
    GDEFSTOR_QueryNode * n = nodes.back().get();
    std::memset(n, 0, sizeof *n);
    n->op = op;
    n->left = l;
    n->right = r;
    return n;
  }
  const GDEFSTOR_QueryNode * Op(int op, std::vector<const GDEFSTOR_QueryNode *> kids,
      const std::string & exists_type = "") {
    nodes.push_back(std::make_unique<GDEFSTOR_QueryNode>());
    GDEFSTOR_QueryNode * n = nodes.back().get();
    std::memset(n, 0, sizeof *n);
    n->op = op;
    lists.push_back(std::make_unique<std::vector<const GDEFSTOR_QueryNode *>>(kids));
    n->children = lists.back()->data();
    n->child_count = kids.size();
    if (!exists_type.empty()) {
      n->exists_type = Str(exists_type);
      n->exists_type_len = exists_type.size();
    }
    return n;
  }
};

struct Rigged : ::testing::Test {
  sqlstub::Rig rig;
  GDEFSSQL_Dialect dialect;
  void Open(const GDEFSSQL_Dialect & d, bool qualified) {
    dialect = d;
    ASSERT_EQ(GDEFSTOR_OK, sqlstub::OpenRig(rig, d, qualified, nullptr));
    ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, "shop.order", qgen::Order()));
    ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, "shop.cust", qgen::Customer()));
  }
  void TearDown() override {
    gdefstor_store_destroy(rig.store);
  }
};

/* What the port hands a backend, built the way the port builds it: the
 * declarations, one per EXISTS depth first, and the clamped page. */
struct Planned {
  GDEFSTOR_Type * type = nullptr;
  std::vector<GDEFSTOR_Type *> exists;
  GDEFSTOR_ModelPlan plan;
  std::vector<uint32_t> tnos;
  ~Planned() {
    gdefstor_type_free(type);
    for (auto * t : exists) {
      gdefstor_type_free(t);
    }
  }
};

void CollectExists(const GDEFSTOR_QueryNode * n, std::vector<std::string> & out) {
  if (n == nullptr) {
    return;
  }
  if (n->op == GDEFSTOR_QUERY_EXISTS) {
    out.push_back(std::string(n->exists_type, n->exists_type_len));
  }
  for (size_t i = 0; i < n->child_count; i++) {
    CollectExists(n->children[i], out);
  }
}

uint32_t TnoOf(const std::string & t) {
  return t == "shop.order" ? 1 : 2;
}

void PlanOf(GDEFSTOR_Store * store, const std::string & type,
    const GDEFSTOR_Query & q, int count_only, Planned & p) {
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_type_get(store, kTenant, 2, type.data(),
      type.size(), &p.type));
  std::vector<std::string> names;
  CollectExists(q.where, names);
  p.tnos.push_back(TnoOf(type));
  for (const auto & n : names) {
    GDEFSTOR_Type * t = nullptr;
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_type_get(store, kTenant, 2, n.data(),
        n.size(), &t));
    p.exists.push_back(t);
    p.tnos.push_back(TnoOf(n));
  }
  std::memset(&p.plan, 0, sizeof p.plan);
  p.plan.alloc = gdefssql_allocator_default();
  p.plan.type = p.type;
  p.plan.query = &q;
  p.plan.exists_types = p.exists.data();
  p.plan.exists_count = p.exists.size();
  p.plan.count_only = count_only;
  p.plan.limit = q.limit == 0 ? UINT64_MAX
                 : (q.limit > GDEFSTOR_QUERY_LIMIT_MAX ? GDEFSTOR_QUERY_LIMIT_MAX
                                                       : q.limit);
}

struct Compiled {
  GDEFSSQL_Result rc = GDEFSSQL_ERR_INTERNAL;
  std::vector<std::string> text;
  std::vector<std::vector<GDEFSSQL_Datum>> binds;
};

Compiled CompileOf(const GDEFSSQL_Dialect & d, const char * qual,
    const Planned & p) {
  Compiled out;
  GDEFSSQL_Compiled * c = nullptr;
  out.rc = gdefssql_query_compile(&d, qual, &p.plan, p.tnos.data(),
      p.tnos.size(), nullptr, &c);
  if (out.rc != GDEFSSQL_OK) {
    return out;
  }
  for (size_t i = 0; i < gdefssql_compiled_count(c); i++) {
    const char * t = nullptr;
    size_t n = 0;
    EXPECT_EQ(GDEFSSQL_OK, gdefssql_compiled_text(c, i, &t, &n));
    out.text.push_back(std::string(t, n));
    out.binds.emplace_back();
    for (size_t k = 0; k < gdefssql_compiled_bind_count(c, i); k++) {
      GDEFSSQL_Datum x;
      EXPECT_EQ(GDEFSSQL_OK, gdefssql_compiled_bind(c, i, k, &x));
      out.binds.back().push_back(x);
    }
  }
  gdefssql_compiled_free(c);
  return out;
}

GDEFSTOR_Query Query(const GDEFSTOR_QueryNode * where,
    const GDEFSTOR_QuerySort * sorts = nullptr, size_t nsorts = 0,
    uint64_t limit = 0, uint64_t offset = 0) {
  GDEFSTOR_Query q;
  std::memset(&q, 0, sizeof q);
  q.size = sizeof q;
  q.where = where;
  q.sorts = sorts;
  q.sort_count = nsorts;
  q.limit = limit;
  q.offset = offset;
  return q;
}

GDEFSTOR_QuerySort Sort(GDEFSTOR_QueryOperand key, int descending) {
  GDEFSTOR_QuerySort s;
  std::memset(&s, 0, sizeof s);
  s.key = key;
  s.descending = descending;
  return s;
}

class Compile : public Rigged {};

/* Every kind of operand and node of the form, in each dialect. The meaning of
 * each piece is argued in compile.c; the agreement sweep checks it. */
const GDEFSTOR_QueryNode * EveryForm(Q & b) {
  return b.Op(GDEFSTOR_QUERY_AND, {
      b.Cmp(GDEFSTOR_QUERY_EQ, b.FieldOp("number"), b.Vals({Txt("A1")})),
      b.Cmp(GDEFSTOR_QUERY_GT, b.FieldOp("lines.taxes.rate"), b.Vals({Num(D, 500)})),
      b.Cmp(GDEFSTOR_QUERY_IN, b.FieldOp("tags"), b.Vals({Txt("x"), Txt("y")})),
      b.Op(GDEFSTOR_QUERY_NOT, {b.Cmp(GDEFSTOR_QUERY_LT, b.FieldOp("placed"),
          b.MetaOp(GDEFSTOR_META_CREATED))}),
      b.Cmp(GDEFSTOR_QUERY_NE, b.EntityOp(), b.Vals({Ref("o2")})),
      b.Cmp(GDEFSTOR_QUERY_EQ, b.MetaOp(GDEFSTOR_META_CREATOR), b.Vals({Ref("u1")})),
      b.Op(GDEFSTOR_QUERY_OR, {}),
      b.Op(GDEFSTOR_QUERY_EXISTS, {b.Op(GDEFSTOR_QUERY_AND, {
          b.Cmp(GDEFSTOR_QUERY_EQ, b.FieldOp("order"), b.EntityOp(GDEFSTOR_QUERY_OUTER)),
          b.Cmp(GDEFSTOR_QUERY_EQ, b.FieldOp("name"), b.FieldOp("lines.sku", GDEFSTOR_QUERY_OUTER))})},
          "shop.cust")});
}

TEST_F(Compile, TheFormHasThisTextInEachDialect) {
  Open(sqlstub::StdDialect(), false);
  Q b;
  GDEFSTOR_QuerySort s[2] = {Sort(b.FieldOp("total"), 1),
      Sort(b.MetaOp(GDEFSTOR_META_CREATOR), 0)};
  GDEFSTOR_Query q = Query(EveryForm(b), s, 2, 10, 5);
  Planned p;
  PlanOf(rig.store, "shop.order", q, 0, p);
  Compiled c = CompileOf(sqlstub::StdDialect(), nullptr, p);
  ASSERT_EQ(GDEFSSQL_OK, c.rc);
  ASSERT_EQ(2u, c.text.size());
  EXPECT_EQ(std::string(
      "SELECT \"a0\".\"rid\", \"a0\".\"pid\" FROM \"t1_e\" \"a0\" JOIN \"t1_n\" \"a1\" ON \"a1"
      "\".\"rid\" = \"a0\".\"rid\" LEFT JOIN \"t1_c0\" \"a2\" ON \"a2\".\"ent\" = \"a0\".\"rid\""
      " WHERE ((\"a0\".\"rid\" IN (SELECT \"a3\".\"ent\" FROM \"t1_c0\" \"a3\" WHERE (\"a3"
      "\".\"v0\" = ?))) AND (\"a0\".\"rid\" IN (SELECT \"a4\".\"ent\" FROM \"t1_c3\" \"a4\" "
      "WHERE (\"a4\".\"v0\" > ?))) AND (\"a0\".\"rid\" IN (SELECT \"a5\".\"ent\" FROM \"t1"
      "_c1\" \"a5\" WHERE \"a5\".\"v0\" IN (?, ?))) AND (NOT (\"a0\".\"rid\" IN (SELECT "
      "\"a6\".\"ent\" FROM \"t1_c0\" \"a6\" WHERE \"a6\".\"v2\" < \"a0\".\"created\"))) AND ("
      "NOT ((\"a0\".\"pid\" = ?))) AND (\"a0\".\"creator\" IS NOT NULL AND (\"a0\".\"cre"
      "ator\" = ?)) AND (NULL IS NOT NULL) AND EXISTS (SELECT NULL FROM \"t2_e\""
      " \"a7\" JOIN \"t2_n\" \"a8\" ON \"a8\".\"rid\" = \"a7\".\"rid\" WHERE ((\"a7\".\"rid\" I"
      "N (SELECT \"a9\".\"ent\" FROM \"t2_c0\" \"a9\" WHERE \"a9\".\"v1\" = \"a0\".\"pid\")) "
      "AND EXISTS (SELECT NULL FROM \"t2_c0\" \"a10\" JOIN \"t1_c2\" \"a11\" ON \"a11\""
      ".\"ent\" = \"a0\".\"rid\" WHERE \"a10\".\"ent\" = \"a7\".\"rid\" AND \"a10\".\"v0\" = \"a"
      "11\".\"v0\")))) ORDER BY \"a2\".\"v1\" DESC, \"a0\".\"creator\", \"a0\".\"rid\" LIMIT"
      " ? OFFSET ?"), c.text[1]);
  Compiled f = CompileOf(sqlstub::FakeDialect(), "s1", p);
  ASSERT_EQ(GDEFSSQL_OK, f.rc);
  EXPECT_EQ(std::string(
      "SELECT [a0].[rid], [a0].[pid] FROM [s1].[t1_e] [a0] JOIN [s1].[t1_n] ["
      "a1] ON [a1].[rid] = [a0].[rid] LEFT JOIN [s1].[t1_c0] [a2] ON [a2].[en"
      "t] = [a0].[rid] WHERE (([a0].[rid] IN (SELECT [a3].[ent] FROM [s1].[t1"
      "_c0] [a3] WHERE ([a3].[v0] = :1))) AND ([a0].[rid] IN (SELECT [a4].[en"
      "t] FROM [s1].[t1_c3] [a4] WHERE ([a4].[v0] > :2))) AND ([a0].[rid] IN "
      "(SELECT [a5].[ent] FROM [s1].[t1_c1] [a5] WHERE [a5].[v0] IN (:3, :4))"
      ") AND (NOT ([a0].[rid] IN (SELECT [a6].[ent] FROM [s1].[t1_c0] [a6] WH"
      "ERE [a6].[v2] < [a0].[created]))) AND (NOT (([a0].[pid] = :5))) AND (["
      "a0].[creator] IS NOT NULL AND ([a0].[creator] = :6)) AND (NULL IS NOT "
      "NULL) AND EXISTS (SELECT NULL FROM [s1].[t2_e] [a7] JOIN [s1].[t2_n] ["
      "a8] ON [a8].[rid] = [a7].[rid] WHERE (([a7].[rid] IN (SELECT [a9].[ent"
      "] FROM [s1].[t2_c0] [a9] WHERE [a9].[v1] = [a0].[pid])) AND EXISTS (SE"
      "LECT NULL FROM [s1].[t2_c0] [a10] JOIN [s1].[t1_c2] [a11] ON [a11].[en"
      "t] = [a0].[rid] WHERE [a10].[ent] = [a7].[rid] AND [a10].[v0] = [a11]."
      "[v0])))) ORDER BY [a2].[v1] DESC NULLS LAST, [a0].[creator] NULLS FIRS"
      "T, [a0].[rid] LIMIT :7 OFFSET :8"), f.text[1]);
  /* The same parameters in the same order: only the words differ. */
  ASSERT_EQ(c.binds[1].size(), f.binds[1].size());
  ASSERT_EQ(8u, c.binds[1].size());
  for (size_t i = 0; i < c.binds[1].size(); i++) {
    EXPECT_EQ(c.binds[1][i].kind, f.binds[1][i].kind);
    EXPECT_EQ(c.binds[1][i].integer, f.binds[1][i].integer);
  }
  EXPECT_EQ(GDEFSSQL_DATUM_TEXT, c.binds[1][0].kind);
  EXPECT_EQ(500, c.binds[1][1].integer);
  EXPECT_EQ(10, c.binds[1][6].integer);
  EXPECT_EQ(5, c.binds[1][7].integer);
  /* The total is the same predicate with no order and no page. */
  std::string page = c.text[1];
  std::string where = page.substr(page.find(" WHERE "), page.find(" ORDER BY ") -
      page.find(" WHERE "));
  EXPECT_EQ("SELECT COUNT(*) FROM \"t1_e\" \"a0\" JOIN \"t1_n\" \"a1\" ON "
            "\"a1\".\"rid\" = \"a0\".\"rid\"" + where, c.text[0]);
  EXPECT_EQ(6u, c.binds[0].size());
}

/* Every constant of a random query is a parameter: the text lexes in the
 * strict vocabulary (no digit, no quote, no word of a value), its
 * placeholders are exactly its parameters, and each constant is one of them. */
TEST_F(Compile, EveryConstantIsABind) {
  Open(sqlstub::StdDialect(), false);
  int checked = 0;
  for (int fake = 0; fake < 2; fake++) {
    GDEFSSQL_Dialect d = fake ? sqlstub::FakeDialect() : sqlstub::StdDialect();
    qgen::Gen g(4242 + (uint32_t)fake);
    g.distinct = true;
    for (int n = 0; n < 400; n++) {
      qgen::Gen::Built b;
      const char * type = g.Chance(4) ? "shop.cust" : "shop.order";
      g.Make(b, type);
      Planned p;
      PlanOf(rig.store, type, b.q, g.Chance(3), p);
      Compiled c = CompileOf(d, fake ? "s1" : nullptr, p);
      ASSERT_EQ(GDEFSSQL_OK, c.rc) << n;
      for (size_t i = 0; i < c.text.size(); i++) {
        sqlstub::Lexed lx;
        try {
          lx = sqlstub::Lex(d, c.text[i]);
        } catch (const std::exception & e) {
          ADD_FAILURE() << e.what() << "\n" << c.text[i];
          continue;
        }
        EXPECT_EQ((size_t)lx.params, c.binds[i].size()) << c.text[i];
        for (const auto & v : b.constants) {
          bool found = false;
          for (const auto & x : c.binds[i]) {
            if (v.type == GDEFSTOR_FIELD_TEXT || v.type == GDEFSTOR_FIELD_REFERENCE) {
              found |= x.kind == GDEFSSQL_DATUM_TEXT &&
                       std::string((const char *)x.bytes, x.len) == v.s;
            } else if (v.type == GDEFSTOR_FIELD_FLOAT) {
              found |= x.kind == GDEFSSQL_DATUM_REAL && x.real == v.r;
            } else {
              found |= x.kind == GDEFSSQL_DATUM_INT && x.integer == v.i;
            }
          }
          EXPECT_TRUE(found) << "a constant that is not a parameter in "
                             << c.text[i];
          if (!v.s.empty()) {
            EXPECT_EQ(std::string::npos, c.text[i].find(v.s)) << c.text[i];
          }
        }
        checked++;
      }
    }
  }
  EXPECT_GT(checked, 1000);
}

TEST_F(Compile, RefusesWhatIsOutsideTheForm) {
  Open(sqlstub::StdDialect(), false);
  struct Case {
    const char * why;
    std::function<void(Q &, GDEFSTOR_Query &)> make;
  };
  const std::vector<Case> cases = {
      {"a projection column", [](Q & b, GDEFSTOR_Query & q) {
         GDEFSTOR_QueryOperand o;
         std::memset(&o, 0, sizeof o);
         o.kind = GDEFSTOR_QUERY_COLUMN;
         o.column = "number";
         o.column_len = 6;
         q.where = b.Cmp(GDEFSTOR_QUERY_EQ, o, b.Vals({Txt("a")}));
       }},
      {"a text constant", [](Q & b, GDEFSTOR_Query & q) {
         GDEFSTOR_QueryOperand o;
         std::memset(&o, 0, sizeof o);
         o.kind = GDEFSTOR_QUERY_CONST;
         q.where = b.Cmp(GDEFSTOR_QUERY_EQ, b.FieldOp("number"), o);
       }},
      {"a select", [](Q & b, GDEFSTOR_Query & q) {
         static GDEFSTOR_QueryOperand sel;
         sel = b.FieldOp("number");
         q.select = &sel;
         q.select_count = 1;
       }},
      {"a numeric tiebreak", [](Q &, GDEFSTOR_Query & q) { q.entity_numeric = 1; }},
      {"a numeric sort", [](Q & b, GDEFSTOR_Query & q) {
         static GDEFSTOR_QuerySort s;
         s = Sort(b.FieldOp("number"), 0);
         s.numeric = 1;
         q.sorts = &s;
         q.sort_count = 1;
       }},
      {"a sort on a multiple", [](Q & b, GDEFSTOR_Query & q) {
         static GDEFSTOR_QuerySort s;
         s = Sort(b.FieldOp("tags"), 0);
         q.sorts = &s;
         q.sort_count = 1;
       }},
      {"a sort inside a group", [](Q & b, GDEFSTOR_Query & q) {
         static GDEFSTOR_QuerySort s;
         s = Sort(b.FieldOp("lines.sku"), 0);
         q.sorts = &s;
         q.sort_count = 1;
       }},
      {"five sort keys", [](Q & b, GDEFSTOR_Query & q) {
         static GDEFSTOR_QuerySort s[5];
         for (auto & x : s) {
           x = Sort(b.EntityOp(), 0);
         }
         q.sorts = s;
         q.sort_count = 5;
       }},
      {"a personal field", [](Q & b, GDEFSTOR_Query & q) {
         q.where = b.Cmp(GDEFSTOR_QUERY_EQ, b.FieldOp("email"), b.Vals({Txt("a")}));
       }},
      {"a group", [](Q & b, GDEFSTOR_Query & q) {
         q.where = b.Cmp(GDEFSTOR_QUERY_EQ, b.FieldOp("lines"), b.Vals({Txt("a")}));
       }},
      {"no such field", [](Q & b, GDEFSTOR_Query & q) {
         q.where = b.Cmp(GDEFSTOR_QUERY_EQ, b.FieldOp("nope"), b.Vals({Txt("a")}));
       }},
      {"an EXISTS in an EXISTS", [](Q & b, GDEFSTOR_Query & q) {
         const GDEFSTOR_QueryNode * in = b.Op(GDEFSTOR_QUERY_EXISTS,
             {b.Op(GDEFSTOR_QUERY_TRUE, {})}, "shop.cust");
         q.where = b.Op(GDEFSTOR_QUERY_EXISTS, {in}, "shop.cust");
       }},
      {"OUTER outside an EXISTS", [](Q & b, GDEFSTOR_Query & q) {
         q.where = b.Cmp(GDEFSTOR_QUERY_EQ, b.FieldOp("number", GDEFSTOR_QUERY_OUTER),
             b.Vals({Txt("a")}));
       }},
      {"an unknown flag", [](Q & b, GDEFSTOR_Query & q) {
         q.where = b.Cmp(GDEFSTOR_QUERY_EQ, b.FieldOp("number", 4), b.Vals({Txt("a")}));
       }},
      {"an unknown node", [](Q & b, GDEFSTOR_Query & q) {
         q.where = b.Cmp(99, b.FieldOp("number"), b.Vals({Txt("a")}));
       }},
      {"an unknown fact", [](Q & b, GDEFSTOR_Query & q) {
         q.where = b.Cmp(GDEFSTOR_QUERY_EQ, b.MetaOp(99), b.Vals({Num(I, 1)}));
       }},
      {"text against an integer", [](Q & b, GDEFSTOR_Query & q) {
         q.where = b.Cmp(GDEFSTOR_QUERY_EQ, b.FieldOp("number"), b.Vals({Num(I, 1)}));
       }},
      {"an order on text", [](Q & b, GDEFSTOR_Query & q) {
         q.where = b.Cmp(GDEFSTOR_QUERY_LT, b.FieldOp("number"), b.Vals({Txt("a")}));
       }},
      {"two scales", [](Q & b, GDEFSTOR_Query & q) {
         q.where = b.Cmp(GDEFSTOR_QUERY_EQ, b.FieldOp("total"),
             b.FieldOp("lines.taxes.rate"));
       }},
      {"IN a field", [](Q & b, GDEFSTOR_Query & q) {
         q.where = b.Cmp(GDEFSTOR_QUERY_IN, b.FieldOp("number"), b.FieldOp("tags"));
       }},
      {"a NOT of two", [](Q & b, GDEFSTOR_Query & q) {
         q.where = b.Op(GDEFSTOR_QUERY_NOT, {b.Op(GDEFSTOR_QUERY_TRUE, {}),
             b.Op(GDEFSTOR_QUERY_TRUE, {})});
       }},
  };
  for (const auto & c : cases) {
    Q b;
    GDEFSTOR_Query q = Query(nullptr);
    c.make(b, q);
    Planned p;
    PlanOf(rig.store, "shop.order", q, 0, p);
    GDEFSSQL_Compiled * out = nullptr;
    GDEFSSQL_Result rc = gdefssql_query_compile(&dialect, nullptr, &p.plan,
        p.tnos.data(), p.tnos.size(), nullptr, &out);
    EXPECT_EQ(GDEFSSQL_ERR_UNSUPPORTED, rc) << c.why;
    EXPECT_EQ(nullptr, out) << c.why;
    gdefssql_compiled_free(out);
  }
  /* And a plan that does not match its own tree. */
  Q b;
  GDEFSTOR_Query q = Query(b.Op(GDEFSTOR_QUERY_EXISTS, {b.Op(GDEFSTOR_QUERY_TRUE,
      {})}, "shop.cust"));
  Planned p;
  PlanOf(rig.store, "shop.order", q, 0, p);
  p.plan.exists_count = 0;
  p.tnos.resize(1);
  GDEFSSQL_Compiled * out = nullptr;
  EXPECT_EQ(GDEFSSQL_ERR_INVALID, gdefssql_query_compile(&dialect, nullptr,
      &p.plan, p.tnos.data(), p.tnos.size(), nullptr, &out));
  EXPECT_EQ(GDEFSSQL_ERR_INVALID, gdefssql_query_compile(&dialect, "Bad",
      &p.plan, p.tnos.data(), p.tnos.size(), nullptr, &out));
}

TEST_F(Compile, MoreParametersThanTheEngineTakesIsALimit) {
  GDEFSSQL_Dialect d = sqlstub::StdDialect();
  d.max_binds = 999;
  Open(d, false);
  std::vector<V> many;
  for (int i = 0; i < 249; i++) {
    many.push_back(Txt("v" + std::to_string(i)));
  }
  Q b;
  std::vector<const GDEFSTOR_QueryNode *> four;
  for (int k = 0; k < 4; k++) {
    four.push_back(b.Cmp(GDEFSTOR_QUERY_IN, b.FieldOp("number"), b.Vals(many)));
  }
  four.push_back(b.Cmp(GDEFSTOR_QUERY_IN, b.FieldOp("number"),
      b.Vals({Txt("a"), Txt("b"), Txt("c")})));
  GDEFSTOR_Query q = Query(b.Op(GDEFSTOR_QUERY_OR, four));
  Planned p;
  PlanOf(rig.store, "shop.order", q, 0, p);
  GDEFSSQL_Compiled * out = nullptr;
  /* The total takes 999 and fits; the page adds its two and does not. */
  EXPECT_EQ(GDEFSSQL_ERR_LIMIT, gdefssql_query_compile(&d, nullptr, &p.plan,
      p.tnos.data(), p.tnos.size(), nullptr, &out));
  p.plan.count_only = 1;
  ASSERT_EQ(GDEFSSQL_OK, gdefssql_query_compile(&d, nullptr, &p.plan,
      p.tnos.data(), p.tnos.size(), nullptr, &out));
  gdefssql_compiled_free(out);
  std::vector<std::string> ids;
  uint64_t total = 0;
  EXPECT_EQ(GDEFSTOR_ERR_LIMIT, Ask(rig.store, "shop.order", q, &ids, &total));
  EXPECT_EQ(0, rig.engine->open_conns);
}

/* No value sorts first ascending and last descending: a dialect whose engine
 * already does that gets no clause, one whose engine does the opposite gets
 * both, and a key that is never NULL gets none. */
TEST_F(Compile, NullsAreWrittenWhereTheEngineDiffers) {
  Open(sqlstub::StdDialect(), false);
  Q b;
  GDEFSTOR_QuerySort s[3] = {Sort(b.FieldOp("total"), 0), Sort(b.FieldOp("placed"), 1),
      Sort(b.EntityOp(), 1)};
  GDEFSTOR_Query q = Query(nullptr, s, 3);
  Planned p;
  PlanOf(rig.store, "shop.order", q, 0, p);
  Compiled c = CompileOf(sqlstub::StdDialect(), nullptr, p);
  ASSERT_EQ(GDEFSSQL_OK, c.rc);
  EXPECT_NE(std::string::npos, c.text[1].find(
      "ORDER BY \"a2\".\"v1\", \"a2\".\"v2\" DESC, \"a0\".\"pid\" DESC, \"a0\".\"rid\" LIMIT"))
      << c.text[1];
  Compiled f = CompileOf(sqlstub::FakeDialect(), nullptr, p);
  ASSERT_EQ(GDEFSSQL_OK, f.rc);
  EXPECT_NE(std::string::npos, f.text[1].find(
      "ORDER BY [a2].[v1] NULLS FIRST, [a2].[v2] DESC NULLS LAST, [a0].[pid] DESC, [a0].[rid] LIMIT"))
      << f.text[1];
}

TEST_F(Compile, TheTiebreakIsTheRowAscending) {
  Open(sqlstub::StdDialect(), false);
  Q b;
  GDEFSTOR_Query plain = Query(nullptr);
  Planned p;
  PlanOf(rig.store, "shop.order", plain, 0, p);
  Compiled c = CompileOf(dialect, nullptr, p);
  ASSERT_EQ(GDEFSSQL_OK, c.rc);
  EXPECT_NE(std::string::npos, c.text[1].find(" ORDER BY \"a0\".\"rid\" LIMIT "))
      << c.text[1];
  GDEFSTOR_QuerySort s = Sort(b.MetaOp(GDEFSTOR_META_PUBLISHED), 1);
  GDEFSTOR_Query sorted = Query(nullptr, &s, 1);
  Planned p2;
  PlanOf(rig.store, "shop.order", sorted, 0, p2);
  c = CompileOf(dialect, nullptr, p2);
  EXPECT_NE(std::string::npos, c.text[1].find(
      " ORDER BY \"a1\".\"pub\" DESC, \"a0\".\"rid\" LIMIT ")) << c.text[1];
  /* Every match, when no limit is asked: the page is all of them. */
  EXPECT_EQ(INT64_MAX, c.binds[1][0].integer);
  EXPECT_EQ(0, c.binds[1][1].integer);
}

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
