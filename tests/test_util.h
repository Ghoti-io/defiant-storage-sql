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
 * Small helpers the tests of this library share: declaring types, building
 * and saving trees, asking queries, and reading the statement log.
 */

#ifndef GHOTI_IO_GDEFSSQL_TESTS_TEST_UTIL_H
#define GHOTI_IO_GDEFSSQL_TESTS_TEST_UTIL_H

#include "sql_stub.h"

#include <map>
#include <string>
#include <vector>

namespace tu {

const char * const kTenant = "t1";

/* ---- declarations --------------------------------------------------------- */

struct F {
  std::string name;
  int type = GDEFSTOR_FIELD_TEXT;
  unsigned flags = 0;
  int scale = 0;
  std::vector<F> kids;
  std::string subject;
  bool has_subject = false;
};

inline F Field(const std::string & name, int type, unsigned flags = 0,
    int scale = 0) {
  F f;
  f.name = name;
  f.type = type;
  f.flags = flags;
  f.scale = scale;
  return f;
}

inline F Group(const std::string & name, unsigned flags, std::vector<F> kids) {
  F f;
  f.name = name;
  f.type = GDEFSTOR_FIELD_GROUP;
  f.flags = flags;
  f.kids = std::move(kids);
  return f;
}

struct SpecArena {
  std::vector<std::vector<GDEFSTOR_FieldSpec>> lists;
  const GDEFSTOR_FieldSpec * Make(const std::vector<F> & fs) {
    lists.emplace_back();
    size_t at = lists.size() - 1;
    lists[at].resize(fs.size());
    for (size_t i = 0; i < fs.size(); i++) {
      GDEFSTOR_FieldSpec s;
      std::memset(&s, 0, sizeof s);
      s.size = sizeof s;
      s.name = fs[i].name.c_str();
      s.name_len = fs[i].name.size();
      s.type = fs[i].type;
      s.scale = fs[i].scale;
      s.flags = fs[i].flags;
      if (fs[i].has_subject) {
        s.subject = fs[i].subject.c_str();
        s.subject_len = fs[i].subject.size();
      }
      if (!fs[i].kids.empty()) {
        s.children = Make(fs[i].kids);
        s.child_count = fs[i].kids.size();
      }
      lists[at][i] = s;
    }
    return lists[at].data();
  }
};

inline GDEFSTOR_Result InWrite(GDEFSTOR_Store * store,
    const std::function<GDEFSTOR_Result(GDEFSTOR_Write *)> & body,
    const char * tenant = kTenant) {
  GDEFSTOR_Write * w = nullptr;
  GDEFSTOR_Result rc = gdefstor_write_begin(store, tenant, std::strlen(tenant),
      nullptr, &w);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rc = body(w);
  if (rc != GDEFSTOR_OK) {
    gdefstor_write_abort(w);
    return rc;
  }
  return gdefstor_write_commit(w);
}

inline GDEFSTOR_Result Declare(GDEFSTOR_Store * store, const std::string & type,
    const std::vector<F> & fields, const char * tenant = kTenant) {
  SpecArena arena;
  GDEFSTOR_TypeSpec spec;
  std::memset(&spec, 0, sizeof spec);
  spec.size = sizeof spec;
  spec.name = type.c_str();
  spec.name_len = type.size();
  spec.fields = arena.Make(fields);
  spec.field_count = fields.size();
  return InWrite(store, [&](GDEFSTOR_Write * w) {
    return gdefstor_type_declare(w, &spec);
  }, tenant);
}

/* ---- values and trees ----------------------------------------------------- */

struct V {
  int type = 0;
  int64_t i = 0;
  double r = 0.0;
  std::string s;
  std::string subject;
};

inline V Txt(const std::string & s) {
  V v;
  v.type = GDEFSTOR_FIELD_TEXT;
  v.s = s;
  return v;
}
inline V Ref(const std::string & s) {
  V v = Txt(s);
  v.type = GDEFSTOR_FIELD_REFERENCE;
  return v;
}
inline V Num(int type, int64_t i) {
  V v;
  v.type = type;
  v.i = i;
  return v;
}
inline V Flt(double d) {
  V v;
  v.type = GDEFSTOR_FIELD_FLOAT;
  v.r = d;
  return v;
}
inline V Personal(int type, const std::string & cipher, const std::string & subject) {
  V v;
  v.type = type;
  v.s = cipher;
  v.subject = subject;
  return v;
}

inline GDEFSTOR_Value ValueOf(const V & v) {
  GDEFSTOR_Value x;
  std::memset(&x, 0, sizeof x);
  x.type = v.type;
  x.integer = v.i;
  x.real = v.r;
  x.bytes = (const uint8_t *)v.s.data();
  x.len = v.s.size();
  if (!v.subject.empty()) {
    x.subject = (const uint8_t *)v.subject.data();
    x.subject_len = v.subject.size();
  }
  return x;
}

struct Node {
  std::map<std::string, V> vals;
  std::vector<std::pair<std::string, Node>> kids; /* in order */
  std::vector<std::pair<std::string, V>> multi;   /* items of multiple scalars */
  Node & Set(const std::string & f, const V & v) {
    vals[f] = v;
    return *this;
  }
  Node & Add(const std::string & f) {
    kids.emplace_back(f, Node());
    return kids.back().second;
  }
  Node & Item(const std::string & f, const V & v) {
    multi.emplace_back(f, v);
    return *this;
  }
};

inline GDEFSTOR_Result Fill(GDEFSTOR_Tree * t, size_t at, const Node & n) {
  for (const auto & kv : n.vals) {
    GDEFSTOR_Value v = ValueOf(kv.second);
    GDEFSTOR_Result rc = gdefstor_tree_set(t, at, kv.first.data(),
        kv.first.size(), &v);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  for (const auto & m : n.multi) {
    size_t item = 0;
    GDEFSTOR_Result rc = gdefstor_tree_add(t, at, m.first.data(),
        m.first.size(), 0, &item);
    GDEFSTOR_Value v = ValueOf(m.second);
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_tree_set(t, item, "", 0, &v);
    }
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  for (const auto & k : n.kids) {
    size_t item = 0;
    GDEFSTOR_Result rc = gdefstor_tree_add(t, at, k.first.data(),
        k.first.size(), 0, &item);
    if (rc == GDEFSTOR_OK) {
      rc = Fill(t, item, k.second);
    }
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  return GDEFSTOR_OK;
}

inline GDEFSTOR_RevisionMeta Meta() {
  GDEFSTOR_RevisionMeta m;
  std::memset(&m, 0, sizeof m);
  m.size = sizeof m;
  m.who = "u1";
  m.who_len = 2;
  m.when_ms = 1000;
  m.why = "test";
  m.why_len = 4;
  return m;
}

/* Create @p pid (when @p create) and save @p tree on @p base. */
inline GDEFSTOR_Result Put(GDEFSTOR_Store * store, const std::string & type,
    const std::string & pid, const Node & tree, uint64_t base, bool create,
    uint64_t * out_rev = nullptr, const std::string & creator = "",
    int64_t created = 5, const char * tenant = kTenant) {
  return InWrite(store, [&](GDEFSTOR_Write * w) {
    GDEFSTOR_Result rc = GDEFSTOR_OK;
    if (create) {
      rc = gdefstor_entity_create(w, type.data(), type.size(), pid.data(),
          pid.size(), creator.data(), creator.size(), created, nullptr);
    }
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    GDEFSTOR_Tree * t = nullptr;
    rc = gdefstor_tree_create(nullptr, &t);
    if (rc == GDEFSTOR_OK) {
      rc = Fill(t, 0, tree);
    }
    GDEFSTOR_RevisionMeta m = Meta();
    if (rc == GDEFSTOR_OK) {
      rc = gdefstor_entity_save(w, pid.data(), pid.size(), base, &m, t, out_rev);
    }
    gdefstor_tree_free(t);
    return rc;
  }, tenant);
}

inline GDEFSTOR_Result State(GDEFSTOR_Store * store, const std::string & pid,
    int pub, int del, const char * tenant = kTenant) {
  return InWrite(store, [&](GDEFSTOR_Write * w) {
    return gdefstor_entity_set_state(w, pid.data(), pid.size(), pub, del, "a1",
        2, 7, nullptr);
  }, tenant);
}

/* Ask: the public ids of the page, in order; the total through *out_total. */
inline GDEFSTOR_Result Ask(GDEFSTOR_Store * store, const std::string & type,
    const GDEFSTOR_Query & q, std::vector<std::string> * out_ids,
    uint64_t * out_total, const char * tenant = kTenant) {
  GDEFSTOR_QueryResult * r = nullptr;
  GDEFSTOR_Result rc = gdefstor_entity_query(store, tenant, std::strlen(tenant),
      type.data(), type.size(), &q, &r);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  out_ids->clear();
  for (size_t i = 0; i < gdefstor_query_result_rows(r); i++) {
    const char * p = nullptr;
    size_t n = 0;
    gdefstor_query_result_entity(r, i, &p, &n);
    out_ids->push_back(std::string(p, n));
  }
  *out_total = gdefstor_query_result_total(r);
  gdefstor_query_result_free(r);
  return GDEFSTOR_OK;
}

/* ---- the statement log ---------------------------------------------------- */

inline bool IsDdl(const std::string & s) {
  return s.rfind("CREATE ", 0) == 0 || s.rfind("ALTER ", 0) == 0;
}

/* The DDL logged from entry @p from on. */
inline std::vector<std::string> DdlSince(const sqlstub::Engine & e, size_t from) {
  std::vector<std::string> out;
  for (size_t i = from; i < e.log.size(); i++) {
    if (IsDdl(e.log[i].text)) {
      out.push_back(e.log[i].text);
    }
  }
  return out;
}

}  // namespace tu

#endif /* GHOTI_IO_GDEFSSQL_TESTS_TEST_UTIL_H */
