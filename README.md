# VirtualMSSQL

VirtualMSSQL is a SQLite extension that exposes Microsoft SQL Server tables, views, approved query results, and spatial data as SQLite virtual tables. Once loaded, remote data can be accessed through standard SQLite SQL. It can be queried, filtered, ordered, joined with local tables, streamed or materialized, and, when a stable key is available — modified using INSERT, UPDATE, and DELETE within transactions and savepoints.

Currently, the extension is distributed as a single `virtualmssql.dll` for Win32 and Win64. It requires the [Microsoft ODBC Driver 18 for SQL Server](https://learn.microsoft.com/sql/connect/odbc/download-odbc-driver-for-sql-server) to be installed on the machine; no other third-party client DLLs are bundled or required.

VirtualMSSQL is primarily intended for Windows applications that already use SQLite but also need secure, transactional access to SQL Server data without implementing a separate database access layer or middleware. It supports streaming reads, safe query pushdown, query materialization, credential providers (including Windows Credential Manager), strict TLS verification, explicit transactions and savepoints, cancellation, connection pooling, metadata caching with schema-integrity checks, and spatial columns as WKB/WKT.

## Features

- SQL Server 2019, 2022, and 2025; SQLite host 3.44.0 or later with loadable-extension support.
- Tables, views, and read-only query sources (`source='query'`) with a bounded T-SQL lexer and read-only validator.
- Catalog metadata, type registry with a deterministic `UNSUPPORTED_TYPE` policy, and stable identity (integer / string / GUID / composite keys).
- Proven-safe pushdown: projection, predicate, single-value `IN`, ordering, and `LIMIT`/`OFFSET`; everything else is rechecked locally.
- DML for tables with a stable key; identity, computed, and rowversion columns remain server-owned.
- Explicit transactions and savepoints on one canonical SQL Server identity; non-cancellable finalization with UNKNOWN-OUTCOME quarantine.
- Streaming reads (LOBs included), independent cursors for parallel scans, bounded connection pool with clean-state verification.
- Cancellation (`virtualmssql_cancel()`), login/query timeouts, monotonic operation deadlines, and a conservative read-only retry policy.
- Materialization into a private SQLite database (`memory` or `temp`) with atomic publish.
- Metadata cache (`metadata_mode='cached'`) with schema fingerprints, stale/drift/corrupt policies, and `xIntegrity` self-checks.
- Credential providers (in-memory zero-on-free and Windows Credential Manager) with secret redaction in all diagnostics.
- Spatial columns (`geometry`/`geography`) projected as WKB or WKT via `STAsBinary()`/`STAsText()`.
- Quality gates: PVS-Studio, MSVC `/analyze`, ASan+UBSan, deterministic fuzzing with fault injection, and a full compatibility matrix.

## Requirements

- Windows 10+ (Win32 or x64).
- [Microsoft ODBC Driver 18 for SQL Server](https://learn.microsoft.com/sql/connect/odbc/download-odbc-driver-for-sql-server) installed (18.6.x tested).
- A SQLite host of version 3.44.0 or later that supports loadable extensions.
- Microsoft Visual C++ Redistributable 2015–2022.
- A reachable SQL Server 2019/2022/2025 instance and a login with the permissions you expect (server ACLs and permissions remain in effect).

Mandatory limitations (deterministic behavior, no best-effort fallbacks):

- No distributed atomicity between SQLite and SQL Server, or between different SQL Server identities.
- No MARS: all access on one connection is serialized by the extension.
- Types without a lossless mapping (`sql_variant`, `hierarchyid`, ...) fail the virtual table at CREATE time with `UNSUPPORTED_TYPE`.
- DML/COMMIT/ROLLBACK and partially exposed streams are never retried.
- `tls=verify` is the default; trusting arbitrary server certificates must be explicitly configured (`tls=trust`).

## Quick Start

Build all configurations and run the test suites (a live SQL Server is required for most suites; the connection profile is provided through `VMS_TEST_PROFILE`):

```pwsh
# from a Visual Studio developer prompt
cmake --preset x64-release
cmake --build --preset x64-release

$env:VMS_TEST_PROFILE = 'server=localhost,1433;auth=sql;cred=test;tls=trust'
& .\build\x64-release\tests\seed_r6.exe          # create test fixtures on the server
ctest --preset x64-release
```

Performance qualification and sanitizer builds are separate presets/scripts:

```pwsh
cmake --build --preset asan-x64                  # AddressSanitizer + UBSan build
.\build\x64-release\tests\bench_r15.exe          # performance qualification
```

See `docs/compatibility-matrix.md` for the qualified server/auth/TLS matrix and `docs/stage-log.md` for the per-stage gate history (R0–R18).

## Example

```sql
-- one-time setup per connection: credentials and the connection profile
SELECT virtualmssql_cred('app/reporting:uid', 'app_user');
SELECT virtualmssql_cred('app/reporting:pwd', '<secret>');
SELECT virtualmssql_profile('server=srv01,1433;db=app;auth=sql;cred=app/reporting;tls=verify');

-- expose a remote table
CREATE VIRTUAL TABLE temp.orders USING virtualmssql(
  schema='dbo', table='orders', mode='rw'
);

-- read: pushdown of projection, predicate and LIMIT; everything rechecked locally
SELECT order_id, created_at
FROM orders
WHERE status = 2
ORDER BY order_id
LIMIT 10;

-- write: values are sent as literals generated from validated identifiers only
INSERT INTO orders(id, name, val) VALUES(42, 'acme', 7);
UPDATE orders SET val = 9 WHERE id = 42;
DELETE FROM orders WHERE id = 42;

-- a read-only query source (a single SELECT, validated before use)
CREATE VIRTUAL TABLE temp.stats USING virtualmssql(
  source='query', query='SELECT region, SUM(amount) AS total FROM dbo.orders GROUP BY region'
);
```

Spatial columns are projected as WKB by default (`spatial='wkb'`) or as WKT (`spatial='wkt'`); metadata can be cached with `metadata_mode='cached'`.

## Virtual Table Arguments

| Argument | Values | Default | Description |
|---|---|---|---|
| `schema=` | identifier | — | Remote schema (table sources). |
| `table=` | identifier | — | Remote table (table sources). |
| `source=` | `table` / `query` | `table` | Table source or validated query source. |
| `query=` | T-SQL SELECT | — | Required for `source='query'`. |
| `mode=` | `ro` / `rw` | `ro` | `rw` enables DML for tables with a stable key. |
| `materialization=` | `off` / `memory` / `temp` | `off` | Query-source snapshot mode. |
| `spatial=` | `wkb` / `wkt` | `wkb` | Representation of `geometry`/`geography` columns. |
| `metadata_mode=` | `live` / `cached` | `live` | `cached` consults the shadow cache with live validation. |
| `conn=` | key | — | Profile key resolved through a registered query-profile provider. |

## Remote Schema Inspection

Four read-only, eponymous table functions inspect the remote SQL Server schema through ordinary SQLite queries:

```sql
SELECT * FROM virtualmssql_tables('server=srv;auth=sql;cred=app;tls=verify', 'dbo');

SELECT * FROM virtualmssql_table_info(
  'server=srv;auth=sql;cred=app;tls=verify', 'dbo', 'orders');

SELECT * FROM virtualmssql_index_list(
  'server=srv;auth=sql;cred=app;tls=verify', 'dbo', 'orders');

SELECT * FROM virtualmssql_index_info(
  'server=srv;auth=sql;cred=app;tls=verify', 'dbo', 'orders', 'PK_orders');
```

The first argument is a comma-separated runtime connection specification in the `virtualmssql_profile` grammar. Remote identifiers are validated and inlined as N''-quoted literals; user input never enters SQL as raw text. The functions are direct-only: invoke them from top-level SQL.

| Function | Visible result columns |
|---|---|
| `virtualmssql_tables(connection, schema)` | table_schema, table_name, table_type |
| `virtualmssql_table_info(connection, schema, table)` | cid, name, type, notnull, dflt_value, pk, ordinal, max_length, precision, scale, is_nullable, is_identity, is_computed, hidden_flags |
| `virtualmssql_index_list(connection, schema, table)` | seq, name, unique, origin, partial, is_disabled, column_count |
| `virtualmssql_index_info(connection, schema, table, index)` | seqno, cid, name, desc, collation, key, is_primary_key, is_unique, is_disabled, key_ordinal, is_nullable |

The SQLite-style columns follow the conventions of the corresponding PRAGMA table functions. For `table_info`, `pk` is the one-based position inside the primary key (0 if not in the PK), and `hidden_flags` is 0 for ordinary columns or 2 for computed columns. For `index_list`, `origin` is `pk`, `u`, or `c`. Remote metadata comes from `sys.columns`, `sys.indexes`, and `sys.index_columns`.

## SQL Functions

| Function | Description |
|---|---|
| `virtualmssql_version()` | Extension version string. |
| `virtualmssql_profile('spec')` | Set the connection profile for this process. |
| `virtualmssql_cred('key:uid', 'v')` | Provision a secret under `key:uid` / `key:pwd`. |
| `virtualmssql_cancel()` | Cancel all in-flight remote operations and interrupt the VM. |

## Public API (ABI v1)

Hosts may link `virtualmssql.dll` directly (see `include/virtualmssql/vms_api.h`):

| Export | Description |
|---|---|
| `virtualmssql_api_version()` | Returns the public ABI version (currently 1). |
| `virtualmssql_register_credential_provider(p)` | Register a `VmsCredProviderV1` credential provider. |
| `virtualmssql_register_query_profile_provider(p)` | Register a query-profile provider (`conn='key'` resolution). |
| `virtualmssql_wincred_provider()` | Built-in Windows Credential Manager provider instance. |
| `virtualmssql_cancel(db)` | Cancel in-flight remote operations; interrupts the VM. |

## Documentation

| Section | Documents |
|---|---|
| Quality and release | [Compatibility matrix](docs/compatibility-matrix.md), [Stage log G0–G18](docs/stage-log.md) |
| Research | [R0 ODBC probe results](docs/research/r0-probe-results.md) |
| Specification | [Technical specification v1.0 (TZ)](01_TZ_VirtualMSSQL.md), [Roadmap R0–R18](02_ROADMAP_VirtualMSSQL.md), [Research notes](03_RESEARCH_NOTES_AND_SOURCES.md) |
| Examples | [load_smoke.c](examples/load_smoke.c) |

## Dependencies

Runtime dependencies of `virtualmssql.dll` (verified with Dependencies/depends for both architectures):

| Module | Source | Notes |
|---|---|---|
| `ODBC32.dll` | Windows | ODBC Driver Manager; loads Microsoft ODBC Driver 18 (`msodbcsql18.dll`) — **required, installed separately**. |
| `ADVAPI32.dll` | Windows | Windows Credential Manager (`CredReadW`). |
| `KERNEL32.dll` | Windows | Workers, heap, interlocked, critical sections. |
| `sqlite3.dll` or host SQLite | Host application | SQLite 3.44.0+; **not bundled** — the host that loads the extension provides it. |
| `VCRUNTIME140.dll`, `api-ms-win-crt-*` | VC++ Redistributable / UCRT | Standard MSVC runtime. |

## Runtime Support

Currently, Windows is the only supported runtime. The extension follows the architectural approach of the sibling projects [VirtualPostgreSQL](https://github.com/alexeydott/VirtualPostgreSQL) and [VirtualMySQL](https://github.com/alexeydott/VirtualMySQL).

## License

See `licenses/THIRD-PARTY.md` for third-party notices (SQLite headers and test binaries; the SQLite source itself is public domain). The project license is defined at the repository root.
