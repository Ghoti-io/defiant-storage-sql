#!/bin/sh
#
# SPDX-License-Identifier: LGPL-3.0-only
#
# Show that each defect planted in the SQL layer turns a named test red: the
# defects a shared schema generator and query compiler can commit, which the
# tests exist to catch. Each plant edits a scratch copy of the real source
# (tools/plant-lib.sh), so nothing in src/ selects a defect.
#
# Usage: check-sql.sh <make> <build-dir> [make args]

set -u

MAKE="${1:?usage: check-sql.sh <make> <build-dir> [make args]}"
BUILD_DIR="${2:?usage: check-sql.sh <make> <build-dir> [make args]}"
shift 2

. tools/plant-lib.sh
plant_setup "$MAKE" "$BUILD_DIR" "$@"
plant_control testSchema 'Schema.*'
plant_control testQuery 'Compile.*'
plant_control testAgreement 'Agreement.*'
plant_control testConformance 'ModelHistory.*:ModelDerived.*'
plant_control testKeystore 'KeyStore.*'
plant_control testConformance 'ModelBlind.*'
plant_control testBlind 'BlindSql.*'
plant_control testConformance 'ModelRecords.*:ModelCounters.*:ModelOutbox.*:ModelSystem.*'
plant_control testConformance 'ModelLanguage*'

# An identifier made from the caller's field name, with the one gate on
# identifiers (gdefssql_sb_ident's check) gone: caller text in SQL.
plantm unvalidated-identifier testSchema 'Schema.HostileNamesNeverReachSqlText' \
  src/sql.c \
  '  if (!gdefssql_name_ok(name, b->d->max_identifier)) {
    b->rc = GDEFSTOR_ERR_INTERNAL;
    return;
  }
  gdefssql_sb_kw(b, b->d->quote_open);' \
  '  gdefssql_sb_kw(b, b->d->quote_open);' \
  src/compile.c \
  '    out->kind = SRC_FIELD;
    out->owner = s;' \
  '    out->kind = SRC_FIELD;
    out->column = o->field;
    out->owner = s;' \
  src/compile.c \
  '  char col[GDEFSSQL_NAME_CAP];
  gdefssql_name_column(col, f->col, 0);
  gdefssql_sb_col(q->sb, alias, col);' \
  '  char col[GDEFSSQL_NAME_CAP];
  gdefssql_name_column(col, f->col, 0);
  gdefssql_sb_col(q->sb, alias, f->column != NULL ? f->column : col);'

# An integer constant written into the text instead of bound.
plantm value-concatenated testQuery 'Compile.EveryConstantIsABind' \
  src/compile.c \
  '#include <string.h>' \
  '#include <stdio.h>
#include <string.h>' \
  src/compile.c \
  '  GDEFSSQL_Datum d;
  gdefssql_value_datum(v, &d);
  gdefssql_sb_param(q->sb, &d);' \
  '  GDEFSSQL_Datum d;
  gdefssql_value_datum(v, &d);
  if (d.kind == GDEFSSQL_DATUM_INT) {
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%lld", (long long)d.integer);
    gdefssql_sb_kw(q->sb, tmp);
    return;
  }
  gdefssql_sb_param(q->sb, &d);'

# The last clause of every AND dropped from the WHERE.
plant dropped-where-clause src/compile.c testAgreement \
  'Agreement.RandomQueriesMatchTheReferenceScan' \
  '    for (i = 0; i < n->child_count; i++) {
      if (i > 0) {
        gdefssql_sb_kw(q->sb, n->op == GDEFSTOR_QUERY_AND ? " AND " : " OR ");' \
  '    for (i = 0; i < n->child_count; i++) {
      if (n->op == GDEFSTOR_QUERY_AND && i > 0 && i + 1 == n->child_count &&
          n->children[i]->op != GDEFSTOR_QUERY_EXISTS) {
        break;
      }
      if (i > 0) {
        gdefssql_sb_kw(q->sb, n->op == GDEFSTOR_QUERY_AND ? " AND " : " OR ");'

# A field that stops being indexed has its index dropped: a schema step
# that is not additive.
plant non-additive-step src/schema.c testSchema \
  'Schema.EveryStepIsAdditive Schema.AddingAFieldIsOneAddColumnStep' \
  '    if (w->indexed && have(&cat, w->grp, w->col, GDEFSSQL_ROLE_INDEX) == NULL) {
      rc = add_index(c, tno, w, GDEFSSQL_ROLE_INDEX);
    }' \
  '    if (w->indexed && have(&cat, w->grp, w->col, GDEFSSQL_ROLE_INDEX) == NULL) {
      rc = add_index(c, tno, w, GDEFSSQL_ROLE_INDEX);
    } else if (!w->indexed &&
               have(&cat, w->grp, w->col, GDEFSSQL_ROLE_INDEX) != NULL) {
      GDEFSSQL_Sb sb;
      char index[GDEFSSQL_NAME_CAP];
      gdefssql_name_index(index, tno, GDEFSSQL_ROLE_INDEX, w->grp, w->col);
      gdefssql_sb_init(&sb, c->b->alloc, &c->b->d, c->qual);
      gdefssql_sb_kw(&sb, "DROP INDEX ");
      gdefssql_sb_ident(&sb, index);
      rc = gdefssql_cx_exec(c, &sb);
      gdefssql_sb_free(&sb);
    }'

# A field's index built in its step, inside the declaring write's
# transaction, instead of recorded and built after the commit (AD-15).
plant index-in-step src/schema.c testSchema \
  'Index.NoIndexIsBuiltInsideAStepTransaction' \
  '  gdefssql_name_index(index, tno, role, w->grp, w->col);
  return record(c, tno, w->grp, w->col, role, index, GDEFSSQL_KIND_PENDING);' \
  '  IndexDef def;
  GDEFSTOR_Result rc;
  gdefssql_name_index(index, tno, role, w->grp, w->col);
  index_of(tno, w->grp, w->col, role, &def);
  rc = create_index(c, def.unique, def.index, def.table, def.cols, def.ncols);
  return rc != GDEFSTOR_OK ? rc
                           : record(c, tno, w->grp, w->col, role, index, 0);'

# A drop of an index that is valid: every pending index dropped and built
# again, the one drop allowed widened from an invalid index to any.
plant drop-of-a-valid-index src/schema.c testSchema \
  'Index.AnIndexAlreadyBuiltIsMarkedAndNotDroppedOrRebuilt' \
  '    if (v[i].held == IX_VALID) {
      /* Built before, by a sweep that stopped before it marked it. */
      v[i].built = 1;
      built += 1;
      continue;
    }
    if (v[i].held == IX_INVALID) {
      rc = drop_invalid(c, def.index);
    }' \
  '    if (d->index_drop != NULL) {
      rc = drop_invalid(c, def.index);
    }'

# The total counted after the page: the page's length.
plant count-after-limit src/backend.c testAgreement \
  'Agreement.RandomQueriesMatchTheReferenceScan' \
  '    gdefssql_st_close(&s);
  }
  gdefssql_sb_free(&total);' \
  '    gdefssql_st_close(&s);
    out->total = out->count;
  }
  gdefssql_sb_free(&total);'

# The final tiebreak dropped from a sorted page.
plant tiebreak-removed src/compile.c testAgreement \
  'Agreement.RandomQueriesMatchTheReferenceScan' \
  '  /* The final tiebreak, ascending under any sort: the row. */
  gdefssql_sb_col(q->sb, main->e, "rid");' \
  '  if (query->sort_count == 0) {
    gdefssql_sb_col(q->sb, main->e, "rid");
  } else {
    q->sb->len -= 2;
    q->sb->text[q->sb->len] = '"'"'\0'"'"';
  }'

# No value sorted last ascending and first descending.
plant null-order-flipped src/compile.c testAgreement \
  'Agreement.RandomQueriesMatchTheReferenceScan Agreement.RandomQueriesMatchTheReferenceScanInAnotherDialect' \
  '      int want_first = !s->descending;' \
  '      int want_first = s->descending;'

# Closing a version closes every version of the item: valid_to written again.
plant valid-to-twice src/backend.c testConformance \
  'ModelHistory.ValidToIsWrittenOnce' \
  '    bare_eq(&sb, " AND ", "vfrom", &from_rev);
    rc = gdefssql_cx_run(c, &sb);' \
  '    rc = gdefssql_cx_run(c, &sb);'

# A removed nested item stays in its current table.
plant current-diverges src/backend.c testConformance \
  'ModelDerived.RebuildGivesTheSameCurrentState' \
  '  if (rc == GDEFSTOR_OK) {
    gdefssql_name_table(t, tno, GDEFSSQL_ROLE_CURRENT, cl->group);' \
  '  if (rc == GDEFSTOR_OK && cl->group == 0) {
    gdefssql_name_table(t, tno, GDEFSSQL_ROLE_CURRENT, cl->group);'

# Erasure that destroys the key and writes no ledger row: the subject reads
# as having no key instead of as erased, and nothing records the erasure for
# a restore to replay.
plant erasure-without-ledger src/keystore.c testKeystore \
  'KeyStore.ErasureIsFinalAndLedgered KeyStore.ErasureDeletesTheKeyRowAndWritesTheLedgerRow' \
  '    rc = insert(c, "dsk_ledger", kv, 3);' \
  '    (void)kv;'

# Erasure that writes the ledger row and keeps the key row: the API reads the
# subject as erased, so only the table shows the key is still stored.
plant erasure-keeps-the-key src/keystore.c testKeystore \
  'KeyStore.ErasureDeletesTheKeyRowAndWritesTheLedgerRow' \
  '    rc = gdefssql_cx_run(c, &sb);
    gdefssql_sb_free(&sb);
  }
  if (rc == GDEFSTOR_OK) {
    rc = delete_subject(c, "dsk_sealed", subject);' \
  '    gdefssql_sb_free(&sb);
  }
  if (rc == GDEFSTOR_OK) {
    rc = delete_subject(c, "dsk_sealed", subject);'

# The blind index (story S5a). A MAC written into a revision row: the first
# blind row's MAC as the revision's reason.
plant mac-in-revision-row src/backend.c testBlind \
  'BlindSql.NoMacIsInAnyTableButTheBlindTable' \
  '    kv[5].v = dbytes(GDEFSSQL_DATUM_TEXT, r->why, r->why_len);' \
  '    kv[5].v = a->blind_count > 0
                  ? dbytes(GDEFSSQL_DATUM_TEXT, a->blinds[0].mac,
                        GDEFSTOR_BLIND_MAC_BYTES)
                  : dbytes(GDEFSSQL_DATUM_TEXT, r->why, r->why_len);'

# The same defect, seen through the port by the conformance suite's dumps.
plant mac-in-revision-row-dump src/backend.c testConformance \
  'ModelBlind.NoMacIsInARevisionGroupOrCurrentRow' \
  '    kv[5].v = dbytes(GDEFSSQL_DATUM_TEXT, r->why, r->why_len);' \
  '    kv[5].v = a->blind_count > 0
                  ? dbytes(GDEFSSQL_DATUM_TEXT, a->blinds[0].mac,
                        GDEFSTOR_BLIND_MAC_BYTES)
                  : dbytes(GDEFSSQL_DATUM_TEXT, r->why, r->why_len);'

# Blind equality matching an older version: the blind row joined to the
# item's current row by item alone, not by the version it indexes.
plant blind-equality-older-version src/compile.c testConformance \
  'ModelBlind.EqualityMatchesOnlyTheCurrentVersion' \
  '  gdefssql_sb_kw(q->sb, " AND ");
  gdefssql_sb_col(q->sb, c, "vfrom");
  gdefssql_sb_kw(q->sb, " = ");
  gdefssql_sb_col(q->sb, a, "vfrom");' \
  ''

# Blind deletion that misses one type: the last type with a blind table.
plant blind-deletion-misses-a-type src/backend.c testConformance \
  'ModelBlind.DeletionRemovesASubjectsBlindRowsInEveryType' \
  '  for (i = 0; rc == GDEFSTOR_OK && i < n; i++) {
    char t[GDEFSSQL_NAME_CAP];
    GDEFSSQL_Datum v = dbytes(GDEFSSQL_DATUM_TEXT, subject, subject_len);' \
  '  for (i = 0; rc == GDEFSTOR_OK && i + 1 < n; i++) {
    char t[GDEFSSQL_NAME_CAP];
    GDEFSSQL_Datum v = dbytes(GDEFSSQL_DATUM_TEXT, subject, subject_len);'

# The blind table cleared by a rebuild, as if it were derived.
plant rebuild-clears-blind src/backend.c testConformance \
  'ModelBlind.RebuildLeavesTheBlindTableAlone' \
  '      rc = rebuild_group(c, tno, &cat.groups[i]);
    }' \
  '      rc = rebuild_group(c, tno, &cat.groups[i]);
    }
    if (rc == GDEFSTOR_OK && cat_blind(&cat)) {
      gdefssql_name_table(t, tno, GDEFSSQL_ROLE_BLIND, 0);
      rc = delete_all(c, t);
    }'

# Records, counters and the outbox (story S6a) --------------------------------

# The write's savepoint taken lazily, at its first counter: what the write did
# before it is outside what an abort undoes, so a record written then is kept
# by the abort that keeps the counter.
plant record-outside-transaction src/backend.c testConformance \
  'ModelRecords.AnAbortedWriteLeavesNoRecord' \
  '      rc = txn_sql(&c, b->sp_set);' \
  '      rc = GDEFSTOR_OK;' \
  '  rc = seq_write(&x->cx, kind, name, name_len, found, "last", base + 1);' \
  '  if (x->ntaken == 0 && kind == GDEFSTOR_MODEL_COUNTER) {
    rc = txn_sql(&x->cx, x->b->sp_set);
  }
  if (rc == GDEFSTOR_OK) {
    rc = seq_write(&x->cx, kind, name, name_len, found, "last", base + 1);
  }'

# An abort that rolls everything back, the counters too: the value an aborted
# write took is handed out again.
plant counter-reuses-aborted-value src/backend.c testConformance \
  'ModelCounters.AnAbortedWritesValueIsSkippedNotReused' \
  '  if (x->ntaken == 0) {
    return txn_sql(&x->cx, b->d.rollback);
  }' \
  '  if (x->ntaken == 0 || 1) {
    return txn_sql(&x->cx, b->d.rollback);
  }'

# A claim that takes the stream's newest rows first.
plant claim-out-of-order src/backend.c testConformance \
  'ModelOutbox.ThreeAppendsAreClaimedInSequence' \
  '    gdefssql_sb_kw(&sb, " ORDER BY ");
    gdefssql_sb_col(&sb, "a0", "seq");
    gdefssql_sb_kw(&sb, " LIMIT ");' \
  '    gdefssql_sb_kw(&sb, " ORDER BY ");
    gdefssql_sb_col(&sb, "a0", "seq");
    gdefssql_sb_kw(&sb, " DESC LIMIT ");'

# The live-lease count read and then not looked at.
plant double-claim-under-live-lease src/backend.c testConformance \
  'ModelOutbox.ASecondClaimerDuringALiveLeaseGetsNothing' \
  '  if (rc != GDEFSTOR_OK || live > 0) {' \
  '  if (rc != GDEFSTOR_OK) {'

# A row that reaches the threshold deleted instead of parked.
plant parked-row-deleted src/backend.c testConformance \
  'ModelOutbox.AFailingRowIsParkedCountedNeverClaimedAndKept' \
  '    parked = fails + 1 >= (int64_t)park_at;' \
  '    parked = fails + 1 >= (int64_t)park_at;
    if (parked) {
      int gone = 0;
      gcu_allocator_free(x->b->alloc, stream);
      *out_found = 1;
      *out_parked = 1;
      return m_outbox_delete(ctx, txn, public_id, public_id_len, &gone);
    }'

# Records treated as derived: a rebuild deletes them.
plant rebuild-clears-records src/backend.c testConformance \
  'ModelSystem.RebuildLeavesRecordsCountersAndTheOutboxAlone' \
  '    rc = delete_all(&x->cx, "dss_entity");' \
  '    rc = delete_all(&x->cx, "dss_entity");
    if (rc == GDEFSTOR_OK) {
      rc = delete_all(&x->cx, "dss_record");
    }'

# The language axis (story S6a2) -------------------------------------------

# A predicate reading another language: a field's subquery over the current
# table loses its language condition, so it reads every language's items.
plant language-other-read src/compile.c testConformance \
  'ModelLanguage.AQueryReadsOnlyItsLanguage ModelLanguageSweep.SeededHistoriesAgreeWithAModel' \
  '  gdefssql_sb_kw(q->sb, " WHERE ");
  in_language(q, a);
  gdefssql_sb_kw(q->sb, " AND ");
  if (l->kind == SRC_VALUES || r->kind == SRC_VALUES) {' \
  '  gdefssql_sb_kw(q->sb, " WHERE ");
  if (l->kind == SRC_VALUES || r->kind == SRC_VALUES) {'

# A sort reading another language: the sort's join to the root group loses
# its language, so an entity sorts by (and is listed once per) any language.
plant language-other-sort src/compile.c testConformance \
  'ModelLanguage.AQueryReadsOnlyItsLanguage ModelLanguage.AnEntityWithNoTreeInTheLanguageMatchesNoField' \
  '    gdefssql_sb_kw(q->sb, " AND ");
    in_language(q, "a2");' \
  ''

plant_finish
