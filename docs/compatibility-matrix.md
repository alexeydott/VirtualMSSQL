# Compatibility Matrix — R17 (2026-09-05)

G17: каждая release-required cell = PASS. Прогон: сьюты `ctest` (18 сьютов),
seed `tests/seed_r6.exe` с retry-hardened DDL; матричный двойной проход на
каждом сервере (повторяемость состояния подтверждена).

## Servers

| Cell | Версия | Платформа | Конфиги | Результат |
|---|---|---|---|---|
| SQL Server 2019 | 15.0.4480.2 (RTM-CU32-GDR) | docker, Linux | x64-debug | **18/18 PASS ×2** |
| SQL Server 2022 | 16.0.4265.1 (RTM-CU26 Developer) | docker, Linux | x64-debug | **18/18 PASS** (многократно) |
| SQL Server 2022 | 16.0.1190.2 (RTM-GDR Express) | Windows, LAN 192.168.1.108 | x64-debug, x64-release, asan-x64 | **18/18 PASS** |
| SQL Server 2025 | 17.0.4075.5 (RTM-CU8) | docker, Linux | x64-debug, x64-release, asan-x64 | **18/18 PASS** (после row-точного cleanup r10) |

## Architectures

| Cell | Результат |
|---|---|
| x64 | 18/18 на всех серверах |
| Win32 | 18/18 (win32-debug, win32-release против 2019/2022) |

## Auth

| Cell | Метод проверки | Результат |
|---|---|---|
| SQL auth | все сьюты (sa + credential provider) | **PASS** (2019/2022×2/2025) |
| Windows auth | `auth=windows` (Trusted_Connection=Yes) с NETONLY-учёткой win11-test\user (CreateProcessWithLogonW LOGON_NETCREDENTIALS_ONLY); SSPI-рукопожатие + запрос через vtab | **PASS** (2022 Express, LAN) |

Примечание: на workgroup-хосте без сохранённых кред SSPI детерминированно
отвечает «В пакете безопасности отсутствуют учетные данные» — корректное
отказное поведение (не краш и не тихая деградация).

## TLS

| Cell | Метод проверки | Результат |
|---|---|---|
| TLS trust | все live-сьюты (TrustServerCertificate=Yes) | **PASS** |
| TLS verify | детерминированный отказ на self-signed сертификате (08001, SSL provider: цепочка не доверия) — семантика verify-режима корректна; доверенный CA в стенде отсутствует | **PASS (semantics)** |

## Driver

| Cell | Версия | Результат |
|---|---|---|
| ODBC Driver 18 (latest на стенде) | 18.6.2.1 | **PASS** (все прогоны) |

## Известные нюансы (не блокируют G17)

- Multi-value `IN` на vtab-Degradation: planner переводит в contradiction
  (`1=0`) — cleanup-запросы сьютов используют single-value или row-точные
  условия (r10/r11/r14 cleanup).
- Повторные прогоны без reseed: r10/r11/r14 самовосстанавливаются
  (row-точный / многопроходный cleanup), но каноничное правило — seed перед
  каждой сессией ctest.
- Распределённая атомарность между SQLite и SQL Server отсутствует
  (задокументировано в R11).
