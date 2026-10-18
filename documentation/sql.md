# The SQL layer

**Status:** describes what shipped in story S2 (2026-10-18), with the key
store and `gdefssql_store_version()` added in story S3, and index builds
outside the step, schemas made per tenant and `gdefssql_store_exists()` in
story S4.

This is the contract between `defiant-storage-sql` and the engine backends
that sit on it. The port's meaning is `defiant-storage`'s
(`documentation/model.md`, `documentation/query.md` there); this page says how
it is laid out in tables and statements, and what a dialect and an executor
must do.

## 1. Names

No identifier is built from caller text. Every identifier is one of:

| Form | What |
| --- | --- |
| `dss_schema`, `dss_counter`, `dss_type`, `dss_name`, `dss_entity`, `dss_draft` | the six tenant tables |
| `dss_type_tno`, `dss_entity_rid` | their unique indexes |
| `tN_e`, `tN_s`, `tN_r`, `tN_n` | type N's entity, state, revision and newest-state tables |
| `tN_gG`, `tN_cG` | type N's item versions and current items of group G |
| `tNp` | unique index on `tN_e (pid)` |
| `tN_iG_C` | index on `tN_cG (vC, ent)` for an indexed field |
| `tN_jG_C` | index on `tN_gG (vCs)`, the subject of a personal field |
| `vC`, `vCs` | column C of a group, and its subject column |
| fixed column names | `rid pid creator created rev last_item seq pub del actor at cseq who why ent item vfrom vto parent ord id version last_row last_type last_cseq name tno decl grp col role kind type base body` |
| `aK` | a table alias in one statement |
| `dsk_schema`, `dsk_counter`, `dsk_key`, `dsk_ledger`, `dsk_ledger_seq` | the key store's tables and index (section 8), never in a data store |
| `subject secret last_seq` | the key store's column names |

N is the type's number, given out by `dss_counter.last_type` the first time a
type is declared; G and C are the port's stable group and column numbers
(`GDEFSTOR_FieldInfo`). The longest name is 34 bytes, within every engine's
limit (Postgres, 63). `gdefssql_sb_ident()` refuses anything that is not
`[a-z][a-z0-9_]*` within the dialect's limit, so even a defect elsewhere cannot
splice caller bytes in as a name. Each name is quoted with the dialect's quotes
and, for a tenant with a qualifier (a Postgres schema), written after it.

`dss_name (tno, grp, col, role, name, kind)` maps every generated table,
column and index to what it holds: `role` is 1 entity, 2 state, 3 revision,
4 newest state, 5 item versions, 6 current items, 7 value column, 8 subject
column, 9 field index, 10 subject index, 11 the entity lookup `tNp`, 12 one
of the tenant tables' two indexes (`tno` 0; `col` 0 `dss_type_tno`, 1
`dss_entity_rid`); `kind` is a value column's logical type, plus 256 for a
personal field, and an index's 0 once it is built, 512 while it is pending
(section 3).

## 2. Tables

Per tenant:

| Table | Columns | Key |
| --- | --- | --- |
| `dss_schema` | `id`, `version` (1) | `id` |
| `dss_counter` | `id`, `last_row`, `last_type`, `last_cseq` | `id` |
| `dss_type` | `name` TEXT, `tno`, `decl` BYTES (the port's declaration text) | `name` |
| `dss_name` | as above | `tno, grp, col, role` |
| `dss_entity` | `pid` REFERENCE, `rid`, `tno` (derived) | `pid` |
| `dss_draft` | `pid` REFERENCE, `type` TEXT, `base`, `body` BYTES | `pid` |

Per type N, truth:

| Table | Columns | Key |
| --- | --- | --- |
| `tN_e` | `rid`, `pid`, `creator` (NULL for the system), `created`, `rev` (highest), `last_item` | `rid` |
| `tN_s` | `rid`, `seq`, `pub`, `del`, `actor`, `at` | `rid, seq` |
| `tN_r` | `rid`, `rev`, `cseq`, `who`, `at`, `why` | `rid, rev` |
| `tN_gG` | `ent`, `item`, `vfrom`, `vto` (NULL while open), `parent`, `ord`, `vC`... | `ent, item, vfrom` |

and derived, written in the same transaction and rebuilt from the truth by
`rebuild`:

| Table | Columns | Key |
| --- | --- | --- |
| `tN_n` | `rid`, `pub`, `del`: the newest state | `rid` |
| `tN_cG` | `ent`, `item`, `vfrom`, `parent`, `ord`, `vC`...: the open versions | `ent, item` |

Group 0 is the root group; a multiple scalar field is a one-column group whose
column is 0. A personal field's value column holds the ciphertext (BYTES) and
`vCs` its subject. The draft table is per tenant, not per type, because the
port names a draft by public id alone and a draft can reserve an id that has
no entity yet.

Rows are numbered by `dss_counter.last_row`, the commit sequence by
`last_cseq`: a counter row read and advanced inside the write, the same way on
every engine. No identity column, sequence or last-insert id is used.

## 3. Schema steps

A step is one of three forms, and nothing else:

```
CREATE TABLE IF NOT EXISTS t (c TYPE [NOT NULL], ..., PRIMARY KEY (k, ...))
ALTER TABLE t ADD COLUMN c TYPE
CREATE [UNIQUE] INDEX IF NOT EXISTS i ON t (c, ...)
```

Each is recorded in `dss_name` in the same transaction, and a step whose record
exists is not taken again: declaring the same tree twice runs no DDL, and
adding a field runs one `ADD COLUMN` on its truth table and one on its current
table (and, for a personal field, the same for its subject column). Nothing is
dropped, renamed or retyped: a retired field's column stays, an index whose
field is no longer indexed stays, and a column recorded with another kind than
the declaration gives is `GDEFSTOR_ERR_CORRUPT`, never a conversion. A
declaration that would exceed the dialect's column limit is
`GDEFSTOR_ERR_LIMIT` before any DDL runs. The first write of a tenant in a
backend's life creates the six tenant tables in a transaction of its own, then
checks `dss_schema.version`: another version is refused
(`GDEFSTOR_ERR_UNSUPPORTED` for a newer one, `GDEFSTOR_ERR_FORMAT` otherwise).

The steps of a declaration run inside the write that declares it, so a dialect
must have transactional DDL (SQLite and Postgres do). A dialect without it is
refused: running and recording each step apart, as MySQL would need, is not
built.

**Indexes are the exception (AD-15): no `CREATE INDEX` of a data store runs
inside a transaction.** An index's step only records it, pending (`kind`
512), in the declaring write. After that write commits, on the same
connection and outside any transaction, every pending index of the tenant is
built:

```
CREATE [UNIQUE] INDEX <index_build>IF NOT EXISTS i ON t (c, ...)
```

with the dialect's `index_build` word (`CONCURRENTLY ` on Postgres; none on
SQLite), and then marked built (`kind` 0) in a short transaction of its own,
once it exists and, where the dialect has `index_valid`, the engine says it is
valid. A concurrent build that fails leaves an invalid index; the next sweep
finds it with `index_valid`, drops it with `index_drop` (`DROP INDEX
CONCURRENTLY IF EXISTS`) and builds it again. That is the only drop this
library writes, and only of an index the engine says is invalid: such an index
holds nothing and serves no query. An index that is valid but still recorded
pending (a backend stopped between the build and the mark) is marked, never
dropped or built again.

A failed build is not the declaring write's failure: the write is committed,
the build's connection is discarded, and the index stays pending. Pending
builds are tried again after the backend's next commit of the tenant and on
the first write of the tenant in every backend's life. The tenant tables' two
indexes and each type's `tNp` are recorded and built the same way. Until an
index is built its field reads `GDEFSTOR_INDEX_INDEXING`
(`gdefstor_type_index_state()`, answered from `dss_name`), and queries on it
answer exactly the same, by reading the table.

The key store (section 8) is a port of its own: its one index,
`dsk_ledger_seq`, is made with its table in the key store's first
transaction, when that table is new and empty.

## 4. Statements

Besides the steps, every statement is one of these forms. Every value is a
parameter; the only other words are the fixed vocabulary.

```
INSERT INTO t (c, ...) VALUES (?, ...)
INSERT INTO t (c, ...) VALUES (?, ...) ON CONFLICT (k) DO UPDATE SET c = EXCLUDED.c, ...
INSERT INTO t (c, ...) VALUES (?, ...) ON DUPLICATE KEY UPDATE c = VALUES(c), ...
INSERT INTO t (c, ...) SELECT a0.c, ... FROM g a0 WHERE a0.vto IS NULL
UPDATE t SET c = ?, ... WHERE c = ? [AND c = ?]...
UPDATE dss_counter SET c = c + ? WHERE id = ? RETURNING c
DELETE FROM t [WHERE c = ? [AND c = ?]]
SELECT a0.c, ... FROM t a0 [JOIN t a1 ON ...] [WHERE ...] [ORDER BY ...] [LIMIT ? OFFSET ?]
SELECT COUNT(*) FROM ...
```

| Operation | Statements |
| --- | --- |
| `begin` | `begin_write` (first time per tenant: the tenant tables first, then the index sweep) |
| `type_load` | `SELECT decl FROM dss_type WHERE name = ?` |
| `type_store` | the type number, the upserted declaration, the steps (indexes recorded pending) |
| `commit` | the dialect's `commit`; then, when a step recorded an index or one is still pending, the index sweep (section 3) |
| index sweep | in a write transaction: `SELECT tno, grp, col, role FROM dss_name WHERE kind = ?` and `index_valid` per index; outside one: `index_drop` of an invalid index, `CREATE INDEX`; in a write transaction: `index_valid`, `UPDATE dss_name SET kind = ? WHERE tno = ? AND grp = ? AND col = ? AND role = ?` |
| `index_state` | `dss_type` by name, then `dss_name` of the type, in a read |
| `entity_find` | `dss_entity` by `pid`, then `tN_e JOIN tN_n` by `rid` |
| `entity_insert` | the row counter; `tN_e`, `tN_n`, `dss_entity` rows |
| `state_append` | the last `seq`; a `tN_s` row; `tN_n` updated |
| `apply` | the commit sequence (once per write); for each close, the open version is read (it must be open), `vto` set, the current row deleted; each new version inserted into `tN_gG` and `tN_cG`; the `tN_r` row; `tN_e.rev` and `last_item` |
| `history` | `tN_r` and `tN_s` by `rid` |
| `items` | each `tN_cG` by `ent`, or each `tN_gG` by `ent` and `vfrom <= ? AND (vto IS NULL OR vto > ?)` |
| drafts | upsert, select, delete on `dss_draft` |
| `query` | the compiled total and page, in one read transaction |
| `rebuild` | `dss_entity` and every `tN_n` and `tN_cG` emptied and refilled from the truth |
| `dump` | every table read in key order |

A read without the write's transaction opens a read connection and runs
`begin_read`, so everything one call reads is one snapshot.

## 5. The query compiler

The form is the port's (query.h, "Over entities"). Each piece and what it
becomes, with entity alias `a0` and its newest state `a1`:

| Port | SQL |
| --- | --- |
| FIELD op VALUES | `a0.rid IN (SELECT c.ent FROM tN_cG c WHERE c.vC op ? OR ...)`, or `c.vC IN (?, ...)` for equality against several |
| FIELD op META/ENTITY | the same, with the scalar column in place of `?` |
| FIELD op FIELD | `EXISTS (SELECT NULL FROM tN_cG x JOIN tM_cH y ON y.ent = owner.rid WHERE x.ent = owner.rid AND x.vC op y.vD)` |
| META/ENTITY op VALUES | `(col op ? OR ...)`, guarded by `col IS NOT NULL` for the creator |
| VALUES op VALUES | decided by the compiler: `(NULL IS NULL)` or `(NULL IS NOT NULL)` |
| `!=` | `(NOT (... = ...))`: no pair is equal |
| `in` | `=` against the list; an empty list is false |
| and, or, not | `(... AND ...)`, `(... OR ...)`, `(NOT ...)`; an empty and is true, an empty or false |
| EXISTS T | `EXISTS (SELECT NULL FROM tM_e x JOIN tM_n y ON y.rid = x.rid WHERE ...)`; OUTER operands name `a0` |
| sort key | a root field through `LEFT JOIN tN_c0 a2 ON a2.ent = a0.rid`, or a fact; `DESC`; `NULLS FIRST`/`LAST` where the engine differs |
| tiebreak | `a0.rid` ascending, last, always |
| page | `LIMIT ? OFFSET ?`; "every match" is `INT64_MAX` |
| total | `SELECT COUNT(*)` of the same predicate, no order, no page |

Every comparison is TRUE or FALSE, never UNKNOWN, which is what makes SQL's
`NOT` the port's `not`: an `IN` whose left side is a key and whose subquery
selects keys cannot be UNKNOWN, nor can `EXISTS`, and the one nullable fact
(the creator) is guarded. A field value that is NULL makes no pair, as an
absent value adds nothing to the port's set.

Refused with `GDEFSSQL_ERR_UNSUPPORTED` (through the backend,
`GDEFSTOR_ERR_UNSUPPORTED`): COLUMN and CONST operands, a select, a numeric
sort or tiebreak, more than four sort keys, a sort on anything but a single
field of the root group, a fact or the public id, a personal, retired or group
field, an EXISTS inside an EXISTS, an OUTER operand outside one, an unknown
node, operand, fact or flag, and two sides of different types or decimal
scales. A statement with more parameters than the dialect allows is
`GDEFSSQL_ERR_LIMIT`.

## 6. Values

| Logical type | Column kind | Parameter |
| --- | --- | --- |
| text | TEXT, with the dialect's collation | UTF-8 bytes |
| integer, boolean, date, datetime | INTEGER, BOOLEAN, DATE, DATETIME | int64 |
| decimal | DECIMAL: the unscaled int64 at the field's scale | int64 |
| float | FLOAT | double, exactly; -0.0 is stored as 0.0 |
| reference | REFERENCE, with the collation | UTF-8 bytes |
| personal (any type) | BYTES, and a REFERENCE subject column | bytes |

A NULL column is no value. A value read back has the field's logical type; a
row written before a column existed reads that column as no value, and
trailing no-values are dropped.

## 7. For the engine backends

What S3 (SQLite) and S4 (Postgres) supply, and what they must know:

- **The dialect.** SQLite: `"` quotes, `?`, `INTEGER`/`REAL`/`TEXT`/`BLOB`,
  no collation (BINARY compares bytes), `nulls_low` 1, ON CONFLICT (3.24+),
  `RETURNING` from 3.35, `BEGIN IMMEDIATE` for writes. Postgres: `"` quotes,
  `$` numbered, `BIGINT`/`DOUBLE PRECISION`/`TEXT`/`BYTEA` (and `SMALLINT` or
  `BIGINT` for BOOLEAN, which is bound as an int64), `COLLATE "C"`,
  `nulls_low` 0, ON CONFLICT, `RETURNING`, a serializable `begin_write` and a
  repeatable-read read-only `begin_read`; and the S4 members: `index_build`
  `CONCURRENTLY `, `index_valid` (a catalog query on `pg_index.indisvalid`),
  `index_drop`, `qualifier_create` `CREATE SCHEMA IF NOT EXISTS `, and
  `store_exists` (a catalog query: whether the schema exists, has the
  version table, and how many tables it holds).
  SQLite leaves all five NULL.
- **The executor.** One connection per call is what the library asks for;
  pooling, statement caching by text (texts repeat exactly per tenant and
  shape) and discarding a connection closed with `discard` are the
  executor's. A tenant is selected by the connection (a SQLite file) or by
  the qualifier `open` returns (a Postgres schema, written in front of every
  table), never by session state (AD-14). A read `open` of a tenant with no
  store returns no connection.
- **One statement at a time.** Each statement is finalized before the next
  is prepared or exec'd on the same connection; the test engine refuses a
  second one, so the whole suite holds the library to it.
- **Statement counts.** An operation looks up its type number (`dss_type` or
  `dss_entity`) and catalog (`dss_name`) on every call; nothing is cached
  across calls, so nothing goes stale when another process declares a type.
  S3's rows-scanned assertions should count these.
- **Indexes.** A field filter is written as `a0.rid IN (SELECT c.ent ...)` so
  that an engine can drive it from `tN_iG_C (vC, ent)`; a sort on a root
  field uses `tN_c0`; nothing relies on a partial index.
- **Refusing a store it must not serve.** The backends check
  `dss_schema.version` on the first write of a tenant, never on a read. An
  executor that can see from the engine's catalog that the version table
  exists calls `gdefssql_store_version()` on each new connection, before
  `open` returns it, and refuses another version; the call is one `SELECT`
  through the executor itself. A store this library did not make (SQLite: a
  milestone-1 file, with `defiant_record` and no `dss_schema`) is the
  executor's to recognise from the catalog and refuse, before anything here
  can create a table in it. An executor whose missing store is a schema, not
  a file, calls `gdefssql_store_exists()` in `open` (the dialect's
  `store_exists`, one `SELECT`) to answer a read of a tenant with no store
  with no connection, as executor.h asks; a schema that holds tables but not
  the version table is `GDEFSTOR_ERR_FORMAT`, never written in.
- **Index builds run on the write connection after its commit**, outside any
  transaction, and may wait (a concurrent build waits for older
  transactions): the executor's session settings (Postgres: `lock_timeout`)
  bound that wait, and a build that gives up only leaves its index pending.
- **Not here:** blind indexes (S5), content-hash values (AD-14, later), and
  the old key/bytes tables (the backends keep them until S9).

## 8. The key store

`gdefssql_keystore_backend()` (keystore.h) serves the key store port
(`defiant-storage`'s keystore.h) from a dialect and an executor that reach a
database of its own, never a tenant's data store (AD-13, AD-19). Per tenant:

| Table | Columns | Key |
| --- | --- | --- |
| `dsk_schema` | `id`, `version` (1) | `id` |
| `dsk_counter` | `id`, `last_seq` | `id` |
| `dsk_key` | `subject` REFERENCE, `secret` BYTES | `subject` |
| `dsk_ledger` | `subject` REFERENCE, `seq`, `at` DATETIME; unique index `dsk_ledger_seq (seq)` | `subject` |

| Operation | Statements |
| --- | --- |
| first write per tenant | the four tables and the index, `IF NOT EXISTS`, and their rows, in a transaction of their own; another `dsk_schema.version` refused |
| create | in the write: the ledger row and the key row by `subject` (either makes it `GDEFSTOR_ERR_INVALID`), then `INSERT` of the key the cipher's randomness made |
| wrap, unwrap | in a read: the ledger row (erased), else the key row; the cipher seals or opens outside the database |
| erase, ledger append | in the write: the ledger row (present: nothing to do), `DELETE` of the key row, the counter, `INSERT` of the ledger row |
| ledger | `SELECT subject, at ... ORDER BY seq` |

A key read for a wrap is copied out, used and wiped; a key never reaches a
log, a statement's text or a data store. What the engine keeps of a deleted
key row (free pages, a write-ahead log, dead tuples) is the engine backend's
to clear.
