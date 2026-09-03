# VirtualMSSQL research package

Пакет содержит результаты архитектурного исследования для проекта **VirtualMSSQL** — SQLite loadable extension для доступа к Microsoft SQL Server через **Microsoft ODBC Driver 18 for SQL Server**.

Состав:

- `01_TZ_VirtualMSSQL.md` — техническое задание версии 1.0: назначение, архитектура, обязательные требования, ограничения, контракты корректности и критерии приёмки.
- `02_ROADMAP_VirtualMSSQL.md` — единая release-critical карта R0–R18: этап → обязательные работы → gate.
- `03_RESEARCH_NOTES_AND_SOURCES.md` — проверенные факты, архитектурные выводы, спорные места и первичные источники Microsoft/SQLite/GitHub.

## Базовое решение

Production backend версии 1.0:

```text
Microsoft ODBC Driver 18 for SQL Server
```

Runtime model:

```text
host application
    -> SQLite
    -> virtualmssql.dll
    -> Windows ODBC Driver Manager (odbc32.dll)
    -> Microsoft ODBC Driver 18 (msodbcsql18.dll)
    -> Microsoft SQL Server
```

На момент исследования актуальная Windows-ветка ODBC Driver 18 — **18.6.2.1**, дата выпуска **31 марта 2026**. Текущую версию перед релизом следует повторно проверять по Microsoft Learn.

## Статус требований

- `MANDATORY` — блокирует выпуск VirtualMSSQL 1.0.
- `EXPERIMENTAL` — не блокирует 1.0 и должен развиваться отдельно.
- `UNSUPPORTED` — должен завершаться детерминированной ошибкой, а не работать «best effort».

## Реализация (статус по этапам)

- **R0 — ODBC feasibility probe: Gate G0 PASS** (2026-09-01). Пробник `tools/vms-odbc-probe`, результаты: `docs/research/r0-probe-results.md`. Ключевые находки: потолок параметров 1999; отмена только через `SQLCancelHandle(SQL_HANDLE_STMT)` на публикуемый воркером statement; `ColumnSize=0` запрещён для LONG-параметров (HY104, рабочий максимум `2^30-1`); ленивое начало транзакции подтверждено; `SQL_COPT_SS_AUTOBEGINTXN = 1402`.
- **R1 — репозиторий/сборка/совместимость: Gate G1 PASS** (2026-09-01). `virtualmssql.dll` собирается в 4 конфигурациях (win32/x64 × debug/release, CMake + Ninja, `/W4`); экспорты `sqlite3_virtualmssql_init` + `sqlite3_extension_init`; runtime-гейт версии SQLite (baseline 3.44.0, без silent downgrade); стаб-модуль `virtualmssql_stub` + скаляр `virtualmssql_version()`; CTest-сьюты load/exports/capability/missing-driver — 4/4 PASS на всех конфигурациях; загрузка проверена через официальный sqlite3 CLI 3.53.4.
- **R2 — foundation core: Gate G2 PASS** (2026-09-01). Checked arithmetic (`vms_add_sz/mul_sz/add_i64/mul_i64` — все переполнения ловятся на обеих архитектурах); bounded-буферы с zero-on-free; UTF-8/UTF-16 с явными границами и отказом на invalid encoding (кириллица, non-BMP surrogate pairs, lone surrogates); ресурс-лимиты (10 `max_*`, дефолт `max_parameters=1999` по данным R0, понижение-only); логирование с безусловной redaction секретов (`PWD=`, `password=` — case-insensitive, утечка в sink невозможна); versioned fingerprints (FNV-1a, соление stage-тегом); fault allocator (OOM-инъекция в bounded-buffer путь). CTest `foundation` — PASS на всех 4 конфигурациях.
- **R3 — ODBC client runtime: Gate G3 PASS** (2026-09-02). Портативный слой `vms_client.h` (`VmsClient/Connection/Statement/Value/ColumnMeta/Error`) без единого ODBC-типа; весь ODBC изолирован в адаптере `vms_odbc_adapter.c` + connection-affine воркер `vms_odbc_worker.c` (один владелец HDBC/HSTMT, сериализация всех вызовов). Инварианты ТЗ подтверждены тестами: полная декодировка строки до видимости; инкрементные ординалы SQLGetData; `SQL_NO_DATA` как успех; статус-классификация (TRANSPORT→quarantine, HY008→CANCELLED); drain через `SQLMoreResults`; `vms_tran_begin/commit/rollback`; cross-thread cancel `vms_conn_cancel` (sqlite3_interrupt-семантика, `SQLCancelHandle(SQL_HANDLE_STMT)` — схема R0) с восстановлением соединения после отмены. Интеграционная сьюта `test_client` против SQL Server 2022 — PASS на всех 4 конфигурациях (6/6×4).
- **R4 — connections/credentials/TLS/pool: Gate G4 PASS** (2026-09-02). `credential_ref` + версионированный ABI провайдеров (v1, проверка версии, отказ при неизвестной); провайдеры: in-memory (zero-on-free) и Windows Credential Manager (`CredRead`, префикс `VirtualMSSQL/`, wipe после использования — секрет живёт только внутри scope `secret_begin/end`); профиль `VmsProfile` + **строгая грамматика** `vms_profile_parse` (неизвестные ключи отвергаются — `RetryExec/DSN/FileDSN/Trusted_Connection` структурно невпихнуть); строгий билдер `vms_connstr_build` — дефолт `tls=verify` (Encrypt=Yes + TrustServerCertificate=No), опции trust/optional, безусловные `ConnectRetryCount=0`, `MARS_Connection=No`, секреты никогда не попадают в возвращаемую строку; bounded connection pool с clean-state верификацией перед reuse (`vms_conn_verify`: не-quarantined + `@@TRANCOUNT=0` + `SELECT 1` round-trip), грязные соединения закрываются, host DM pooling не трогается. Тесты ABI/грамматики/posture/redaction + живой pool-тест — PASS на всех 4 конфигурациях (7/7×4).
- **R5 — metadata/type registry/stable identity: Gate G5 PASS** (2026-09-02). Слой `vms_meta` поверх каталога `sys.*` (schemas/objects/columns/types/indexes/index_columns/computed_columns/triggers) через R3-клиент; валидация идентификаторов + `N''`-экранирование — SQL-инъекция через имена невозможна; type registry (`sys.types` → `VmsColType`, корректный выбор TEXT/BIGTEXT/DECIMAL/DATETIME/GUID/BLOB); выбор stable identity key: PRIMARY KEY приоритетно, затем suitable UNIQUE NOT NULL; отсев unsuitable (nullable unique, computed key, filtered/disabled/hypothetical) подтверждён тестами на всех формах; versioned lossless identity token `v1` (int64 напрямую, text/blob — hex-кодирование, lossless round-trip, отбраковка мусора, truncation-safe). Интеграционные тесты на живом SQL Server 2022 — PASS на всех 4 конфигурациях (8/8×4).
- **R6 — read-only tables and views: Gate G6 PASS** (2026-09-02). Полноценный vtab-модуль `virtualmssql`: `CREATE VIRTUAL TABLE x USING virtualmssql(schema='dbo', table='...')` — shape из каталога R5 при xConnect, декларация SQLite-схемы по affinity; read-only базовые таблицы **и представления**; курсоры на независимых lease-соединениях из bounded-пула (параллельные/вложенные сканы); потоковое чтение LOB (nvarchar(max)/varbinary(max)) через SQLGetData-чанки; скаляры `virtualmssql_profile()` (конфигурация) и `virtualmssql_cred()` (provisioning секретов внутрь DLL — состояние провайдера живёт в DLL, а не в хосте); ранний close освобождает lease немедленно. G6-матрица (empty/типы/NULL/Unicode+emoji/LOB/100k-row streaming с агрегатами/два курсора/early-close) — PASS на всех 4 конфигурациях (9/9×4). Производительность baseline: ~10с на 100k-строчный полный скан (pushdown приходит в R7).
- **R7 — planner / safe pushdown: Gate G7 PASS** (2026-09-02). Компилятор планов `vms_plan`: xBestIndex анализирует constraints и потребляет **только доказуемо безопасные** операторы — проекция по colUsed (с учётом bit-63 fallback на все колонки), int-сравнения (EQ/LT/LE/GT/GE) только для INT-аффинных колонок и целых значений, IS NULL/IS NOT NULL, single-value IN (multi-value IN безопасно деградирует: одинаковое значение → equality, различное → contradiction `1=0`), ORDER BY только по int-колонкам (полный ORDER, не частичный), LIMIT через TOP(?)/OFFSET-FETCH (OFFSET только с ORDER BY, как требует T-SQL); текст-сравнения и текст-ORDER остаются локальными. План сериализуется в idxStr с magic/валидацией, параметры уходят в ODBC как типизированные SQLBIGINT-бинды (проверка каждого SQLBindParameter). Differential-тесты remote==local на живом сервере: 19 кейсов (проекция, 5 сравнений + negative + range, IS NULL/IS NOT NULL, IN, ORDER ASC/DESC+WHERE, LIMIT/LIMIT+OFFSET/OFFSET за границей, комбинированный, текст локальный, агрегаты) — PASS на всех 4 конфигурациях (10/10×4).
- **R8 — source=query / query profiles: Gate G8 PASS** (2026-09-02). Bounded T-SQL lexer (`vms_lexer`, allocation-free, позиции+спаны, строки/комментарии/скобки/скобочные идентификаторы) + валидатор read-only: ровно один statement, голова SELECT/WITH, 40+ запрещённых ключевых слов (DML/DDL/EXEC/динамика/BULK/OPENROWSET...), запрет SELECT INTO, баланс скобок, только хвостовая `;`. Метаданные результата — `sys.dm_exec_describe_first_result_set` (не sp_, т.к. даёт плоский SELECT-набор: name/system_type_name/max_length/precision/scale/is_nullable); контракт результата: уникальные имена колонок, ≥1 колонка, type registry → VmsColType. Модуль расширен: `CREATE VIRTUAL TABLE x USING virtualmssql(source='query', query='...')` — запрос исполняется **как есть** (без outer wrapper: T-SQL запрещает WITH внутри derived tables, а валидатор уже гарантирует один read-only SELECT). G8-матрица: SELECT/CTE/JOIN/GROUP/window-источники, rejection-матрица (offline+live), metadata failure, duplicate-columns contract — PASS на всех 4 конфигурациях (11/11×4).
- **R9 — Materialization: Gate G9 PASS** (2026-09-02). Материализатор `vms_mat`: query-source снапшот в private SQLite DB (`materialization='memory'` → `:memory:`, `'temp'` → приватный temp-файл, удаляется при destroy; `'off'` → потоковый режим R8). State machine **BUILDING → READY → PUBLISHED** / FAILED: снапшот заливается батчами по 2000 строк (промежуточные COMMIT внутри BUILDING), затем `query_indexes` — индекс на каждую INTEGER-колонку, затем **атомарная публикация** (единственная критическая секция state-store). Инвариант partial-never-published: cancel/OOM/лимит строк/ошибка сервера/ошибка индекса → FAILED, приватная БД уничтожается, читатели получают ошибку — PUBLISHED недостижим из частичного состояния. Курсор vtab в материализованном режиме читает снапшот через private-db statement (снапшот строится при первом скане). G9: happy path (12 строк + индексы + integrity), row-limit → FAILED, cancel → FAILED, temp-режим с cleanup, live vtab `materialization='memory'` с фильтрами — PASS на всех 4 конфигурациях (12/12×4).
- **R10 — DML: Gate G10 PASS** (2026-09-03). Write path для `mode=rw` table-источников: `vms_dml` — генерация INSERT/UPDATE/DELETE inline-SQL (значения: int64/float как канонические литералы, текст как N'...' с удвоением кавычек после UTF-8→UTF-16; идентификаторы bracket-quoted validated; injection-поверхности нет). `mode=rw` гейт (без rw → SQLITE_READONLY; source=query + rw запрещено структурно); identity/computed/rowversion колонки исключены из записи (server-owned). Ключевое решение по ключам: xRowid stash — SQLite вызывает xRowid перед xUpdate, поэтому xRowid сохраняет **все** stable-key значения текущей строки в vtab; xUpdate строит WHERE из stash (**старые** значения ключа — изменение ключа не ретаргетит строку), что покрывает int/string/GUID/составные ключи; DELETE/UPDATE с изменённым ключом корректны. INSERT с NULL-ключом опускает ключевую колонку → работают server-side defaults (NEWID()); для rw-таблиц best_index форсирует проекцию ключевых колонок (used_mask), иначе stash неполон. Тест-инфраструктура: seed дополнен таблицами vms10_* (int/PK, composite PK, GUID PK DEFAULT NEWID(), rowversion, AFTER-trigger с логом, view без ключа); G10-матрица: INSERT/UPDATE/DELETE для int/string/GUID/composite ключей, неизменяемость ключа, rowversion server-owned (два UPDATE → разные hex-токены), AFTER-trigger (v+1 и запись в лог), rejections (view/query/ro). PASS на всех 4 конфигурациях (13/13×4).

### Быстрая сборка

```powershell
# из окружения "vcvarsall x64" (или x86 для Win32-конфигураций)
cmake --preset x64-release
cmake --build --preset x64-release
ctest --preset x64-release
```

Скрипт-обёртка: `scripts/build-and-test.ps1 -Arch x64 -Config release`.

SQLite для тестов (официальные прекомпилированные DLL 3.53.4) лежит в `third_party/bin/{x64,x86}` — в release-артефакт не входит.

