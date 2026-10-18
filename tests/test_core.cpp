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
 * Result codes: every one has a string, and maps to and from the port's
 * result of the same name.
 */

#include <gtest/gtest.h>

#include <ghoti.io/defiant-storage-sql/defiant-storage-sql.h>
#include <ghoti.io/defiant-storage/defiant-storage.h>

namespace {

TEST(Result, EveryResultHasAStringAndThePortsName) {
  for (int r = 0; r < GDEFSSQL_RESULT_COUNT; r++) {
    GDEFSSQL_Result x = (GDEFSSQL_Result)r;
    EXPECT_STRNE("Unknown error", gdefssql_result_string(x)) << r;
    EXPECT_EQ(x, gdefssql_result_from_port(gdefssql_result_to_port(x))) << r;
    EXPECT_STREQ(gdefstor_result_string(gdefssql_result_to_port(x)),
        gdefssql_result_string(x)) << r;
  }
  EXPECT_EQ(GDEFSTOR_ERR_INTERNAL, gdefssql_result_to_port(GDEFSSQL_RESULT_COUNT));
  EXPECT_EQ(GDEFSSQL_ERR_INTERNAL, gdefssql_result_from_port(GDEFSTOR_RESULT_COUNT));
}

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
