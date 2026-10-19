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
 * The blind table as this library lays it out (AD-14; story S5a), read from
 * the test engine's own tables rather than through the port: it is a recorded
 * schema step with its indexes built outside the step, a MAC is in no other
 * table, and each lookup reads its own columns (the history lookup the blind
 * table, the subject lookup the subject columns), never the other's.
 */

#include <gtest/gtest.h>

#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/defiant-storage/defiant-storage.h>

#include "sql_stub.h"
#include "test_util.h"

#include <string>
#include <vector>

using namespace tu;

namespace {

const int T = GDEFSTOR_FIELD_TEXT;
const unsigned PER = GDEFSTOR_FIELD_PERSONAL;
const unsigned MUL = GDEFSTOR_FIELD_MULTIPLE;

std::string Mac(char c) {
  return std::string(GDEFSTOR_BLIND_MAC_BYTES, c);
}

/* Save a member: its name (personal, with MAC @p mac) and a tag. */
GDEFSTOR_Result SaveMember(GDEFSTOR_Store * store, const std::string & pid,
    const std::string & cipher, const std::string & mac, uint64_t base,
    bool create) {
  return InWrite(store, [&](GDEFSTOR_Write * w) {
    GDEFSTOR_Result rc = GDEFSTOR_OK;
    if (create) {
      rc = gdefstor_entity_create(w, "site.member", 11, pid.data(), pid.size(),
          "", 0, 1, nullptr);
    }
    GDEFSTOR_Tree * t = nullptr;
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_tree_create(nullptr, &t);
    }
    /* ValueOf() borrows the V's bytes: the V must outlive the value. */
    V name = Personal(T, cipher, "s1");
    GDEFSTOR_Value v = ValueOf(name);
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_tree_set(t, 0, "name", 4, &v);
    }
    GDEFSTOR_BlindMac m;
    std::memset(&m, 0, sizeof m);
    m.size = sizeof m;
    m.mac = reinterpret_cast<const uint8_t *>(mac.data());
    m.mac_len = mac.size();
    m.key_id = 3;
    m.version = 1;
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_tree_set_blind(t, 0, "name", 4, &m);
    }
    V tag_v = Txt("x");
    GDEFSTOR_Value tag = ValueOf(tag_v);
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_tree_set(t, 0, "tag", 3, &tag);
    }
    GDEFSTOR_RevisionMeta meta = Meta();
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_entity_save(w, pid.data(), pid.size(), base, &meta, t,
          nullptr);
    }
    gdefstor_tree_free(t);
    return rc;
  });
}

class BlindSql : public ::testing::Test {
 protected:
  sqlstub::Rig rig;

  void Open(const GDEFSSQL_Dialect & d, bool qualified) {
    ASSERT_EQ(GDEFSTOR_OK, sqlstub::OpenRig(rig, d, qualified, nullptr));
  }
  void TearDown() override {
    gdefstor_store_destroy(rig.store);
  }

  const sqlstub::Db & Committed() {
    return rig.engine->stores[kTenant].committed;
  }

  /* Rows of dss_name with role @p role for type @p tno. */
  size_t Recorded(int64_t tno, int64_t role) {
    const auto & t = *Committed().tables.at("dss_name");
    size_t n = 0;
    for (const auto & row : t.rows) {
      n += row[0] == sqlstub::Val::Int(tno) && row[3] == sqlstub::Val::Int(role);
    }
    return n;
  }
};

/* The blind table and its two indexes are steps like any other: recorded in
 * dss_name, the table made in the declaring step and the indexes built after
 * it, outside every transaction (AD-15), and only for a type with a personal
 * field. */
TEST_F(BlindSql, TheBlindTableAndItsIndexesAreRecordedSteps) {
  for (int fake = 0; fake < 2; fake++) {
    SCOPED_TRACE(fake ? "fake dialect" : "std dialect");
    if (fake) {
      gdefstor_store_destroy(rig.store);
      rig = sqlstub::Rig();
    }
    Open(fake ? sqlstub::FakeDialect() : sqlstub::StdDialect(), fake != 0);
    ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, "site.plain", {Field("a", T)}));
    ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, "site.member",
        {Field("name", T, PER), Field("tag", T)}));
    EXPECT_EQ(0u, Committed().tables.count("t1_b"));
    EXPECT_EQ(0u, Recorded(1, 13));
    ASSERT_EQ(1u, Committed().tables.count("t2_b"));
    EXPECT_EQ(1u, Recorded(2, 13));
    EXPECT_EQ(1u, Recorded(2, 14));
    EXPECT_EQ(1u, Recorded(2, 15));
    for (const char * ix : {"t2_bm", "t2_bs"}) {
      ASSERT_EQ(1u, Committed().indexes.count(ix)) << ix;
      EXPECT_TRUE(Committed().indexes.at(ix).valid) << ix;
      EXPECT_EQ("t2_b", Committed().indexes.at(ix).table) << ix;
    }
    EXPECT_EQ((std::vector<std::string>{"fld", "mac"}),
        Committed().indexes.at("t2_bm").cols);
    EXPECT_EQ((std::vector<std::string>{"subject"}),
        Committed().indexes.at("t2_bs").cols);
    EXPECT_EQ(0, rig.engine->index_builds_in_txn);
    /* A type that gains its first personal field gains the table then. */
    ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, "site.plain",
        {Field("a", T), Field("b", T, PER | MUL)}));
    EXPECT_EQ(1u, Committed().tables.count("t1_b"));
    EXPECT_EQ(1u, Recorded(1, 13));
    EXPECT_EQ(1u, Committed().indexes.count("t1_bm"));
    EXPECT_EQ(0, rig.engine->index_builds_in_txn);
    for (const auto & l : rig.engine->log) {
      if (l.text.find("CREATE TABLE") == 0 && l.text.find("_b") != std::string::npos) {
        EXPECT_TRUE(l.in_txn) << l.text;
      }
    }
  }
}

/* A MAC is written into the blind table and nowhere else: no cell of any
 * other table, truth or derived, holds its bytes. */
TEST_F(BlindSql, NoMacIsInAnyTableButTheBlindTable) {
  for (int fake = 0; fake < 2; fake++) {
    SCOPED_TRACE(fake ? "fake dialect" : "std dialect");
    if (fake) {
      gdefstor_store_destroy(rig.store);
      rig = sqlstub::Rig();
    }
    Open(fake ? sqlstub::FakeDialect() : sqlstub::StdDialect(), fake != 0);
    ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, "site.member",
        {Field("name", T, PER), Field("tag", T)}));
    ASSERT_EQ(GDEFSTOR_OK, SaveMember(rig.store, "m1", "c1", Mac('A'), 0, true));
    ASSERT_EQ(GDEFSTOR_OK, SaveMember(rig.store, "m1", "c2", Mac('B'), 1, false));
    for (const std::string & mac : {Mac('A'), Mac('B')}) {
      size_t in_blind = 0;
      for (const auto & kv : Committed().tables) {
        for (const auto & row : kv.second->rows) {
          for (const auto & cell : row) {
            if ((cell.k == sqlstub::V_TEXT || cell.k == sqlstub::V_BYTES) &&
                cell.s.find(mac) != std::string::npos) {
              EXPECT_EQ("t1_b", kv.first) << "a MAC in " << kv.first;
              in_blind += kv.first == "t1_b";
            }
          }
        }
      }
      EXPECT_EQ(1u, in_blind);
    }
  }
}

/* The history lookup reads the blind table; the subject lookup reads the
 * subject columns and never the blind table, so neither answers from the
 * other's rows. */
TEST_F(BlindSql, EachLookupReadsItsOwnColumns) {
  Open(sqlstub::StdDialect(), false);
  ASSERT_EQ(GDEFSTOR_OK, Declare(rig.store, "site.member",
      {Field("name", T, PER), Field("tag", T)}));
  ASSERT_EQ(GDEFSTOR_OK, SaveMember(rig.store, "m1", "c1", Mac('A'), 0, true));
  size_t from = rig.engine->log.size();
  GDEFSTOR_SubjectHits * sh = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_subject_lookup(rig.store, kTenant, 2, "s1", 2,
      &sh));
  EXPECT_EQ(1u, gdefstor_subject_hits_count(sh));
  gdefstor_subject_hits_free(sh);
  bool subject_column = false;
  for (size_t i = from; i < rig.engine->log.size(); i++) {
    const std::string & t = rig.engine->log[i].text;
    EXPECT_EQ(std::string::npos, t.find("\"t1_b\"")) << t;
    subject_column |= t.find("\"v0s\" = ?") != std::string::npos;
  }
  EXPECT_TRUE(subject_column);
  from = rig.engine->log.size();
  std::string mac = Mac('A');
  GDEFSTOR_BlindHits * bh = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_blind_history(rig.store, kTenant, 2,
      "site.member", 11, "name", 4, reinterpret_cast<const uint8_t *>(mac.data()),
      mac.size(), &bh));
  EXPECT_EQ(1u, gdefstor_blind_hits_count(bh));
  gdefstor_blind_hits_free(bh);
  bool blind_table = false;
  for (size_t i = from; i < rig.engine->log.size(); i++) {
    const std::string & t = rig.engine->log[i].text;
    EXPECT_EQ(std::string::npos, t.find("\"v0s\"")) << t;
    blind_table |= t.find("FROM \"t1_b\"") != std::string::npos &&
                   t.find("\"mac\" = ?") != std::string::npos;
  }
  EXPECT_TRUE(blind_table);
}

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
