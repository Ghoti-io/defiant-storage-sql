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
 * A SQL engine for tests: it parses and runs exactly the statement forms
 * defiant-storage-sql writes (documentation/sql.md), in either test dialect,
 * so the library is tested end to end with no database. It is deliberately
 * strict and deliberately unhelpful:
 *
 * - every statement is linted first: only the fixed keyword vocabulary,
 *   quoted generated identifiers, the dialect's placeholders and punctuation
 *   may appear. A digit, a quote character or a word outside the vocabulary
 *   is an error, which is how "every value is bound" is enforced on every
 *   statement every test runs;
 * - columns are typed, and a parameter of the wrong kind is an error;
 * - primary keys, unique indexes and NOT NULL are enforced;
 * - DDL other than the additive forms does not parse;
 * - rows are scanned newest first and ties in ORDER BY keep that order, so a
 *   query that relies on an order it did not ask for answers wrongly;
 * - NULL sorts where the dialect says the engine sorts it;
 * - transactions are snapshots; a read sees the last commit;
 * - a connection holds one statement at a time, as executor.h promises;
 * - no CREATE INDEX of a data store runs inside a transaction (AD-15): one
 *   that does is refused and counted. A build outside one goes straight to
 *   the committed store, as a concurrent build does, and an injected failure
 *   of a concurrent build leaves its index behind, invalid, as Postgres does;
 *   an index is dropped only when it is invalid.
 *
 * Every statement run is recorded (text, parameters, tenant) for tests that
 * look at what was sent. Nothing here is the library's: it is the test's
 * instrument, written from the statement forms, not from the library's code.
 */

#ifndef GHOTI_IO_GDEFSSQL_TESTS_SQL_STUB_H
#define GHOTI_IO_GDEFSSQL_TESTS_SQL_STUB_H

#include <ghoti.io/defiant-storage-sql/defiant-storage-sql.h>
#include <ghoti.io/defiant-storage/defiant-storage.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace sqlstub {

/* ---- dialects ------------------------------------------------------------- */

/* SQLite-like: double quotes, '?', NULL lowest, ON CONFLICT, no RETURNING. */
inline GDEFSSQL_Dialect StdDialect() {
  GDEFSSQL_Dialect d;
  std::memset(&d, 0, sizeof d);
  d.size = sizeof d;
  d.name = "std";
  d.quote_open = "\"";
  d.quote_close = "\"";
  d.placeholder = "?";
  d.placeholder_numbered = 0;
  d.type_names[GDEFSSQL_COL_ID] = "INTEGER";
  d.type_names[GDEFSSQL_COL_INTEGER] = "INTEGER";
  d.type_names[GDEFSSQL_COL_DECIMAL] = "INTEGER";
  d.type_names[GDEFSSQL_COL_FLOAT] = "REAL";
  d.type_names[GDEFSSQL_COL_BOOLEAN] = "INTEGER";
  d.type_names[GDEFSSQL_COL_DATE] = "INTEGER";
  d.type_names[GDEFSSQL_COL_DATETIME] = "INTEGER";
  d.type_names[GDEFSSQL_COL_TEXT] = "TEXT";
  d.type_names[GDEFSSQL_COL_REFERENCE] = "TEXT";
  d.type_names[GDEFSSQL_COL_BYTES] = "BLOB";
  d.text_collation = nullptr;
  d.upsert = GDEFSSQL_UPSERT_ON_CONFLICT;
  d.returning = 0;
  d.nulls_low = 1;
  d.ddl_transactional = 1;
  d.max_identifier = 63;
  d.max_binds = 32766;
  d.max_columns = 2000;
  d.begin_read = "BEGIN";
  d.begin_write = "BEGIN IMMEDIATE";
  d.commit = "COMMIT";
  d.rollback = "ROLLBACK";
  return d;
}

/* Deliberately different in everything a dialect may say: brackets,
 * numbered ':N' parameters, its own type names and collation, NULL highest
 * (so the compiler must write NULLS FIRST/LAST), the MySQL upsert, RETURNING,
 * a schema per tenant made by the tenant's first write, and concurrent index
 * builds that can leave an invalid index, as Postgres does. */
inline GDEFSSQL_Dialect FakeDialect() {
  GDEFSSQL_Dialect d = StdDialect();
  d.name = "fake";
  d.quote_open = "[";
  d.quote_close = "]";
  d.placeholder = ":";
  d.placeholder_numbered = 1;
  d.type_names[GDEFSSQL_COL_ID] = "BIGINT";
  d.type_names[GDEFSSQL_COL_INTEGER] = "BIGINT";
  d.type_names[GDEFSSQL_COL_DECIMAL] = "NUMERIC EXACT";
  d.type_names[GDEFSSQL_COL_FLOAT] = "DOUBLE PRECISION";
  d.type_names[GDEFSSQL_COL_BOOLEAN] = "SMALLINT";
  d.type_names[GDEFSSQL_COL_DATE] = "DAYS";
  d.type_names[GDEFSSQL_COL_DATETIME] = "MILLIS";
  d.type_names[GDEFSSQL_COL_TEXT] = "VARCHAR";
  d.type_names[GDEFSSQL_COL_REFERENCE] = "IDENT";
  d.type_names[GDEFSSQL_COL_BYTES] = "BYTEA";
  d.text_collation = "COLLATE \"C\"";
  d.upsert = GDEFSSQL_UPSERT_ON_DUPLICATE_KEY;
  d.returning = 1;
  d.nulls_low = 0;
  d.max_identifier = 63;
  d.max_binds = 65535;
  d.max_columns = 1600;
  d.begin_read = "START TRANSACTION READ ONLY";
  d.begin_write = "START TRANSACTION READ WRITE";
  d.commit = "COMMIT WORK";
  d.rollback = "ROLLBACK WORK";
  d.index_build = "CONCURRENTLY ";
  d.index_valid = "SELECT VALIDITY :1 :2";
  d.index_drop = "DROP INDEX CONCURRENTLY IF EXISTS ";
  d.qualifier_create = "CREATE SCHEMA IF NOT EXISTS ";
  d.store_exists = "SELECT STORE :1 :2";
  return d;
}

/* ---- values --------------------------------------------------------------- */

enum { V_NULL = 0, V_INT, V_REAL, V_TEXT, V_BYTES };

struct Val {
  int k = V_NULL;
  int64_t i = 0;
  double r = 0.0;
  std::string s;
  static Val Int(int64_t v) {
    Val x;
    x.k = V_INT;
    x.i = v;
    return x;
  }
  bool operator==(const Val & o) const {
    return k == o.k && i == o.i && r == o.r && s == o.s;
  }
};

/* Tri-valued truth: 1, 0, or -1 for UNKNOWN. */
inline Val Truth(int t) {
  if (t < 0) {
    return Val();
  }
  return Val::Int(t);
}

inline int TruthOf(const Val & v) {
  if (v.k == V_NULL) {
    return -1;
  }
  if (v.k != V_INT) {
    throw std::runtime_error("a condition that is not a truth value");
  }
  return v.i != 0;
}

/* Compare two non-NULL values; throws on kinds that SQL would not compare. */
inline int Compare(const Val & a, const Val & b) {
  if ((a.k == V_INT || a.k == V_REAL) && (b.k == V_INT || b.k == V_REAL)) {
    if (a.k == V_INT && b.k == V_INT) {
      return a.i < b.i ? -1 : (a.i > b.i ? 1 : 0);
    }
    double x = a.k == V_INT ? (double)a.i : a.r;
    double y = b.k == V_INT ? (double)b.i : b.r;
    return x < y ? -1 : (x > y ? 1 : 0);
  }
  if ((a.k == V_TEXT && b.k == V_TEXT) || (a.k == V_BYTES && b.k == V_BYTES)) {
    /* By bytes: what COLLATE "C" and BINARY both mean. */
    int c = std::memcmp(a.s.data(), b.s.data(), std::min(a.s.size(), b.s.size()));
    if (c != 0) {
      return c < 0 ? -1 : 1;
    }
    return a.s.size() < b.s.size() ? -1 : (a.s.size() > b.s.size() ? 1 : 0);
  }
  throw std::runtime_error("comparison of different kinds");
}

/* ---- tables --------------------------------------------------------------- */

struct Col {
  std::string name;
  int coltype = 0;
  bool notnull = false;
};

struct Index {
  std::string name;
  std::string table;
  std::vector<std::string> cols;
  bool unique = false;
  bool valid = true; /* false: left by a failed concurrent build */
};

struct Table {
  std::string name;
  std::vector<Col> cols;
  std::vector<std::string> key;
  std::vector<std::vector<Val>> rows;
  int Find(const std::string & c) const {
    for (size_t i = 0; i < cols.size(); i++) {
      if (cols[i].name == c) {
        return (int)i;
      }
    }
    return -1;
  }
};

struct Db {
  std::map<std::string, std::shared_ptr<Table>> tables;
  std::map<std::string, Index> indexes;
  bool schema = false; /* CREATE SCHEMA ran for the tenant's qualifier */
};

/* ---- lexing and linting ------------------------------------------------- */

enum { T_END = 0, T_KW, T_ID, T_PARAM, T_PUNCT, T_TYPE };

struct Tok {
  int t = T_END;
  std::string s; /* keyword, identifier, punctuation */
  int n = 0;     /* parameter position (1-based), or TYPE's coltype */
};

inline const std::set<std::string> & Keywords() {
  static const std::set<std::string> k = {"SELECT", "FROM", "WHERE", "AND",
      "OR", "NOT", "NULL", "IS", "IN", "EXISTS", "JOIN", "LEFT", "ON",
      "ORDER", "BY", "ASC", "DESC", "NULLS", "FIRST", "LAST", "LIMIT",
      "OFFSET", "COUNT", "INSERT", "INTO", "VALUES", "UPDATE", "SET",
      "DELETE", "CREATE", "TABLE", "IF", "INDEX", "UNIQUE", "PRIMARY", "KEY",
      "ALTER", "ADD", "COLUMN", "RETURNING", "CONFLICT", "DO", "EXCLUDED",
      "DUPLICATE", "CONCURRENTLY", "SCHEMA", "DROP", "SAVEPOINT", "ROLLBACK",
      "TO", "COALESCE", "NULLIF"};
  return k;
}

/* What a generated identifier looks like: the only names the library may
 * write. A name made from a caller's text cannot match. */
inline bool KnownIdent(const std::string & s) {
  static const std::set<std::string> fixed = {"dss_schema", "dss_counter",
      "dss_type", "dss_name", "dss_entity", "dss_draft", "dss_type_tno",
      "dss_entity_rid", "id", "version", "last_row", "last_type", "last_cseq",
      "name", "tno", "decl", "grp", "col", "role", "kind", "pid", "rid",
      "type", "base", "body", "creator", "created", "rev", "last_item",
      "seq", "pub", "del", "actor", "at", "cseq", "who", "why", "ent", "item",
      "vfrom", "vto", "parent", "ord", "dsk_schema", "dsk_counter",
      "dsk_key", "dsk_ledger", "dsk_ledger_seq", "dsk_sealed", "dsk_secret",
      "last_seq", "subject",
      "secret", "fld", "mac", "kid", "norm",
      /* story S6a */
      "dss_record", "dss_seq", "dss_outbox", "dss_outbox_id", "dss_w", "ns",
      "rkey", "last", "kept", "stream", "fmt", "lease", "fails", "parked",
      /* story S6a2 */
      "lang", "langs",
      /* story S11 */
      "stamp"};
  if (fixed.count(s) != 0) {
    return true;
  }
  /* t<n>_[esrn], t<n>p, t<n>_[gc]<n>, t<n>_[ij]<n>_<n>, t<n>_b[ms], v<n>[s],
   * a<n>, s<n>.
   * Matched by hand: GCC 16's std::regex trips -Wmaybe-uninitialized under
   * the sanitizers. */
  size_t i = 1;
  auto digits = [&]() {
    size_t from = i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      i++;
    }
    return i > from;
  };
  if (s.empty()) {
    return false;
  }
  switch (s[0]) {
  case 'a':
  case 's':
    return digits() && i == s.size();
  case 'v':
    if (!digits()) {
      return false;
    }
    return i == s.size() || (s[i] == 's' && i + 1 == s.size());
  case 't':
    if (!digits() || i == s.size()) {
      return false;
    }
    if (s[i] == 'p') {
      return i + 1 == s.size();
    }
    if (s[i] != '_' || i + 1 >= s.size()) {
      return false;
    }
    i++;
    if (std::strchr("esrn", s[i]) != nullptr) {
      return i + 1 == s.size();
    }
    if (s[i] == 'b') {
      return i + 1 == s.size() ||
             (i + 2 == s.size() && (s[i + 1] == 'm' || s[i + 1] == 's'));
    }
    if (s[i] == 'g' || s[i] == 'c') {
      i++;
      return digits() && i == s.size();
    }
    if (s[i] == 'i' || s[i] == 'j') {
      i++;
      if (!digits() || i >= s.size() || s[i] != '_') {
        return false;
      }
      i++;
      return digits() && i == s.size();
    }
    return false;
  default:
    return false;
  }
}

struct Lexed {
  std::vector<Tok> toks;
  int params = 0;
};

/* Lex @p sql in dialect @p d. Throws, naming the problem, on anything that
 * is not the library's vocabulary. */
inline Lexed Lex(const GDEFSSQL_Dialect & d, const std::string & sql) {
  Lexed out;
  bool ddl = sql.rfind("CREATE TABLE", 0) == 0 || sql.rfind("ALTER TABLE", 0) == 0;
  std::vector<std::pair<std::string, int>> types;
  for (int i = 0; i < GDEFSSQL_COL_COUNT; i++) {
    std::string t = d.type_names[i];
    if ((i == GDEFSSQL_COL_TEXT || i == GDEFSSQL_COL_REFERENCE) &&
        d.text_collation != nullptr && d.text_collation[0] != '\0') {
      t += std::string(" ") + d.text_collation;
    }
    types.push_back({t, i});
  }
  /* Longest first, so "TEXT COLLATE ..." wins over "TEXT". */
  std::sort(types.begin(), types.end(),
      [](const auto & a, const auto & b) { return a.first.size() > b.first.size(); });
  std::string qo = d.quote_open;
  std::string qc = d.quote_close;
  std::string ph = d.placeholder;
  size_t p = 0;
  while (p < sql.size()) {
    char c = sql[p];
    if (c == ' ') {
      p++;
      continue;
    }
    if (ddl && !out.toks.empty() && out.toks.back().t == T_ID) {
      bool matched = false;
      for (const auto & t : types) {
        if (sql.compare(p, t.first.size(), t.first) == 0) {
          size_t e = p + t.first.size();
          if (e == sql.size() || sql[e] == ',' || sql[e] == ')' || sql[e] == ' ') {
            Tok k;
            k.t = T_TYPE;
            k.s = t.first;
            k.n = t.second;
            out.toks.push_back(k);
            p = e;
            matched = true;
            break;
          }
        }
      }
      if (matched) {
        continue;
      }
    }
    if (sql.compare(p, qo.size(), qo) == 0) {
      size_t e = sql.find(qc, p + qo.size());
      if (e == std::string::npos) {
        throw std::runtime_error("unterminated identifier");
      }
      Tok k;
      k.t = T_ID;
      k.s = sql.substr(p + qo.size(), e - p - qo.size());
      if (!KnownIdent(k.s)) {
        throw std::runtime_error("an identifier that is not generated: " + k.s);
      }
      out.toks.push_back(k);
      p = e + qc.size();
      continue;
    }
    if (sql.compare(p, ph.size(), ph) == 0) {
      p += ph.size();
      Tok k;
      k.t = T_PARAM;
      if (d.placeholder_numbered) {
        size_t e = p;
        while (e < sql.size() && sql[e] >= '0' && sql[e] <= '9') {
          e++;
        }
        if (e == p) {
          throw std::runtime_error("a placeholder without its number");
        }
        k.n = std::stoi(sql.substr(p, e - p));
        p = e;
        if (k.n != out.params + 1) {
          throw std::runtime_error("placeholders out of order");
        }
      } else {
        k.n = out.params + 1;
      }
      out.params += 1;
      out.toks.push_back(k);
      continue;
    }
    if (c >= 'A' && c <= 'Z') {
      size_t e = p;
      while (e < sql.size() && sql[e] >= 'A' && sql[e] <= 'Z') {
        e++;
      }
      Tok k;
      k.t = T_KW;
      k.s = sql.substr(p, e - p);
      if (Keywords().count(k.s) == 0) {
        throw std::runtime_error("a word outside the vocabulary: " + k.s);
      }
      out.toks.push_back(k);
      p = e;
      continue;
    }
    static const char * const punct[] = {"<>", "<=", ">=", "(", ")", ",", ".",
        "*", "+", "=", "<", ">"};
    bool hit = false;
    for (const char * q : punct) {
      size_t n = std::strlen(q);
      if (sql.compare(p, n, q) == 0) {
        Tok k;
        k.t = T_PUNCT;
        k.s = q;
        out.toks.push_back(k);
        p += n;
        hit = true;
        break;
      }
    }
    if (!hit) {
      /* A digit, a quote, a lower-case word: a value or a name in the text. */
      throw std::runtime_error(std::string("a stray byte in SQL text: '") + c +
                               "' at " + std::to_string(p) + " in: " + sql);
    }
  }
  Tok e;
  e.t = T_END;
  out.toks.push_back(e);
  return out;
}

/* ---- the syntax tree ------------------------------------------------------ */

struct Select;

struct Expr {
  enum K { COL, PARAM, NUL, COUNT, ADD, CMP, AND, OR, NOT, ISNULL, NOTNULL,
      INLIST, INSUB, EXISTS, VALUESOF, SUBVAL, COALESCE, NULLIF };
  K k = NUL;
  std::string alias;
  std::string col;
  int param = 0;
  std::string op;
  std::vector<std::shared_ptr<Expr>> kids;
  std::shared_ptr<Select> sub;
};
using ExprP = std::shared_ptr<Expr>;

struct From {
  std::string table;
  std::string alias;
  int join = 0; /* 0 first, 1 JOIN, 2 LEFT JOIN */
  ExprP on;
};

struct Order {
  ExprP e;
  bool desc = false;
  int nulls = 0; /* 0 the engine's, 1 FIRST, 2 LAST */
};

struct Select {
  std::vector<ExprP> items;
  std::vector<From> from;
  ExprP where;
  std::vector<Order> order;
  ExprP limit;
  ExprP offset;
};

struct Stmt {
  enum K { CREATE_TABLE, CREATE_INDEX, ADD_COLUMN, INSERT, UPDATE, DELETE,
      SELECT, CREATE_SCHEMA, DROP_INDEX, SAVEPOINT, ROLLBACK_TO };
  K k = SELECT;
  std::string table;
  std::string index;
  bool unique = false;
  bool concurrent = false;
  std::vector<Col> cols;
  std::vector<std::string> names; /* key, index columns, insert columns */
  std::vector<ExprP> values;      /* INSERT VALUES, UPDATE SET */
  std::shared_ptr<Select> select; /* SELECT, INSERT ... SELECT */
  std::vector<std::string> conflict;   /* ON CONFLICT (k) */
  std::vector<std::string> set_cols;   /* upsert / UPDATE targets */
  std::vector<ExprP> set_vals;
  bool upsert = false;
  ExprP where;
  std::string returning;
};

class Parser {
 public:
  Parser(const Lexed & l, const std::string & qual) : t_(l.toks), qual_(qual) {}

  Stmt Statement() {
    Stmt s;
    if (Kw("CREATE")) {
      bool unique = Kw("UNIQUE");
      if (!unique && Kw("SCHEMA")) {
        Want("IF");
        Want("NOT");
        Want("EXISTS");
        s.k = Stmt::CREATE_SCHEMA;
        s.table = Id();
        if (s.table != qual_) {
          throw std::runtime_error("a schema that is not the tenant's: " + s.table);
        }
      } else if (!unique && Kw("TABLE")) {
        Want("IF");
        Want("NOT");
        Want("EXISTS");
        s.k = Stmt::CREATE_TABLE;
        s.table = TableName();
        Punct("(");
        while (true) {
          if (Kw("PRIMARY")) {
            Want("KEY");
            Punct("(");
            s.names = Names();
            Punct(")");
            break;
          }
          Col c;
          c.name = Id();
          c.coltype = Type();
          if (Kw("NOT")) {
            Want("NULL");
            c.notnull = true;
          }
          s.cols.push_back(c);
          Punct(",");
        }
        Punct(")");
      } else {
        Want("INDEX");
        s.concurrent = Kw("CONCURRENTLY");
        Want("IF");
        Want("NOT");
        Want("EXISTS");
        s.k = Stmt::CREATE_INDEX;
        s.unique = unique;
        s.index = Id();
        Want("ON");
        s.table = TableName();
        Punct("(");
        s.names = Names();
        Punct(")");
      }
    } else if (Kw("SAVEPOINT")) {
      s.k = Stmt::SAVEPOINT;
      s.index = Id();
    } else if (Kw("ROLLBACK")) {
      Want("TO");
      Want("SAVEPOINT");
      s.k = Stmt::ROLLBACK_TO;
      s.index = Id();
    } else if (Kw("DROP")) {
      /* Only an index, and the engine allows only an invalid one. */
      Want("INDEX");
      s.concurrent = Kw("CONCURRENTLY");
      Want("IF");
      Want("EXISTS");
      s.k = Stmt::DROP_INDEX;
      s.index = TableName();
    } else if (Kw("ALTER")) {
      Want("TABLE");
      s.k = Stmt::ADD_COLUMN;
      s.table = TableName();
      Want("ADD");
      Want("COLUMN");
      Col c;
      c.name = Id();
      c.coltype = Type();
      s.cols.push_back(c);
    } else if (Kw("INSERT")) {
      Want("INTO");
      s.k = Stmt::INSERT;
      s.table = TableName();
      Punct("(");
      s.names = Names();
      Punct(")");
      if (Kw("VALUES")) {
        Punct("(");
        do {
          s.values.push_back(Operand());
        } while (IsPunct(",") && Next());
        Punct(")");
        if (Kw("ON")) {
          s.upsert = true;
          if (Kw("CONFLICT")) {
            Punct("(");
            s.conflict = Names();
            Punct(")");
            Want("DO");
            Want("UPDATE");
            Want("SET");
            do {
              s.set_cols.push_back(Id());
              Punct("=");
              Want("EXCLUDED");
              Punct(".");
              std::string from = Id();
              if (from != s.set_cols.back()) {
                throw std::runtime_error("upsert sets one column from another");
              }
            } while (IsPunct(",") && Next());
          } else {
            Want("DUPLICATE");
            Want("KEY");
            Want("UPDATE");
            do {
              s.set_cols.push_back(Id());
              Punct("=");
              Want("VALUES");
              Punct("(");
              std::string from = Id();
              Punct(")");
              if (from != s.set_cols.back()) {
                throw std::runtime_error("upsert sets one column from another");
              }
            } while (IsPunct(",") && Next());
          }
        }
      } else {
        s.select = SelectBody();
      }
    } else if (Kw("UPDATE")) {
      s.k = Stmt::UPDATE;
      s.table = TableName();
      Want("SET");
      do {
        s.set_cols.push_back(Id());
        Punct("=");
        ExprP v = Operand();
        if (IsPunct("+")) {
          Next();
          ExprP add = std::make_shared<Expr>();
          add->k = Expr::ADD;
          add->kids = {v, Operand()};
          v = add;
        }
        s.set_vals.push_back(v);
      } while (IsPunct(",") && Next());
      if (Kw("WHERE")) {
        s.where = Cond();
      }
      if (Kw("RETURNING")) {
        s.returning = Id();
      }
    } else if (Kw("DELETE")) {
      Want("FROM");
      s.k = Stmt::DELETE;
      s.table = TableName();
      if (Kw("WHERE")) {
        s.where = Cond();
      }
    } else if (IsKw("SELECT")) {
      s.k = Stmt::SELECT;
      s.select = SelectBody();
    } else {
      throw std::runtime_error("not a statement this engine knows: " + Here());
    }
    if (t_[i_].t != T_END) {
      throw std::runtime_error("text after the statement: " + Here());
    }
    return s;
  }

 private:
  const std::vector<Tok> & t_;
  std::string qual_;
  size_t i_ = 0;

  std::string Here() const {
    return t_[i_].s.empty() ? std::string("(end)") : t_[i_].s;
  }
  bool Next() {
    i_++;
    return true;
  }
  bool IsKw(const char * k) const {
    return t_[i_].t == T_KW && t_[i_].s == k;
  }
  bool Kw(const char * k) {
    if (IsKw(k)) {
      i_++;
      return true;
    }
    return false;
  }
  void Want(const char * k) {
    if (!Kw(k)) {
      throw std::runtime_error(std::string("expected ") + k + " at " + Here());
    }
  }
  bool IsPunct(const char * p) const {
    return t_[i_].t == T_PUNCT && t_[i_].s == p;
  }
  void Punct(const char * p) {
    if (!IsPunct(p)) {
      throw std::runtime_error(std::string("expected ") + p + " at " + Here());
    }
    i_++;
  }
  std::string Id() {
    if (t_[i_].t != T_ID) {
      throw std::runtime_error("expected an identifier at " + Here());
    }
    return t_[i_++].s;
  }
  int Type() {
    if (t_[i_].t != T_TYPE) {
      throw std::runtime_error("expected a dialect type at " + Here());
    }
    return t_[i_++].n;
  }
  /* A table: qualified exactly when the tenant has a qualifier. */
  std::string TableName() {
    std::string a = Id();
    if (IsPunct(".")) {
      Next();
      std::string b = Id();
      if (qual_.empty() || a != qual_) {
        throw std::runtime_error("a table of another schema: " + a);
      }
      return b;
    }
    if (!qual_.empty()) {
      throw std::runtime_error("an unqualified table in a qualified tenant: " + a);
    }
    return a;
  }
  std::vector<std::string> Names() {
    std::vector<std::string> v;
    do {
      v.push_back(Id());
    } while (IsPunct(",") && Next());
    return v;
  }

  ExprP Operand() {
    ExprP e = std::make_shared<Expr>();
    if (t_[i_].t == T_PARAM) {
      e->k = Expr::PARAM;
      e->param = t_[i_++].n;
      return e;
    }
    if (Kw("NULL")) {
      e->k = Expr::NUL;
      return e;
    }
    if (Kw("COUNT")) {
      Punct("(");
      Punct("*");
      Punct(")");
      e->k = Expr::COUNT;
      return e;
    }
    if (Kw("EXCLUDED")) {
      throw std::runtime_error("EXCLUDED outside an upsert");
    }
    if (IsPunct("(") && t_[i_ + 1].t == T_KW && t_[i_ + 1].s == "SELECT") {
      /* A scalar subquery: its first row's first column, or NULL. */
      Next();
      e->k = Expr::SUBVAL;
      e->sub = SelectBody();
      Punct(")");
      return e;
    }
    if (IsKw("COALESCE") || IsKw("NULLIF")) {
      e->k = IsKw("COALESCE") ? Expr::COALESCE : Expr::NULLIF;
      Next();
      Punct("(");
      e->kids.push_back(Operand());
      Punct(",");
      e->kids.push_back(Operand());
      Punct(")");
      return e;
    }
    e->k = Expr::COL;
    e->alias = Id();
    if (IsPunct(".")) {
      Next();
      e->col = Id();
    } else {
      e->col = e->alias;
      e->alias.clear();
    }
    return e;
  }

  std::shared_ptr<Select> SelectBody() {
    auto s = std::make_shared<Select>();
    Want("SELECT");
    do {
      s->items.push_back(Operand());
    } while (IsPunct(",") && Next());
    Want("FROM");
    From f;
    f.table = TableName();
    f.alias = Id();
    s->from.push_back(f);
    while (IsKw("JOIN") || IsKw("LEFT")) {
      From j;
      j.join = Kw("LEFT") ? 2 : 1;
      Want("JOIN");
      j.table = TableName();
      j.alias = Id();
      Want("ON");
      j.on = Cond();
      s->from.push_back(j);
    }
    if (Kw("WHERE")) {
      s->where = Cond();
    }
    if (Kw("ORDER")) {
      Want("BY");
      do {
        Order o;
        o.e = Operand();
        if (Kw("DESC")) {
          o.desc = true;
        } else {
          Kw("ASC");
        }
        if (Kw("NULLS")) {
          o.nulls = Kw("FIRST") ? 1 : (Kw("LAST") ? 2 : 0);
          if (o.nulls == 0) {
            throw std::runtime_error("NULLS without FIRST or LAST");
          }
        }
        s->order.push_back(o);
      } while (IsPunct(",") && Next());
    }
    if (Kw("LIMIT")) {
      s->limit = Operand();
      Want("OFFSET");
      s->offset = Operand();
    }
    return s;
  }

  ExprP Cond() {
    ExprP l = AndCond();
    while (Kw("OR")) {
      ExprP e = std::make_shared<Expr>();
      e->k = Expr::OR;
      e->kids = {l, AndCond()};
      l = e;
    }
    return l;
  }
  ExprP AndCond() {
    ExprP l = NotCond();
    while (Kw("AND")) {
      ExprP e = std::make_shared<Expr>();
      e->k = Expr::AND;
      e->kids = {l, NotCond()};
      l = e;
    }
    return l;
  }
  ExprP NotCond() {
    if (Kw("NOT")) {
      ExprP e = std::make_shared<Expr>();
      e->k = Expr::NOT;
      e->kids = {NotCond()};
      return e;
    }
    return Primary();
  }
  ExprP Primary() {
    if (Kw("EXISTS")) {
      Punct("(");
      ExprP e = std::make_shared<Expr>();
      e->k = Expr::EXISTS;
      e->sub = SelectBody();
      Punct(")");
      return e;
    }
    if (IsPunct("(") &&
        !(t_[i_ + 1].t == T_KW && t_[i_ + 1].s == "SELECT")) {
      Next();
      ExprP e = Cond();
      Punct(")");
      return e;
    }
    ExprP l = Operand();
    if (Kw("IS")) {
      ExprP e = std::make_shared<Expr>();
      e->k = Kw("NOT") ? Expr::NOTNULL : Expr::ISNULL;
      Want("NULL");
      e->kids = {l};
      return e;
    }
    if (Kw("IN")) {
      Punct("(");
      ExprP e = std::make_shared<Expr>();
      e->kids = {l};
      if (IsKw("SELECT")) {
        e->k = Expr::INSUB;
        e->sub = SelectBody();
      } else {
        e->k = Expr::INLIST;
        do {
          e->kids.push_back(Operand());
        } while (IsPunct(",") && Next());
      }
      Punct(")");
      return e;
    }
    static const char * const ops[] = {"=", "<>", "<", "<=", ">", ">="};
    for (const char * op : ops) {
      if (IsPunct(op)) {
        Next();
        ExprP e = std::make_shared<Expr>();
        e->k = Expr::CMP;
        e->op = op;
        e->kids = {l, Operand()};
        return e;
      }
    }
    throw std::runtime_error("expected a condition at " + Here());
  }
};

/* ---- evaluation ----------------------------------------------------------- */

struct Frame {
  std::string alias;
  const Table * t = nullptr;
  const std::vector<Val> * row = nullptr; /* NULL: a LEFT JOIN's missing row */
};

struct Ctx {
  const Db * db;
  const std::vector<Val> * params;
  int nulls_low;
};

inline Val Lookup(const std::vector<Frame> & fr, const Expr & e) {
  for (size_t i = fr.size(); i-- > 0;) {
    if (fr[i].alias == e.alias) {
      int c = fr[i].t->Find(e.col);
      if (c < 0) {
        throw std::runtime_error("no column " + e.col + " in " + fr[i].t->name);
      }
      return fr[i].row == nullptr ? Val() : (*fr[i].row)[(size_t)c];
    }
  }
  throw std::runtime_error("no table aliased " + e.alias);
}

struct Result {
  std::vector<std::vector<Val>> rows;
};

inline Result RunSelect(const Ctx & c, const Select & s, std::vector<Frame> & fr);

inline Val Eval(const Ctx & c, const Expr & e, std::vector<Frame> & fr) {
  switch (e.k) {
  case Expr::COL:
    return Lookup(fr, e);
  case Expr::PARAM:
    return (*c.params)[(size_t)e.param - 1];
  case Expr::NUL:
    return Val();
  case Expr::ADD: {
    Val a = Eval(c, *e.kids[0], fr);
    Val b = Eval(c, *e.kids[1], fr);
    if (a.k != V_INT || b.k != V_INT) {
      throw std::runtime_error("+ of non-integers");
    }
    return Val::Int(a.i + b.i);
  }
  case Expr::CMP: {
    Val a = Eval(c, *e.kids[0], fr);
    Val b = Eval(c, *e.kids[1], fr);
    if (a.k == V_NULL || b.k == V_NULL) {
      return Val();
    }
    int r = Compare(a, b);
    if (e.op == "=") return Truth(r == 0);
    if (e.op == "<>") return Truth(r != 0);
    if (e.op == "<") return Truth(r < 0);
    if (e.op == "<=") return Truth(r <= 0);
    if (e.op == ">") return Truth(r > 0);
    return Truth(r >= 0);
  }
  case Expr::AND: {
    int a = TruthOf(Eval(c, *e.kids[0], fr));
    if (a == 0) {
      return Truth(0);
    }
    int b = TruthOf(Eval(c, *e.kids[1], fr));
    if (b == 0) {
      return Truth(0);
    }
    return Truth(a == 1 && b == 1 ? 1 : -1);
  }
  case Expr::OR: {
    int a = TruthOf(Eval(c, *e.kids[0], fr));
    if (a == 1) {
      return Truth(1);
    }
    int b = TruthOf(Eval(c, *e.kids[1], fr));
    if (b == 1) {
      return Truth(1);
    }
    return Truth(a == 0 && b == 0 ? 0 : -1);
  }
  case Expr::NOT: {
    int a = TruthOf(Eval(c, *e.kids[0], fr));
    return Truth(a < 0 ? -1 : !a);
  }
  case Expr::ISNULL:
    return Truth(Eval(c, *e.kids[0], fr).k == V_NULL);
  case Expr::NOTNULL:
    return Truth(Eval(c, *e.kids[0], fr).k != V_NULL);
  case Expr::INLIST: {
    Val x = Eval(c, *e.kids[0], fr);
    if (x.k == V_NULL) {
      return Val();
    }
    bool unknown = false;
    for (size_t i = 1; i < e.kids.size(); i++) {
      Val y = Eval(c, *e.kids[i], fr);
      if (y.k == V_NULL) {
        unknown = true;
      } else if (Compare(x, y) == 0) {
        return Truth(1);
      }
    }
    return Truth(unknown ? -1 : 0);
  }
  case Expr::INSUB: {
    Val x = Eval(c, *e.kids[0], fr);
    Result r = RunSelect(c, *e.sub, fr);
    if (x.k == V_NULL) {
      return Val();
    }
    bool unknown = false;
    for (const auto & row : r.rows) {
      if (row[0].k == V_NULL) {
        unknown = true;
      } else if (Compare(x, row[0]) == 0) {
        return Truth(1);
      }
    }
    return Truth(unknown ? -1 : 0);
  }
  case Expr::EXISTS:
    return Truth(!RunSelect(c, *e.sub, fr).rows.empty());
  case Expr::SUBVAL: {
    Result r = RunSelect(c, *e.sub, fr);
    if (r.rows.empty()) {
      return Val();
    }
    return r.rows[0][0];
  }
  case Expr::COALESCE: {
    Val a = Eval(c, *e.kids[0], fr);
    return a.k != V_NULL ? a : Eval(c, *e.kids[1], fr);
  }
  case Expr::NULLIF: {
    Val a = Eval(c, *e.kids[0], fr);
    Val b = Eval(c, *e.kids[1], fr);
    if (a.k != V_NULL && b.k != V_NULL && Compare(a, b) == 0) {
      return Val();
    }
    return a;
  }
  default:
    throw std::runtime_error("an expression this engine cannot evaluate");
  }
}

inline const Table & TableOf(const Ctx & c, const std::string & name) {
  auto it = c.db->tables.find(name);
  if (it == c.db->tables.end()) {
    throw std::runtime_error("no table " + name);
  }
  return *it->second;
}

struct OutRow {
  std::vector<Val> vals;
  std::vector<Val> keys;
};

/* Join the FROM list into frames and call @p each for every combination. */
inline void Join(const Ctx & c, const Select & s, size_t at,
    std::vector<Frame> & fr, const std::function<void()> & each) {
  if (at == s.from.size()) {
    if (s.where == nullptr || TruthOf(Eval(c, *s.where, fr)) == 1) {
      each();
    }
    return;
  }
  const From & f = s.from[at];
  const Table & t = TableOf(c, f.table);
  bool any = false;
  /* Newest first: no caller may depend on insertion order. */
  for (size_t i = t.rows.size(); i-- > 0;) {
    fr.push_back(Frame{f.alias, &t, &t.rows[i]});
    bool on = f.on == nullptr || TruthOf(Eval(c, *f.on, fr)) == 1;
    if (on) {
      any = true;
      Join(c, s, at + 1, fr, each);
    }
    fr.pop_back();
  }
  if (!any && f.join == 2) {
    fr.push_back(Frame{f.alias, &t, nullptr});
    Join(c, s, at + 1, fr, each);
    fr.pop_back();
  }
}

inline Result RunSelect(const Ctx & c, const Select & s, std::vector<Frame> & fr) {
  std::vector<OutRow> out;
  bool count = s.items.size() == 1 && s.items[0]->k == Expr::COUNT;
  Join(c, s, 0, fr, [&]() {
    OutRow r;
    if (!count) {
      for (const auto & it : s.items) {
        r.vals.push_back(it->k == Expr::NUL ? Val() : Eval(c, *it, fr));
      }
    }
    for (const auto & o : s.order) {
      r.keys.push_back(Eval(c, *o.e, fr));
    }
    out.push_back(std::move(r));
  });
  Result res;
  if (count) {
    if (!s.order.empty() || s.limit != nullptr) {
      throw std::runtime_error("COUNT(*) with an order or a page");
    }
    res.rows.push_back({Val::Int((int64_t)out.size())});
    return res;
  }
  if (!s.order.empty()) {
    std::stable_sort(out.begin(), out.end(), [&](const OutRow & a, const OutRow & b) {
      for (size_t k = 0; k < s.order.size(); k++) {
        const Order & o = s.order[k];
        const Val & x = a.keys[k];
        const Val & y = b.keys[k];
        int cmp;
        if (x.k == V_NULL || y.k == V_NULL) {
          if (x.k == y.k) {
            continue;
          }
          /* Where a NULL goes: explicitly, or where this engine puts it. */
          bool null_first = o.nulls == 1 ? true
                            : o.nulls == 2 ? false
                                           : (c.nulls_low ? !o.desc : o.desc);
          bool x_first = (x.k == V_NULL) == null_first;
          return x_first;
        }
        cmp = Compare(x, y);
        if (cmp != 0) {
          return o.desc ? cmp > 0 : cmp < 0;
        }
      }
      return false;
    });
  }
  size_t first = 0;
  size_t n = out.size();
  if (s.limit != nullptr) {
    Val l = Eval(c, *s.limit, fr);
    Val o = Eval(c, *s.offset, fr);
    if (l.k != V_INT || o.k != V_INT || l.i < 0 || o.i < 0) {
      throw std::runtime_error("a page that is not two non-negative integers");
    }
    first = (uint64_t)o.i < out.size() ? (size_t)o.i : out.size();
    n = std::min((uint64_t)(out.size() - first), (uint64_t)l.i);
  }
  for (size_t i = 0; i < n; i++) {
    res.rows.push_back(std::move(out[first + i].vals));
  }
  return res;
}

/* ---- the engine ----------------------------------------------------------- */

struct LogEntry {
  std::string tenant;
  std::string text;
  std::vector<Val> binds;
  bool in_txn = false; /* run inside a transaction */
};

struct Engine;

struct Conn {
  Engine * e = nullptr;
  std::string tenant;
  bool write = false;
  bool in_txn = false;
  Db work; /* the snapshot, and a write's changes */
  std::set<std::string> begin_indexes; /* the indexes the snapshot began with */
  std::string qual;
  int stmts = 0; /* prepared and not finalized: at most one (executor.h) */
  std::map<std::string, Db> saves; /* the write's savepoints */
};

struct StmtH {
  Conn * c = nullptr;
  std::string text;
  Lexed lexed;
  std::vector<Val> binds;
  std::vector<bool> bound;
  bool ran = false;
  bool validity = false; /* the dialect's index_valid query */
  bool exists = false;   /* the dialect's store_exists query */
  Result result;
  size_t pos = 0;
};

struct Store {
  Db committed;
  std::string qual;
  bool writer = false;
};

struct Engine {
  GDEFSSQL_Dialect d;
  bool qualified = false; /* a schema per tenant, as Postgres */
  std::map<std::string, Store> stores;
  std::vector<LogEntry> log;
  bool keep_log = true;
  /* Fail the Nth statement (prepare or exec) from now, once: 0 never. */
  long fail_in = 0;
  long statements = 0;
  long discarded = 0;
  long open_conns = 0;
  long open_stmts = 0;
  /* Fail the Nth index build (outside a transaction) from now, once: 0
   * never. A concurrent build leaves its index invalid, as Postgres does. */
  long fail_index_build = 0;
  long index_builds_in_txn = 0; /* refused: CREATE INDEX inside a transaction */
  long invalid_drops = 0;       /* drops of an invalid index */
  std::string last_error;

  explicit Engine(const GDEFSSQL_Dialect & dialect, bool q = false)
      : d(dialect), qualified(q) {}

  bool Tick() {
    statements += 1;
    if (fail_in > 0 && --fail_in == 0) {
      last_error = "an injected failure";
      return false;
    }
    return true;
  }

  static Table & Mut(Db & db, const std::string & name) {
    auto it = db.tables.find(name);
    if (it == db.tables.end()) {
      throw std::runtime_error("no table " + name);
    }
    if (it->second.use_count() > 1) {
      /* Copy on write: the snapshot other connections hold is untouched. */
      it->second = std::make_shared<Table>(*it->second);
    }
    return *it->second;
  }

  static void Check(const Db & db, const Table & t, const std::vector<Val> & row,
      const std::vector<Val> * skip) {
    for (size_t i = 0; i < t.cols.size(); i++) {
      const Val & v = row[i];
      int ct = t.cols[i].coltype;
      if (v.k == V_NULL) {
        if (t.cols[i].notnull) {
          throw std::runtime_error("NULL in NOT NULL column " + t.name + "." + t.cols[i].name);
        }
        continue;
      }
      int want = ct == GDEFSSQL_COL_FLOAT ? V_REAL
                 : ct == GDEFSSQL_COL_TEXT || ct == GDEFSSQL_COL_REFERENCE ? V_TEXT
                 : ct == GDEFSSQL_COL_BYTES ? V_BYTES
                                            : V_INT;
      if (v.k != want) {
        throw std::runtime_error("a value of the wrong kind for " + t.name + "." + t.cols[i].name);
      }
    }
    auto clash = [&](const std::vector<std::string> & cols) {
      for (const auto & r : t.rows) {
        if (skip != nullptr && &r == skip) {
          continue;
        }
        bool eq = true;
        for (const auto & c : cols) {
          int k = t.Find(c);
          if (r[(size_t)k].k == V_NULL || row[(size_t)k].k == V_NULL ||
              !(r[(size_t)k] == row[(size_t)k])) {
            eq = false;
            break;
          }
        }
        if (eq) {
          return true;
        }
      }
      return false;
    };
    if (clash(t.key)) {
      throw std::runtime_error("duplicate key in " + t.name);
    }
    for (const auto & ix : db.indexes) {
      if (ix.second.unique && ix.second.table == t.name && clash(ix.second.cols)) {
        throw std::runtime_error("duplicate in unique index " + ix.first);
      }
    }
  }

  /* CREATE INDEX outside any transaction: straight into the committed store,
   * as a concurrent build is; IF NOT EXISTS skips an index of that name even
   * when it is invalid, as Postgres does. */
  GDEFSTOR_Result BuildOutside(Conn & c, const Stmt & s) {
    Db & db = stores[c.tenant].committed;
    if (!c.write) {
      last_error = "an index build on a read connection";
      return GDEFSTOR_ERR_IO;
    }
    if (s.concurrent != (d.index_build != nullptr && d.index_build[0] != '\0')) {
      last_error = "a build that is not the dialect's";
      return GDEFSTOR_ERR_IO;
    }
    if (db.indexes.count(s.index) != 0) {
      return GDEFSTOR_OK;
    }
    auto it = db.tables.find(s.table);
    if (it == db.tables.end()) {
      last_error = "an index on no table: " + s.table;
      return GDEFSTOR_ERR_IO;
    }
    for (const auto & col : s.names) {
      if (it->second->Find(col) < 0) {
        last_error = "an index on a column that is not there";
        return GDEFSTOR_ERR_IO;
      }
    }
    Index ix;
    ix.name = s.index;
    ix.table = s.table;
    ix.cols = s.names;
    ix.unique = s.unique;
    if (fail_index_build > 0 && --fail_index_build == 0) {
      if (s.concurrent) {
        ix.valid = false;
        db.indexes[s.index] = ix;
      }
      last_error = "an injected index build failure";
      return GDEFSTOR_ERR_IO;
    }
    db.indexes[s.index] = ix;
    return GDEFSTOR_OK;
  }

  GDEFSTOR_Result DropOutside(Conn & c, const Stmt & s) {
    Db & db = stores[c.tenant].committed;
    auto it = db.indexes.find(s.index);
    if (!c.write || !s.concurrent) {
      last_error = "a drop that is not the dialect's";
      return GDEFSTOR_ERR_IO;
    }
    if (it == db.indexes.end()) {
      return GDEFSTOR_OK;
    }
    if (it->second.valid) {
      last_error = "a drop of a valid index: " + s.index;
      return GDEFSTOR_ERR_IO;
    }
    invalid_drops += 1;
    db.indexes.erase(it);
    return GDEFSTOR_OK;
  }

  Result Run(Conn & c, const std::string & text, const Lexed & lx,
      const std::vector<Val> & binds) {
    Parser p(lx, c.qual);
    Stmt s = p.Statement();
    Db & db = c.work;
    Ctx cx{&db, &binds, d.nulls_low};
    std::vector<Frame> fr;
    Result res;
    (void)text;
    if (s.k != Stmt::SELECT && !c.write) {
      throw std::runtime_error("a write on a read connection");
    }
    switch (s.k) {
    case Stmt::CREATE_TABLE: {
      if (db.tables.count(s.table) != 0) {
        return res;
      }
      auto t = std::make_shared<Table>();
      t->name = s.table;
      t->cols = s.cols;
      t->key = s.names;
      for (const auto & k : t->key) {
        if (t->Find(k) < 0) {
          throw std::runtime_error("a key column that is not a column");
        }
      }
      for (const auto & col : t->cols) {
        if (t->cols.size() > d.max_columns) {
          throw std::runtime_error("too many columns");
        }
        (void)col;
      }
      db.tables[s.table] = t;
      return res;
    }
    case Stmt::CREATE_INDEX: {
      if (db.indexes.count(s.index) != 0) {
        return res;
      }
      const Table & t = TableOf(cx, s.table);
      for (const auto & col : s.names) {
        if (t.Find(col) < 0) {
          throw std::runtime_error("an index on a column that is not there");
        }
      }
      Index ix;
      ix.name = s.index;
      ix.table = s.table;
      ix.cols = s.names;
      ix.unique = s.unique;
      db.indexes[s.index] = ix;
      return res;
    }
    case Stmt::CREATE_SCHEMA:
      db.schema = true;
      return res;
    case Stmt::DROP_INDEX:
      throw std::runtime_error("DROP INDEX inside a transaction");
    case Stmt::ADD_COLUMN: {
      Table & t = Mut(db, s.table);
      if (t.Find(s.cols[0].name) >= 0) {
        /* Not repeatable without its record: a step taken twice. */
        throw std::runtime_error("duplicate column " + s.cols[0].name);
      }
      t.cols.push_back(s.cols[0]);
      for (auto & r : t.rows) {
        r.push_back(Val());
      }
      if (t.cols.size() > d.max_columns) {
        throw std::runtime_error("too many columns");
      }
      return res;
    }
    case Stmt::INSERT: {
      std::vector<std::vector<Val>> rows;
      const Table & t0 = TableOf(cx, s.table);
      std::vector<int> at;
      for (const auto & n : s.names) {
        int k = t0.Find(n);
        if (k < 0) {
          throw std::runtime_error("no column " + n + " in " + s.table);
        }
        at.push_back(k);
      }
      if (s.select != nullptr) {
        Result r = RunSelect(cx, *s.select, fr);
        for (auto & row : r.rows) {
          if (row.size() != at.size()) {
            throw std::runtime_error("INSERT ... SELECT of the wrong width");
          }
          rows.push_back(row);
        }
      } else {
        if (s.values.size() != at.size()) {
          throw std::runtime_error("INSERT of the wrong width");
        }
        std::vector<Val> row;
        for (const auto & v : s.values) {
          row.push_back(Eval(cx, *v, fr));
        }
        rows.push_back(row);
      }
      Table & t = Mut(db, s.table);
      for (const auto & in : rows) {
        std::vector<Val> full(t.cols.size());
        for (size_t i = 0; i < at.size(); i++) {
          full[(size_t)at[i]] = in[i];
        }
        if (s.upsert) {
          /* The conflicting row: by the named key (ON CONFLICT) or the
           * primary key (ON DUPLICATE KEY). */
          const std::vector<std::string> & key = s.conflict.empty() ? t.key : s.conflict;
          if (key != t.key) {
            throw std::runtime_error("ON CONFLICT on columns that are not the key");
          }
          std::vector<Val> * hit = nullptr;
          for (auto & r : t.rows) {
            bool eq = true;
            for (const auto & k : key) {
              int x = t.Find(k);
              eq = eq && r[(size_t)x] == full[(size_t)x];
            }
            if (eq) {
              hit = &r;
            }
          }
          if (hit != nullptr) {
            std::vector<Val> next = *hit;
            for (const auto & sc : s.set_cols) {
              int x = t.Find(sc);
              if (x < 0) {
                throw std::runtime_error("no column " + sc);
              }
              next[(size_t)x] = full[(size_t)x];
            }
            Check(db, t, next, hit);
            *hit = next;
            continue;
          }
        }
        Check(db, t, full, nullptr);
        t.rows.push_back(full);
      }
      return res;
    }
    case Stmt::UPDATE: {
      Table & t = Mut(db, s.table);
      std::vector<int> at;
      for (const auto & n : s.set_cols) {
        int k = t.Find(n);
        if (k < 0) {
          throw std::runtime_error("no column " + n);
        }
        at.push_back(k);
      }
      for (auto & r : t.rows) {
        fr.clear();
        fr.push_back(Frame{"", &t, &r});
        if (s.where != nullptr && TruthOf(Eval(cx, *s.where, fr)) != 1) {
          continue;
        }
        std::vector<Val> next = r;
        for (size_t i = 0; i < at.size(); i++) {
          next[(size_t)at[i]] = Eval(cx, *s.set_vals[i], fr);
        }
        Check(db, t, next, &r);
        r = next;
        if (!s.returning.empty()) {
          int k = t.Find(s.returning);
          if (k < 0) {
            throw std::runtime_error("RETURNING a column that is not there");
          }
          res.rows.push_back({r[(size_t)k]});
        }
      }
      fr.clear();
      return res;
    }
    case Stmt::DELETE: {
      Table & t = Mut(db, s.table);
      std::vector<std::vector<Val>> keep;
      for (auto & r : t.rows) {
        fr.clear();
        fr.push_back(Frame{"", &t, &r});
        if (s.where != nullptr && TruthOf(Eval(cx, *s.where, fr)) != 1) {
          keep.push_back(r);
        }
      }
      fr.clear();
      t.rows = keep;
      return res;
    }
    case Stmt::SELECT:
      return RunSelect(cx, *s.select, fr);
    case Stmt::SAVEPOINT:
    case Stmt::ROLLBACK_TO:
      throw std::runtime_error("a savepoint is run by exec, never prepared");
    }
    return res;
  }
};

/* ---- the executor --------------------------------------------------------- */

inline GDEFSTOR_Result X_open(void * ctx, const char * tenant, size_t len,
    int write, void ** out, const char ** out_qual) {
  Engine * e = (Engine *)ctx;
  std::string t(tenant == nullptr ? "" : tenant, len);
  auto it = e->stores.find(t);
  *out = nullptr;
  *out_qual = nullptr;
  if (it == e->stores.end()) {
    if (!write) {
      return GDEFSTOR_OK;
    }
    Store s;
    if (e->qualified) {
      s.qual = "s" + std::to_string(e->stores.size() + 1);
    }
    it = e->stores.emplace(t, s).first;
  }
  Conn * c = new Conn();
  c->e = e;
  c->tenant = t;
  c->write = write != 0;
  c->qual = it->second.qual;
  e->open_conns += 1;
  *out = c;
  *out_qual = c->qual.empty() ? nullptr : c->qual.c_str();
  return GDEFSTOR_OK;
}

inline void X_close(void * ctx, void * conn, int discard) {
  Engine * e = (Engine *)ctx;
  Conn * c = (Conn *)conn;
  if (c->in_txn && c->write) {
    e->stores[c->tenant].writer = false;
  }
  if (discard) {
    e->discarded += 1;
  }
  e->open_conns -= 1;
  delete c;
}

inline GDEFSTOR_Result X_exec(void * ctx, void * conn, const char * sql,
    size_t len) {
  Engine * e = (Engine *)ctx;
  Conn * c = (Conn *)conn;
  std::string text(sql, len);
  if (c->stmts != 0) {
    e->last_error = "exec with a statement open: " + text;
    return GDEFSTOR_ERR_IO;
  }
  Store & st = e->stores[c->tenant];
  if (e->keep_log) {
    e->log.push_back(LogEntry{c->tenant, text, {}, c->in_txn});
  }
  if (!e->Tick()) {
    return GDEFSTOR_ERR_IO;
  }
  if (text == e->d.begin_read || text == e->d.begin_write) {
    bool w = text == e->d.begin_write;
    if (c->in_txn || (w && !c->write) || (!w && c->write)) {
      e->last_error = "BEGIN on the wrong connection or twice";
      return GDEFSTOR_ERR_IO;
    }
    if (w) {
      if (st.writer) {
        return GDEFSTOR_ERR_BUSY;
      }
      st.writer = true;
    }
    c->work = st.committed;
    c->saves.clear();
    c->begin_indexes.clear();
    for (const auto & ix : st.committed.indexes) {
      c->begin_indexes.insert(ix.first);
    }
    c->in_txn = true;
    return GDEFSTOR_OK;
  }
  if (text == e->d.commit || text == e->d.rollback) {
    if (!c->in_txn) {
      e->last_error = "COMMIT outside a transaction";
      return GDEFSTOR_ERR_IO;
    }
    if (c->write) {
      if (text == e->d.commit) {
        /* Indexes built or dropped outside this transaction since it began
         * stay as they are; those it made itself are added. */
        std::map<std::string, Index> merged = st.committed.indexes;
        for (const auto & ix : c->work.indexes) {
          if (c->begin_indexes.count(ix.first) == 0) {
            merged[ix.first] = ix.second;
          }
        }
        st.committed = c->work;
        st.committed.indexes = merged;
      }
      st.writer = false;
    }
    c->work = Db();
    c->saves.clear();
    c->in_txn = false;
    return GDEFSTOR_OK;
  }
  try {
    Lexed lx = Lex(e->d, text);
    if (lx.params != 0) {
      throw std::runtime_error("exec of a statement with parameters");
    }
    Stmt s = Parser(lx, c->qual).Statement();
    if (s.k == Stmt::SAVEPOINT || s.k == Stmt::ROLLBACK_TO) {
      /* A savepoint is a copy of the write's state; rolling back to it puts
       * that copy back and keeps the savepoint, as SQL does. */
      if (!c->in_txn || !c->write) {
        throw std::runtime_error("a savepoint outside a write: " + text);
      }
      if (s.k == Stmt::SAVEPOINT) {
        c->saves[s.index] = c->work;
      } else {
        auto sv = c->saves.find(s.index);
        if (sv == c->saves.end()) {
          throw std::runtime_error("no such savepoint: " + text);
        }
        c->work = sv->second;
      }
      return GDEFSTOR_OK;
    }
    /* The key store is a port of its own: its one index is made with its
     * table, in its first transaction (keystore.c says why). */
    bool keystore = s.table.rfind("dsk_", 0) == 0;
    if (s.k == Stmt::CREATE_INDEX && c->in_txn && !keystore) {
      e->index_builds_in_txn += 1;
      throw std::runtime_error("CREATE INDEX inside a transaction (AD-15): " + text);
    }
    if (!c->in_txn) {
      if (s.k == Stmt::CREATE_INDEX) {
        return e->BuildOutside(*c, s);
      }
      if (s.k == Stmt::DROP_INDEX) {
        return e->DropOutside(*c, s);
      }
      throw std::runtime_error("a statement outside a transaction: " + text);
    }
    if (s.k != Stmt::CREATE_TABLE && s.k != Stmt::CREATE_INDEX &&
        s.k != Stmt::ADD_COLUMN && s.k != Stmt::CREATE_SCHEMA) {
      throw std::runtime_error("exec of a statement that is not DDL");
    }
    e->Run(*c, text, lx, {});
  } catch (const std::exception & ex) {
    e->last_error = ex.what();
    return GDEFSTOR_ERR_IO;
  }
  return GDEFSTOR_OK;
}

inline GDEFSTOR_Result X_prepare(void * ctx, void * conn, const char * sql,
    size_t len, void ** out) {
  Engine * e = (Engine *)ctx;
  Conn * c = (Conn *)conn;
  StmtH * h = new StmtH();
  h->c = c;
  h->text.assign(sql, len);
  if (c->stmts != 0) {
    /* The library promises one statement at a time per connection. */
    e->last_error = "a second statement open on one connection: " + h->text;
    delete h;
    return GDEFSTOR_ERR_IO;
  }
  if (!e->Tick()) {
    delete h;
    return GDEFSTOR_ERR_IO;
  }
  bool exists = e->d.store_exists != nullptr && h->text == e->d.store_exists;
  if (!c->in_txn && !exists) {
    e->last_error = "a statement outside a transaction: " + h->text;
    delete h;
    return GDEFSTOR_ERR_IO;
  }
  if (exists) {
    /* The dialect's catalog query: (qualifier, version table) -> a row when
     * the schema exists: whether the table is in it, and how many tables. */
    h->exists = true;
    h->lexed.params = 2;
  } else if (e->d.index_valid != nullptr && h->text == e->d.index_valid) {
    /* The dialect's own catalog query: (qualifier, index) -> its validity. */
    h->validity = true;
    h->lexed.params = 2;
  } else {
    try {
      h->lexed = Lex(e->d, h->text);
    } catch (const std::exception & ex) {
      e->last_error = ex.what();
      delete h;
      return GDEFSTOR_ERR_IO;
    }
  }
  h->binds.resize((size_t)h->lexed.params);
  h->bound.assign((size_t)h->lexed.params, false);
  e->open_stmts += 1;
  c->stmts += 1;
  *out = h;
  return GDEFSTOR_OK;
}

inline GDEFSTOR_Result X_bind(void * ctx, void * stmt, unsigned index,
    const GDEFSSQL_Datum * v) {
  Engine * e = (Engine *)ctx;
  StmtH * h = (StmtH *)stmt;
  if (index == 0 || index > h->binds.size() || h->ran) {
    e->last_error = "a bind out of range or after the run";
    return GDEFSTOR_ERR_IO;
  }
  Val x;
  switch (v->kind) {
  case GDEFSSQL_DATUM_INT:
    x = Val::Int(v->integer);
    break;
  case GDEFSSQL_DATUM_REAL:
    x.k = V_REAL;
    x.r = v->real;
    break;
  case GDEFSSQL_DATUM_TEXT:
  case GDEFSSQL_DATUM_BYTES:
    if (v->bytes == nullptr && v->len != 0) {
      e->last_error = "bytes NULL with a length";
      return GDEFSTOR_ERR_IO;
    }
    x.k = v->kind == GDEFSSQL_DATUM_TEXT ? V_TEXT : V_BYTES;
    x.s.assign((const char *)v->bytes, v->len);
    break;
  default:
    e->last_error = "a NULL bound";
    return GDEFSTOR_ERR_IO;
  }
  h->binds[index - 1] = x;
  h->bound[index - 1] = true;
  return GDEFSTOR_OK;
}

inline GDEFSTOR_Result X_step(void * ctx, void * stmt, int * out_row) {
  Engine * e = (Engine *)ctx;
  StmtH * h = (StmtH *)stmt;
  *out_row = 0;
  if (!h->ran) {
    h->ran = true;
    for (bool b : h->bound) {
      if (!b) {
        e->last_error = "a parameter left unbound: " + h->text;
        return GDEFSTOR_ERR_IO;
      }
    }
    if (e->keep_log) {
      e->log.push_back(LogEntry{h->c->tenant, h->text, h->binds, h->c->in_txn});
    }
    if (h->exists) {
      const Store & st = e->stores[h->c->tenant];
      if (h->binds[0].k == V_TEXT && h->binds[1].k == V_TEXT &&
          h->binds[0].s == st.qual && st.committed.schema) {
        h->result.rows.push_back(
            {Val::Int(st.committed.tables.count(h->binds[1].s) != 0 ? 1 : 0),
                Val::Int((int64_t)st.committed.tables.size())});
      }
      if (h->pos < h->result.rows.size()) {
        h->pos += 1;
        *out_row = 1;
      }
      return GDEFSTOR_OK;
    }
    if (h->validity) {
      if (h->binds[0].k != V_TEXT || h->binds[1].k != V_TEXT ||
          h->binds[0].s != h->c->qual) {
        e->last_error = "a validity query for another schema";
        return GDEFSTOR_ERR_IO;
      }
      auto it = h->c->work.indexes.find(h->binds[1].s);
      if (it != h->c->work.indexes.end()) {
        h->result.rows.push_back({Val::Int(it->second.valid ? 1 : 0)});
      }
      if (h->pos < h->result.rows.size()) {
        h->pos += 1;
        *out_row = 1;
      }
      return GDEFSTOR_OK;
    }
    try {
      h->result = e->Run(*h->c, h->text, h->lexed, h->binds);
    } catch (const std::exception & ex) {
      e->last_error = std::string(ex.what()) + " in: " + h->text;
      return GDEFSTOR_ERR_IO;
    }
  }
  if (h->pos < h->result.rows.size()) {
    h->pos += 1;
    *out_row = 1;
  }
  return GDEFSTOR_OK;
}

inline GDEFSTOR_Result X_column(void * ctx, void * stmt, unsigned index,
    int kind, GDEFSSQL_Datum * out) {
  Engine * e = (Engine *)ctx;
  StmtH * h = (StmtH *)stmt;
  std::memset(out, 0, sizeof *out);
  if (h->pos == 0 || index >= h->result.rows[h->pos - 1].size()) {
    e->last_error = "a column out of range";
    return GDEFSTOR_ERR_IO;
  }
  const Val & v = h->result.rows[h->pos - 1][index];
  int want = kind == GDEFSSQL_DATUM_INT ? V_INT
             : kind == GDEFSSQL_DATUM_REAL ? V_REAL
             : kind == GDEFSSQL_DATUM_TEXT ? V_TEXT
                                           : V_BYTES;
  if (v.k == V_NULL) {
    out->kind = GDEFSSQL_DATUM_NULL;
    return GDEFSTOR_OK;
  }
  if (v.k != want) {
    e->last_error = "a column read as another kind";
    return GDEFSTOR_ERR_CORRUPT;
  }
  out->kind = kind;
  out->integer = v.i;
  out->real = v.r;
  out->bytes = (const uint8_t *)v.s.data();
  out->len = v.s.size();
  return GDEFSTOR_OK;
}

inline void X_finalize(void * ctx, void * stmt) {
  Engine * e = (Engine *)ctx;
  StmtH * h = (StmtH *)stmt;
  e->open_stmts -= 1;
  h->c->stmts -= 1;
  delete h;
}

/* The executor over @p e; the engine stays the caller's (no destroy). */
inline GDEFSSQL_Executor ExecutorOf(Engine * e) {
  GDEFSSQL_Executor x;
  std::memset(&x, 0, sizeof x);
  x.size = sizeof x;
  x.ctx = e;
  x.open = X_open;
  x.close = X_close;
  x.exec = X_exec;
  x.prepare = X_prepare;
  x.bind = X_bind;
  x.step = X_step;
  x.column = X_column;
  x.finalize = X_finalize;
  return x;
}

/* ---- the tee: every query also answered by the reference scan ------------ */

struct Tee {
  GDEFSTOR_ModelBackend inner;
  long queries = 0;
  long disagreements = 0;
  long refusals = 0;
  std::string first_disagreement;
  std::function<void(const std::string &)> on_disagree;
};

struct ScanCtx {
  Tee * tee;
  const char * tenant;
  size_t tenant_len;
};

struct ScanFilter {
  const GDEFSTOR_ModelVisitor * to;
  std::string type;
  bool on = false;
};

inline GDEFSTOR_Result SF_entity(void * ctx, const GDEFSTOR_ModelEntity * e) {
  ScanFilter * f = (ScanFilter *)ctx;
  f->on = std::string(e->type, e->type_len) == f->type;
  return f->on ? f->to->entity(f->to->ctx, e) : GDEFSTOR_OK;
}

inline GDEFSTOR_Result SF_item(void * ctx, uint64_t row, const GDEFSTOR_ModelItem * i) {
  ScanFilter * f = (ScanFilter *)ctx;
  return f->on ? f->to->item(f->to->ctx, row, i) : GDEFSTOR_OK;
}

/* gdefstor_model_scan()'s view of the store: the current tables of the last
 * commit, through the backend's own dump of them. Entities come grouped
 * with their items, as the scan wants, because the dump visits an entity's
 * type's items after its entities: so collect first. */
struct Collected {
  struct B {
    GDEFSTOR_ModelBlind b;
    std::string subject, mac, lang;
  };
  struct E {
    GDEFSTOR_ModelEntity e;
    std::string type, pid, creator;
    std::vector<GDEFSTOR_ModelItem> items;
    std::vector<std::string> langs;
    std::vector<std::vector<GDEFSTOR_Value>> vals;
    std::vector<std::vector<std::string>> bytes;
    std::vector<std::vector<std::string>> subjects;
    std::vector<B> blinds;
    /* The newest revision, when the scan is asked for it. */
    bool have_rev = false;
    GDEFSTOR_ModelRevision rev;
    std::string who, why, rlangs, rschema;
  };
  std::map<uint64_t, E> ents;
};

inline GDEFSTOR_Result C_entity(void * ctx, const GDEFSTOR_ModelEntity * e) {
  Collected * c = (Collected *)ctx;
  Collected::E & x = c->ents[e->row];
  x.e = *e;
  x.type.assign(e->type, e->type_len);
  x.pid.assign(e->public_id, e->public_id_len);
  x.creator.assign(e->creator, e->creator_len);
  return GDEFSTOR_OK;
}

inline GDEFSTOR_Result C_item(void * ctx, uint64_t row, const GDEFSTOR_ModelItem * i) {
  Collected * c = (Collected *)ctx;
  Collected::E & x = c->ents[row];
  x.items.push_back(*i);
  x.langs.push_back(std::string(i->language, i->language_len));
  std::vector<GDEFSTOR_Value> v(i->values, i->values + i->value_count);
  /* Copies of what the dump lends: bytes and subjects both, since the
   * lent ones are gone once the dump returns. */
  std::vector<std::string> b;
  std::vector<std::string> subj;
  for (auto & one : v) {
    b.push_back(one.len == 0 ? std::string()
                             : std::string((const char *)one.bytes, one.len));
    subj.push_back(one.subject_len == 0
                       ? std::string()
                       : std::string((const char *)one.subject, one.subject_len));
  }
  x.vals.push_back(v);
  x.bytes.push_back(b);
  x.subjects.push_back(subj);
  return GDEFSTOR_OK;
}

/* Blind rows are truth, not current: they come from the truth dump. */
inline GDEFSTOR_Result C_blind(void * ctx, uint64_t row, const GDEFSTOR_ModelBlind * b) {
  Collected * c = (Collected *)ctx;
  auto it = c->ents.find(row);
  if (it == c->ents.end()) {
    return GDEFSTOR_ERR_CORRUPT; /* a blind row of no entity */
  }
  Collected::B x;
  x.b = *b;
  x.subject.assign((const char *)b->subject, b->subject_len);
  x.mac.assign((const char *)b->mac, GDEFSTOR_BLIND_MAC_BYTES);
  x.lang.assign(b->language, b->language_len);
  it->second.blinds.push_back(x);
  return GDEFSTOR_OK;
}

/* Revisions are truth: the newest of each entity, from the truth dump. */
inline GDEFSTOR_Result C_revision(void * ctx, uint64_t row,
    const GDEFSTOR_ModelRevision * r) {
  Collected * c = (Collected *)ctx;
  auto it = c->ents.find(row);
  if (it == c->ents.end()) {
    return GDEFSTOR_ERR_CORRUPT; /* a revision of no entity */
  }
  Collected::E & x = it->second;
  if (x.have_rev && r->revision < x.rev.revision) {
    return GDEFSTOR_OK;
  }
  x.have_rev = true;
  x.rev = *r;
  x.who.assign(r->who == nullptr ? "" : r->who, r->who_len);
  x.why.assign(r->why == nullptr ? "" : r->why, r->why_len);
  x.rlangs.assign(r->languages == nullptr ? "" : r->languages,
      r->languages_len);
  x.rschema.assign(r->schema == nullptr ? "" : r->schema, r->schema_len);
  return GDEFSTOR_OK;
}

inline GDEFSTOR_Result TeeScan(void * ctx, const char * type, size_t type_len,
    const GDEFSTOR_ModelVisitor * visitor) {
  ScanCtx * s = (ScanCtx *)ctx;
  Collected col;
  GDEFSTOR_ModelVisitor v;
  std::memset(&v, 0, sizeof v);
  v.ctx = &col;
  v.entity = C_entity;
  v.item = C_item;
  GDEFSTOR_Result rc = s->tee->inner.dump(s->tee->inner.ctx, s->tenant,
      s->tenant_len, GDEFSTOR_MODEL_CURRENT, &v);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (visitor->blind != nullptr || visitor->revision != nullptr) {
    GDEFSTOR_ModelVisitor bv;
    std::memset(&bv, 0, sizeof bv);
    bv.ctx = &col;
    bv.blind = visitor->blind != nullptr ? C_blind : nullptr;
    bv.revision = visitor->revision != nullptr ? C_revision : nullptr;
    rc = s->tee->inner.dump(s->tee->inner.ctx, s->tenant, s->tenant_len,
        GDEFSTOR_MODEL_TRUTH, &bv);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  std::string want(type, type_len);
  for (auto & kv : col.ents) {
    Collected::E & x = kv.second;
    if (x.type != want) {
      continue;
    }
    x.e.type = x.type.data();
    x.e.public_id = x.pid.data();
    x.e.creator = x.creator.data();
    rc = visitor->entity(visitor->ctx, &x.e);
    for (size_t i = 0; rc == GDEFSTOR_OK && i < x.items.size(); i++) {
      for (size_t k = 0; k < x.vals[i].size(); k++) {
        x.vals[i][k].bytes = (const uint8_t *)x.bytes[i][k].data();
        x.vals[i][k].subject = (const uint8_t *)x.subjects[i][k].data();
      }
      x.items[i].values = x.vals[i].data();
      x.items[i].language = x.langs[i].data();
      rc = visitor->item(visitor->ctx, x.e.row, &x.items[i]);
    }
    for (size_t i = 0; rc == GDEFSTOR_OK && visitor->blind != nullptr &&
                       i < x.blinds.size(); i++) {
      x.blinds[i].b.subject = (const uint8_t *)x.blinds[i].subject.data();
      x.blinds[i].b.mac = (const uint8_t *)x.blinds[i].mac.data();
      x.blinds[i].b.language = x.blinds[i].lang.data();
      rc = visitor->blind(visitor->ctx, x.e.row, &x.blinds[i].b);
    }
    if (rc == GDEFSTOR_OK && visitor->revision != nullptr && x.have_rev) {
      x.rev.who = x.who.data();
      x.rev.why = x.why.data();
      x.rev.languages = x.rlangs.data();
      x.rev.schema = x.rschema.data();
      rc = visitor->revision(visitor->ctx, x.e.row, &x.rev);
    }
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  return GDEFSTOR_OK;
}

inline std::string HitsText(const GDEFSTOR_ModelHits & h) {
  std::string s = "total " + std::to_string(h.total) + ":";
  for (size_t i = 0; i < h.count; i++) {
    s += " " + std::string(h.public_ids[i], h.public_id_lens[i]) + "#" +
         std::to_string(h.rows[i]);
  }
  return s;
}

inline GDEFSTOR_Result Tee_query(void * ctx, const char * tenant, size_t tlen,
    const GDEFSTOR_ModelPlan * plan, GDEFSTOR_ModelHits * out) {
  Tee * t = (Tee *)ctx;
  GDEFSTOR_Result rc = t->inner.query(t->inner.ctx, tenant, tlen, plan, out);
  t->queries += 1;
  if (rc != GDEFSTOR_OK) {
    if (rc == GDEFSTOR_ERR_UNSUPPORTED || rc == GDEFSTOR_ERR_LIMIT) {
      t->refusals += 1;
    }
    return rc;
  }
  GDEFSTOR_ModelHits ref;
  std::memset(&ref, 0, sizeof ref);
  ScanCtx sc{t, tenant, tlen};
  GDEFSTOR_Result rr = gdefstor_model_scan(plan, TeeScan, &sc, &ref);
  if (rr != GDEFSTOR_OK) {
    /* The reference could not answer (an allocation failed): no verdict. */
    gdefstor_model_hits_release(plan->alloc, &ref);
    return rc;
  }
  std::string a = HitsText(*out);
  std::string b = HitsText(ref);
  if (a != b) {
    t->disagreements += 1;
    if (t->first_disagreement.empty()) {
      t->first_disagreement = "sql " + a + "\nref " + b;
    }
    if (t->on_disagree) {
      t->on_disagree("sql " + a + "\nref " + b);
    }
  }
  gdefstor_model_hits_release(plan->alloc, &ref);
  return rc;
}

#define SQLSTUB_FWD(name, ret, params, args)                                   \
  inline ret Tee_##name params {                                               \
    Tee * t = (Tee *)ctx;                                                      \
    return t->inner.name args;                                                 \
  }
SQLSTUB_FWD(begin, GDEFSTOR_Result, (void * ctx, const char * a, size_t b, void ** c),
    (t->inner.ctx, a, b, c))
SQLSTUB_FWD(commit, GDEFSTOR_Result, (void * ctx, void * x), (t->inner.ctx, x))
SQLSTUB_FWD(abort, GDEFSTOR_Result, (void * ctx, void * x), (t->inner.ctx, x))
SQLSTUB_FWD(type_load, GDEFSTOR_Result, (void * ctx, void * x, const char * a,
    size_t b, const char * c, size_t d, uint8_t ** e, size_t * f),
    (t->inner.ctx, x, a, b, c, d, e, f))
SQLSTUB_FWD(type_store, GDEFSTOR_Result, (void * ctx, void * x,
    const GDEFSTOR_Type * a, const uint8_t * b, size_t c), (t->inner.ctx, x, a, b, c))
SQLSTUB_FWD(entity_find, GDEFSTOR_Result, (void * ctx, void * x, const char * a,
    size_t b, const char * c, size_t d, const GDEFSTOR_ModelVisitor * v),
    (t->inner.ctx, x, a, b, c, d, v))
SQLSTUB_FWD(entity_insert, GDEFSTOR_Result, (void * ctx, void * x,
    const GDEFSTOR_ModelEntity * e, uint64_t * r), (t->inner.ctx, x, e, r))
SQLSTUB_FWD(state_append, GDEFSTOR_Result, (void * ctx, void * x, uint64_t r,
    const GDEFSTOR_ModelState * s), (t->inner.ctx, x, r, s))
SQLSTUB_FWD(apply, GDEFSTOR_Result, (void * ctx, void * x,
    const GDEFSTOR_ModelApply * a), (t->inner.ctx, x, a))
SQLSTUB_FWD(history, GDEFSTOR_Result, (void * ctx, void * x, const char * a,
    size_t b, uint64_t r, const GDEFSTOR_ModelVisitor * v), (t->inner.ctx, x, a, b, r, v))
SQLSTUB_FWD(items, GDEFSTOR_Result, (void * ctx, void * x, const char * a,
    size_t b, uint64_t r, uint64_t o, const char * l, size_t ll,
    const GDEFSTOR_ModelVisitor * v), (t->inner.ctx, x, a, b, r, o, l, ll, v))
SQLSTUB_FWD(draft_put, GDEFSTOR_Result, (void * ctx, void * x,
    const GDEFSTOR_ModelDraft * d), (t->inner.ctx, x, d))
SQLSTUB_FWD(draft_get, GDEFSTOR_Result, (void * ctx, void * x, const char * a,
    size_t b, const char * c, size_t d, const GDEFSTOR_ModelVisitor * v),
    (t->inner.ctx, x, a, b, c, d, v))
SQLSTUB_FWD(draft_delete, GDEFSTOR_Result, (void * ctx, void * x,
    const char * a, size_t b), (t->inner.ctx, x, a, b))
SQLSTUB_FWD(rebuild, GDEFSTOR_Result, (void * ctx, void * x), (t->inner.ctx, x))
SQLSTUB_FWD(dump, GDEFSTOR_Result, (void * ctx, const char * a, size_t b, int w,
    const GDEFSTOR_ModelVisitor * v), (t->inner.ctx, a, b, w, v))
#undef SQLSTUB_FWD

inline void Tee_destroy(void * ctx) {
  Tee * t = (Tee *)ctx;
  t->inner.destroy(t->inner.ctx);
}

/* A backend that is @p inner with every query cross-checked. The Tee is the
 * caller's and must outlive the store. */
inline GDEFSTOR_ModelBackend TeeOf(Tee * t, const GDEFSTOR_ModelBackend & inner) {
  t->inner = inner;
  GDEFSTOR_ModelBackend b = inner;
  b.ctx = t;
  b.destroy = Tee_destroy;
  b.begin = Tee_begin;
  b.commit = Tee_commit;
  b.abort = Tee_abort;
  b.type_load = Tee_type_load;
  b.type_store = Tee_type_store;
  b.entity_find = Tee_entity_find;
  b.entity_insert = Tee_entity_insert;
  b.state_append = Tee_state_append;
  b.apply = Tee_apply;
  b.history = Tee_history;
  b.items = Tee_items;
  b.draft_put = Tee_draft_put;
  b.draft_get = Tee_draft_get;
  b.draft_delete = Tee_draft_delete;
  b.query = Tee_query;
  b.rebuild = Tee_rebuild;
  b.dump = Tee_dump;
  return b;
}

/* ---- a store over the engine --------------------------------------------- */

struct Rig {
  std::unique_ptr<Engine> engine;
  Tee tee;
  GDEFSTOR_Store * store = nullptr;
  GDEFSTOR_ModelBackend model; /* the library's backend, untee'd */
};

/* Open a store: the library's backend over a fresh engine, tee'd. */
inline GDEFSTOR_Result OpenRig(Rig & r, const GDEFSSQL_Dialect & d,
    bool qualified, const GDEFSTOR_Allocator * alloc) {
  r.engine = std::make_unique<Engine>(d, qualified);
  GDEFSSQL_Executor x = ExecutorOf(r.engine.get());
  GDEFSSQL_Result rc = gdefssql_model_backend(&d, &x, alloc, &r.model);
  if (rc != GDEFSSQL_OK) {
    return gdefssql_result_to_port(rc);
  }
  GDEFSTOR_ModelBackend tee = TeeOf(&r.tee, r.model);
  GDEFSTOR_Result prc = gdefstor_store_open(&tee, alloc, nullptr, &r.store);
  if (prc != GDEFSTOR_OK) {
    r.model.destroy(r.model.ctx);
  }
  return prc;
}

}  // namespace sqlstub

#endif /* GHOTI_IO_GDEFSSQL_TESTS_SQL_STUB_H */
