/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Defiant Storage SQL.
 *
 * Ghoti.io Defiant Storage SQL is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Defiant Storage SQL is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file libver.h
 * @stability free
 *
 * Version numbering and the symbol namespace for the Ghoti.io Defiant Storage SQL library.
 *
 * Every exported symbol carries a per-version token so that two versions of
 * this library can be loaded into one process without the dynamic linker
 * binding one caller to the other version's implementation.
 *
 * See CONVENTIONS.md section 4.
 */

#ifndef GHOTI_IO_GDEFSSQL_LIBVER_H
#define GHOTI_IO_GDEFSSQL_LIBVER_H

/**
 * GHOTIIO_DEFSSQL_NAME and GHOTIIO_DEFSSQL_VERSION come from here.  They are generated at
 * build time from the Makefile's BRANCH, so that the token inside every
 * exported symbol is the same one that names the .pc file, the install
 * directory and the shared library.
 */
#include <ghoti.io/defiant-storage-sql/libver_gen.h>

/**
 * Produce the namespaced form of an identifier.
 *
 * @param NAME The identifier to prefix with GHOTIIO_DEFSSQL_NAME.
 */
#define GHOTIIO_DEFSSQL(NAME) GHOTIIO_DEFSSQL_RENAME(GHOTIIO_DEFSSQL_NAME, _##NAME)

/** Helper.  Concatenation needs two levels of expansion. */
#define GHOTIIO_DEFSSQL_RENAME_INNER(a, b) a##b

/** Helper.  Concatenation needs two levels of expansion. */
#define GHOTIIO_DEFSSQL_RENAME(a, b) GHOTIIO_DEFSSQL_RENAME_INNER(a, b)


//-----------------------------------------------------------------------------
// Version
//-----------------------------------------------------------------------------
//
// The numbers come from libver_gen.h, which the Makefile writes from
// MAJOR_VERSION and MINOR_VERSION. Writing them out here instead is correct
// only until someone bumps the Makefile, at which point the soname, the .pc
// Version: and the install directory all move and these do not.

/** This build's major version. */
#define GDEFSSQL_VERSION_MAJOR GHOTIIO_DEFSSQL_VERSION_MAJOR
/** This build's minor version. */
#define GDEFSSQL_VERSION_MINOR GHOTIIO_DEFSSQL_VERSION_MINOR
/** This build's patch version. */
#define GDEFSSQL_VERSION_PATCH GHOTIIO_DEFSSQL_VERSION_PATCH
/** This build's version as a string, e.g. "1.2.3" or "1.2.3-dev". */
#define GDEFSSQL_VERSION_STRING GHOTIIO_DEFSSQL_VERSION

/**
 * Pack a version into one comparable integer, one byte per component.
 *
 * This is libcurl's LIBCURL_VERSION_NUM layout, which is the common spelling
 * across C libraries: 1.2.3 becomes 0x010203, and a plain `<` compares two
 * versions correctly. Every library in the suite uses it, so a consumer
 * checking one checks them all the same way.
 */
#define GDEFSSQL_MAKE_VERSION(major, minor, patch)                                  \
  ((((unsigned)(major)) << 16) | (((unsigned)(minor)) << 8) |                  \
      ((unsigned)(patch)))

/** This build's version, packed. Compare against GDEFSSQL_MAKE_VERSION(1, 2, 3). */
#define GDEFSSQL_VERSION_NUMBER                                                     \
  GDEFSSQL_MAKE_VERSION(GDEFSSQL_VERSION_MAJOR, GDEFSSQL_VERSION_MINOR, GDEFSSQL_VERSION_PATCH)

#endif // GHOTI_IO_GDEFSSQL_LIBVER_H
