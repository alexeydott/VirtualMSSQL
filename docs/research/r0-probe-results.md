# R0 — результаты ODBC feasibility probe (Gate G0)

> Прогон `tools/vms-odbc-probe` 2026-09-01. Цель: проверить, что Microsoft ODBC Driver 18 поддерживает архитектуру VirtualMSSQL до написания кода расширения (этап R0, `02_ROADMAP_VirtualMSSQL.md`).

## Окружение прогона

| Компонент | Значение |
|---|---|
| ОС | Windows, x64 |
| Драйвер | Microsoft ODBC Driver 18 for SQL Server, 18.6.2.1 (установлен 2026-09-01; x64-инсталлятор ставит и 32-битный драйвер) |
| Сервер | SQL Server 2022 (Docker-контейнер `vms-sql2022`, `localhost:1433`, SQL auth, self-signed сертификат) |
| Конфигурации | x64 и Win32 сборки пробника (MSVC, C11, Ninja) |
| Настройки соединения | `ConnectRetryCount=0; ConnectRetryInterval=1; MARS_Connection=No` в каждой строке соединения |

## Итог

**x64: PASS=22, FAIL=2, SKIP=1 (25). Win32: PASS=22, FAIL=2, SKIP=1 (25).** Результаты идентичны на обеих архитектурах.

Оба FAIL — ограничения окружения, не архитектуры:

- `connect/windows_auth` — Windows-аутентификация из хоста в Linux-контейнер требует AD/Kerberos-инфраструктуры, которой нет. Диагностика корректная: `HY000, «В пакете безопасности отсутствуют учетные данные»`.
- `connect/tls_verify` — ожидаемый отказ: серверный сертификат self-signed, проверка цепочки работает (подтверждено PASS-ом негативного кейса `tls_verify_untrusted_expected_fail`).

SKIP: `tx/unknown_commit_policy` — требует сетевой fault injection; политика «ошибка + вывести HDBC из строя + никогда не повторять COMMIT» зафиксирована как обязательное правило, fault injection переносится в R14 (resilience).

## Ключевые находки (кормят R2–R3)

1. **Потолок параметров: 1999** (бинарный поиск по `SELECT ?,?...`). SQL Server допускает 2100 маркеров на запрос; практический потолок ниже из-за служебных расходов драйвера. → `max_parameters` по умолчанию: **1999** с запасным порогом на 2000.
2. **Cross-thread отмена работает только через statement-handle.** `SQLCancelHandle(SQL_HANDLE_DBC)` НЕ прерывает выполняемый statement (он отменяет лишь connection-level операции вроде connect). Работающая схема: воркер публикует активный `HSTMT`, отменяющая сторона вызывает `SQLCancelHandle(SQL_HANDLE_STMT, stmt)`. Отмена занимает ~2 с, соединение остаётся пригодным после отмены. → архитектура R3: connection-affine воркер + публикация активного statement'а для `sqlite3_interrupt`.
3. **ColumnSize=0 запрещён для LONG-типов в параметрах.** `SQLBindParameter` для `SQL_WLONGVARCHAR`/`SQL_LONGVARBINARY` отвергает `ColumnSize=0` с `HY104` (недопустимое значение точности); документированный `SQL_SS_LENGTH_UNLIMITED` (0) для параметров не работает. Максимальное принимаемое значение — `2^30-1` (1073741823). До этого «немые» отказы биндов проявлялись на `SQLExecute` как `07002 COUNT field incorrect` — **всегда проверять код возврата каждого `SQLBindParameter`**.
4. **`SQL_NO_DATA` — успех для DML по пустой таблице.** `DELETE`/`UPDATE`, не затронувшие строк, возвращают `SQL_NO_DATA`; трактовать как успех, не как ошибку.
5. **Ленивое начало транзакции подтверждено.** При `SQL_ATTR_AUTOCOMMIT=OFF` сразу после коннекта серверной транзакции НЕТ; первый statement (даже `SELECT`) её открывает; `SQLEndTran(ROLLBACK)` закрывает. `SQL_COPT_SS_AUTOBEGINTXN=OFF` не дал наблюдаемой разницы в этом контуре. Важно: наблюдать `@@TRANCOUNT` «до первого statement» невозможно — сам замер является statement'ом и открывает транзакцию.
6. **`SQL_COPT_SS_AUTOBEGINTXN = 1402`** (SQL_COPT_SS_BASE_ADD=1400 + 2), а не 1245; драйвер молча принимает неверный номер атрибута. Брать константы только из актуального `msodbcsql.h` (MSI x64: `Client SDK\ODBC\180\SDK\Include`).
7. **Savepoint-primer SUPPORTED**: `SAVE TRANSACTION` работает до первого бизнес-statement'а в транзакции; `SAVE`/`ROLLBACK TRANSACTION` с именами работают штатно.
8. **`XACT_STATE()` после пойманной ошибки = 1** (транзакция коммиттабельна); контур `-1`/doom остаётся на R11 с полноценной fault-инъекцией.
9. **OUTPUT INTO @table-переменную с AFTER-триггером работает**; значения — pre-trigger. `CREATE TRIGGER` обязан быть первым оператором батча.
10. **Стриминг**: 1 000 000 строк через `SQLFetch` + `SQLGetData` (col 2 чанками, bounded buffer 4 КБ) — 234–313 мс; ограничение по увеличению ординалов не нарушено. Полная выборка без полной буферизации реализуема.
11. **DAE (SQLParamData/SQLPutData)** работает (8 чанков, `SQL_C_WCHAR` → `nvarchar(max)`).
12. **Кодеки**: полный 25-колоночный bound-круг (bit…xml, GUID, decimal(30,10) как текст, деньги, Unicode ASCII/кириллица/CJK/emoji/non-BMP, binary, дата/время, LOB) + отдельный NULL-bind — PASS на обеих архитектурах.
13. **Соединение**: SQL-auth по hostname:port — OK; TLS `Encrypt=Optional` и `TrustServerCertificate=Yes` — OK; verify-off работает как ожидается; грамматика строк соединения не может выдать `RetryExec/DSN/FileDSN/SaveFile/Driver`.

## Оценка Gate G0

**PASS.** Все архитектурно-критичные кейсы прошли на x64 и Win32:

- R0.1 scaffolding — PASS (сборка 2 конфигураций, JSON+лог, exit-агрегация).
- R0.2 connection/TLS/auth — PASS (2 FAILа — ограничения окружения, объяснены выше; детерминированные no-retry настройки принимаются драйвером).
- R0.3 execution/binding/types — PASS (25 кодеков, NULL, DAE, потолок 1999).
- R0.4 streaming — PASS (1M строк, чанкинг, ограниченная память).
- R0.5 cancellation/threading — PASS (cross-thread cancel через `SQL_HANDLE_STMT`, прототип interrupt, соединение переиспользуемо).
- R0.6 transaction semantics — PASS по всем автоматизируемым кейсам; fault-injection кейсы (unknown COMMIT, `XACT_STATE()=-1` recovery) перенесены в R14 по дизайну плана.

Найденные в пробнике дефекты в ходе прогона исправлены: stack overflow от 1.7-МБ контекста на стеке, `ConnectRetryInterval=0`, обрезка SQL-строк `_snwprintf_s`, неверный атрибут AUTOBEGINTXN, игнор кодов возврата `SQLBindParameter`, DBC-уровень отмены, утечка «брошенного» воркера (use-after-free → краш tx-группы).
