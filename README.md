# defiant-storage-sql

The part of every SQL storage backend that does not depend on the engine: the
physical schema a type declaration becomes, generated names and their mapping
table, additive schema steps, the statements for each operation of the
entity backend (`defiant-storage/model_backend.h`) and the compiler from the
port's query form to SQL, behind a dialect each engine fills in. It executes
nothing itself.

This is the library's first commit: its layout, build, result codes and
gates. The rest follows.

## Building

Depends on `defiant-storage` through pkg-config, and on nothing else.

```bash
make test PREFIX=/path/to/prefix
```

## Status

Being built (story S2 of the storage rewrite).
