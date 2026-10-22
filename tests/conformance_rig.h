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
 * What a test binary needs to run the port's entity conformance suite
 * (model.inc) on this library: ModelOpen() and ModelClose() over the test
 * engine, in the dialect the including file names with
 * GDEFSSQL_CONFORMANCE_DIALECT and GDEFSSQL_CONFORMANCE_QUALIFIED.
 *
 * Every store is tee'd (sql_stub.h): each query the suite asks is answered
 * by the compiled SQL and by gdefstor_model_scan() over the same rows, and a
 * difference fails the test that asked it. Closing a store also fails the
 * test if a connection or a statement was left open.
 */

#ifndef GHOTI_IO_GDEFSSQL_TESTS_CONFORMANCE_RIG_H
#define GHOTI_IO_GDEFSSQL_TESTS_CONFORMANCE_RIG_H

#include <gtest/gtest.h>

#include "sql_stub.h"

#include <map>
#include <mutex>

namespace conformance_rig {

inline std::map<GDEFSTOR_Store *, sqlstub::Rig *> & Rigs() {
  static std::map<GDEFSTOR_Store *, sqlstub::Rig *> rigs;
  return rigs;
}

/* The allocation-failure sweeps open and close stores from several threads at
 * once (model.inc), so the table of open stores is locked. */
inline std::mutex & RigsLock() {
  static std::mutex m;
  return m;
}

}  // namespace conformance_rig

extern "C" GDEFSTOR_Result ModelOpen(GDEFSTOR_Store ** out,
    const GDEFSTOR_Allocator * alloc) {
  sqlstub::Rig * r = new sqlstub::Rig();
  r->tee.on_disagree = [](const std::string & what) {
    ADD_FAILURE() << "the compiled query and the reference scan disagree:\n"
                  << what;
  };
  GDEFSTOR_Result rc = sqlstub::OpenRig(*r, GDEFSSQL_CONFORMANCE_DIALECT,
      GDEFSSQL_CONFORMANCE_QUALIFIED, alloc);
  if (rc != GDEFSTOR_OK) {
    delete r;
    return rc;
  }
  {
    std::lock_guard<std::mutex> hold(conformance_rig::RigsLock());
    conformance_rig::Rigs()[r->store] = r;
  }
  *out = r->store;
  return GDEFSTOR_OK;
}

extern "C" void ModelClose(GDEFSTOR_Store * store) {
  sqlstub::Rig * r = nullptr;
  {
    std::lock_guard<std::mutex> hold(conformance_rig::RigsLock());
    auto it = conformance_rig::Rigs().find(store);
    if (it != conformance_rig::Rigs().end()) {
      r = it->second;
      conformance_rig::Rigs().erase(it);
    }
  }
  gdefstor_store_destroy(store);
  if (r == nullptr) {
    return;
  }
  EXPECT_EQ(0, r->engine->open_conns) << "a connection was left open";
  EXPECT_EQ(0, r->engine->open_stmts) << "a statement was left open";
  delete r;
}

#endif /* GHOTI_IO_GDEFSSQL_TESTS_CONFORMANCE_RIG_H */
