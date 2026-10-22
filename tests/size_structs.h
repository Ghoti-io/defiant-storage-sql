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
 * Every public struct of this library that begins with a `size` member, once,
 * with the file that runs it (size_probe.inc, which defiant-storage installs
 * beside its conformance suites). tools/check-size-structs.sh fails, naming
 * the struct, when a header declares one that is missing here.
 */

#ifndef GHOTI_GDEFSSQL_TEST_SIZE_STRUCTS_H
#define GHOTI_GDEFSSQL_TEST_SIZE_STRUCTS_H

#include <ghoti.io/defiant-storage-sql/defiant-storage-sql.h>

#include <gtest/gtest.h>

#include "size_probe.inc"

#define GDEFSSQL_SIZE_STRUCTS(X) \
  X(GDEFSSQL_Dialect, "tests/test_schema.cpp") \
  X(GDEFSSQL_Executor, "tests/test_schema.cpp")

namespace size_structs {

inline const std::vector<Entry> & Sql() {
  static const std::vector<Entry> all = {
#define X(NAME, HOME) {#NAME, HOME, sizeof(NAME)},
      GDEFSSQL_SIZE_STRUCTS(X)
#undef X
  };
  return all;
}

}  // namespace size_structs

#endif
