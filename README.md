# defiant-storage-sql

The part of every SQL storage backend that does not depend on the engine.
`defiant-storage` defines the entity port (`model.h`) and what a backend
implements to serve it (`model_backend.h`); this library implements all of
that once, for any SQL engine: the physical schema a type declaration becomes,
the generated names and their mapping table, the additive schema steps, the
statements for every backend operation, and the compiler from the port's query
form to SQL. It executes nothing. A backend (`defiant-storage-sqlite`,
`defiant-storage-postgres`) gives it a **dialect** (`dialect.h`: quoting,
placeholders, type names, collation, NULL order, upsert form, limits, the
transaction statements) and an **executor** (`executor.h`: open a tenant's
connection, prepare, bind, step, read a column, finalize, exec), and gets a
complete `GDEFSTOR_ModelBackend` back.

```c
#include <ghoti.io/defiant-storage-sql/defiant-storage-sql.h>

GDEFSTOR_ModelBackend model;
GDEFSSQL_Result rc = gdefssql_model_backend(&my_dialect, &my_executor,
    NULL, &model);
/* then gdefstor_store_create_model(&key_bytes_backend, &model, ...) */
```

Nothing a caller wrote reaches SQL text: every table and column is a generated
name (`t3_g2`, `v5`) recorded in `dss_name`, and every value, the page
included, is a bound parameter. Text compares by bytes through the dialect's
column collation; no value sorts first ascending and last descending, with
`NULLS FIRST`/`NULLS LAST` written where the engine's default differs; the
entity row is the final tiebreak; the total is counted before the page. A
query outside the port's form is refused, never answered differently from
`gdefstor_model_scan()`, the port's reference engine. The compiler is public
(`compile.h`) so that a backend can look at what it runs.

documentation/sql.md has the schema column by column, every statement form,
the dialect and executor contracts, and what each later backend must provide.

## Building

Depends on `defiant-storage` through pkg-config, and on nothing else: no
engine, by rule (`check-seams`).

```bash
make test PREFIX=/path/to/prefix
```

## Tests

There is no database in the tests. `tests/sql_stub.h` is a small, strict SQL
engine that parses and runs exactly the statement forms this library writes,
in two test dialects: one SQLite-like, and one that differs in everything a
dialect may say (bracket quotes, numbered `:N` parameters, its own type names
and collation, NULL sorting high, the MySQL upsert, `RETURNING`, a schema per
tenant). Every statement it runs is linted first: a digit, a quote, a word
outside the fixed vocabulary or an identifier that is not a generated name is
an error, so "every value is bound" is checked on every statement of every
test. It scans newest first and keeps ties in that order, so a query that
relies on an order it did not ask for answers wrongly.

- `testConformance`, `testConformance_fake`: the port's entity conformance
  suite (`model.inc`) on this library, in each dialect, with every query also
  answered by the reference engine over the same rows and compared.
- `testAgreement`: seeded random data and queries, compiled and reference
  answers compared, with counts showing the sweep met ties, absent sort keys,
  descending keys, later pages, clamped limits, relations and negations.
- `testQuery`: the compiled text of the form in each dialect, every constant a
  parameter, what is refused, the parameter limit, NULL placement, the
  tiebreak.
- `testSchema`: the schema of a nested declaration, steps taken once and only
  additively, hostile names, the column limit, layout versions, an engine
  error part way through a write.

`make test` also runs `check-sql`, which plants each defect the story names (an
unvalidated identifier, a concatenated value, a dropped WHERE clause, a
non-additive step, count after limit, the tiebreak removed, NULL order
flipped, plus a `valid_to` written twice and a current table left stale) in a
scratch copy of the source and requires a named test to fail for each;
`check-labels`, `check-seams` and `check-symbols`, each with plants of its own.

## Status

Built in story S2 of the storage rewrite. The SQLite (S3) and Postgres (S4)
backends that use it are not built yet.
