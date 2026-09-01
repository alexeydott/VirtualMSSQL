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

