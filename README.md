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

### Быстрая сборка

```powershell
# из окружения "vcvarsall x64" (или x86 для Win32-конфигураций)
cmake --preset x64-release
cmake --build --preset x64-release
ctest --preset x64-release
```

Скрипт-обёртка: `scripts/build-and-test.ps1 -Arch x64 -Config release`.

SQLite для тестов (официальные прекомпилированные DLL 3.53.4) лежит в `third_party/bin/{x64,x86}` — в release-артефакт не входит.

