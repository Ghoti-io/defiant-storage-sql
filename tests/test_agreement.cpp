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
 * The agreement sweep: seeded random data and queries, every one answered by
 * the compiled SQL on the test engine and by gdefstor_model_scan(), the
 * reference engine, over the same rows (the tee of sql_stub.h). Any
 * difference in the page, its order or the total fails. The sweep also
 * requires that it met the cases that tell engines apart: ties, absent sort
 * keys, descending keys, pages past the first, clamped limits, empty and
 * full answers, relations, negations.
 */

#include <gtest/gtest.h>

#include "query_gen.h"

using namespace tu;

namespace {

struct Seen {
  int queries = 0;
  int counts = 0;
  int nonempty = 0;
  int empty = 0;
  int paged = 0;
  int clamped = 0;
  int partial = 0; /* a page shorter than the total */
  int sorted = 0;
  int descending = 0;
  int exists = 0;
  int nots = 0;
  int field_field = 0;
  int outer = 0;
  int other_language = 0; /* answered in "fr", with rows */
  int no_tree = 0;        /* asked in "de", which no entity has a tree in */
};

void Sweep(const GDEFSSQL_Dialect & d, bool qualified, uint32_t seed,
    Seen & seen) {
  sqlstub::Rig rig;
  ASSERT_EQ(GDEFSTOR_OK, sqlstub::OpenRig(rig, d, qualified, nullptr));
  rig.tee.on_disagree = [&](const std::string & what) {
    ADD_FAILURE() << "seed " << seed << ": the compiled query and the "
                  << "reference scan disagree:\n" << what;
  };
  qgen::Gen g(seed);
  g.norders = 10 + g.Pick(20);
  ASSERT_EQ(GDEFSTOR_OK, g.Populate(rig.store));
  for (int n = 0; n < 150; n++) {
    qgen::Gen::Built b;
    const char * type = g.Chance(5) ? "shop.cust" : "shop.order";
    g.Make(b, type);
    std::vector<std::string> ids;
    uint64_t total = 0;
    GDEFSTOR_Result rc = Ask(rig.store, type, b.q, &ids, &total);
    ASSERT_EQ(GDEFSTOR_OK, rc) << "seed " << seed << " query " << n << ": "
                               << gdefstor_result_string(rc);
    uint64_t count = 0;
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_count(rig.store, kTenant, 2, type,
        std::strlen(type), b.q.language, b.q.language_len, b.q.where, &count));
    EXPECT_EQ(total, count) << "seed " << seed << " query " << n;
    seen.queries++;
    seen.counts++;
    (ids.empty() ? seen.empty : seen.nonempty)++;
    seen.paged += b.q.offset > 0 && total > b.q.offset;
    seen.clamped += b.q.limit > GDEFSTOR_QUERY_LIMIT_MAX && total > 0;
    seen.partial += ids.size() < total && !ids.empty();
    seen.sorted += b.q.sort_count > 0 && ids.size() > 2;
    for (size_t k = 0; k < b.q.sort_count; k++) {
      seen.descending += b.q.sorts[k].descending && ids.size() > 2;
    }
    seen.exists += b.exists;
    seen.nots += b.nots;
    seen.field_field += b.field_field;
    seen.outer += b.outer;
    seen.other_language += !ids.empty() && b.q.language[0] == 'f';
    seen.no_tree += b.q.language[0] == 'd';
  }
  EXPECT_EQ(0, rig.tee.disagreements) << rig.tee.first_disagreement;
  EXPECT_EQ(0, rig.tee.refusals);
  /* Each query asked twice (the page and the count), each answered by both. */
  EXPECT_EQ(300, rig.tee.queries);
  gdefstor_store_destroy(rig.store);
}

void Check(const Seen & s) {
  EXPECT_GT(s.nonempty, 300);
  EXPECT_GT(s.empty, 50);
  EXPECT_GT(s.paged, 50);
  EXPECT_GT(s.clamped, 20);
  EXPECT_GT(s.partial, 100);
  EXPECT_GT(s.sorted, 200);
  EXPECT_GT(s.descending, 100);
  EXPECT_GT(s.exists, 50);
  EXPECT_GT(s.nots, 100);
  EXPECT_GT(s.field_field, 20);
  EXPECT_GT(s.outer, 30);
  EXPECT_GT(s.other_language, 100);
  EXPECT_GT(s.no_tree, 100);
}

TEST(Agreement, RandomQueriesMatchTheReferenceScan) {
  Seen s;
  for (uint32_t seed = 1; seed <= 6; seed++) {
    Sweep(sqlstub::StdDialect(), false, seed, s);
  }
  Check(s);
}

/* The fake dialect sorts NULL high, so here the compiler must write the
 * NULLS clauses, and its schema qualifies every table. */
TEST(Agreement, RandomQueriesMatchTheReferenceScanInAnotherDialect) {
  Seen s;
  for (uint32_t seed = 101; seed <= 106; seed++) {
    Sweep(sqlstub::FakeDialect(), true, seed, s);
  }
  Check(s);
}

/* Rebuilding the derived tables from the truth gives the same current rows
 * and the same answers. */
TEST(Agreement, RebuiltCurrentTablesAnswerAlike) {
  sqlstub::Rig rig;
  ASSERT_EQ(GDEFSTOR_OK, sqlstub::OpenRig(rig, sqlstub::StdDialect(), false,
      nullptr));
  qgen::Gen g(77);
  ASSERT_EQ(GDEFSTOR_OK, g.Populate(rig.store));
  uint8_t * before = nullptr;
  size_t blen = 0;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_model_dump(rig.store, kTenant, 2,
      GDEFSTOR_MODEL_CURRENT, &before, &blen));
  /* Wipe the derived tables by hand first, so the rebuild has to make them. */
  auto & tables = rig.engine->stores[kTenant].committed.tables;
  int wiped = 0;
  for (auto & t : tables) {
    /* The derived tables: tN_n and tN_cG. */
    size_t u = t.first.find('_');
    bool derived = t.first[0] == 't' && u != std::string::npos &&
                   u + 1 < t.first.size() &&
                   (t.first.substr(u + 1) == "n" || t.first[u + 1] == 'c');
    if (derived || t.first == "dss_entity") {
      t.second = std::make_shared<sqlstub::Table>(*t.second);
      t.second->rows.clear();
      wiped++;
    }
  }
  EXPECT_GE(wiped, 8);
  ASSERT_EQ(GDEFSTOR_OK, InWrite(rig.store, [](GDEFSTOR_Write * w) {
    return gdefstor_model_rebuild(w);
  }));
  uint8_t * after = nullptr;
  size_t alen = 0;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_model_dump(rig.store, kTenant, 2,
      GDEFSTOR_MODEL_CURRENT, &after, &alen));
  EXPECT_EQ(std::string((char *)before, blen), std::string((char *)after, alen));
  gcu_allocator_free(nullptr, before);
  gcu_allocator_free(nullptr, after);
  for (int n = 0; n < 50; n++) {
    qgen::Gen::Built b;
    g.Make(b, "shop.order");
    std::vector<std::string> ids;
    uint64_t total = 0;
    ASSERT_EQ(GDEFSTOR_OK, Ask(rig.store, "shop.order", b.q, &ids, &total));
  }
  EXPECT_EQ(0, rig.tee.disagreements) << rig.tee.first_disagreement;
  gdefstor_store_destroy(rig.store);
}

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
