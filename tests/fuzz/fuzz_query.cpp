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
 * Fuzz the query compiler: the input seeds a random query over a fixed,
 * seeded data set (query_gen.h), which runs through the port on the test
 * engine with every answer checked against the reference scan, in both test
 * dialects. A disagreement, a refusal of a query in the form, a statement
 * the strict engine rejects, or a leaked connection aborts.
 */

#include "query_gen.h"

#include <cstdlib>

namespace {

struct World {
  sqlstub::Rig rig[2];
  bool ok = false;
  World() {
    for (int i = 0; i < 2; i++) {
      if (sqlstub::OpenRig(rig[i], i ? sqlstub::FakeDialect() : sqlstub::StdDialect(),
              i == 1, nullptr) != GDEFSTOR_OK) {
        return;
      }
      rig[i].engine->keep_log = false;
      qgen::Gen g(5150);
      if (g.Populate(rig[i].store) != GDEFSTOR_OK) {
        return;
      }
    }
    ok = true;
  }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  static World w;
  if (!w.ok) {
    std::abort();
  }
  std::vector<uint32_t> words;
  for (size_t i = 0; i < size; i += 4) {
    uint32_t x = 0;
    for (size_t k = 0; k < 4 && i + k < size; k++) {
      x |= (uint32_t)data[i + k] << (8 * k);
    }
    words.push_back(x);
  }
  std::seed_seq seq(words.begin(), words.end());
  for (int i = 0; i < 2; i++) {
    qgen::Gen g(0);
    g.rng.seed(seq);
    qgen::Gen::Built b;
    const char * type = g.Chance(5) ? "shop.cust" : "shop.order";
    g.Make(b, type);
    std::vector<std::string> ids;
    uint64_t total = 0;
    if (tu::Ask(w.rig[i].store, type, b.q, &ids, &total) != GDEFSTOR_OK ||
        w.rig[i].tee.disagreements != 0 || w.rig[i].engine->open_conns != 0 ||
        w.rig[i].engine->open_stmts != 0) {
      std::abort();
    }
  }
  return 0;
}
