SQLite third-party components used by VirtualMSSQL
====================================================

third_party/sqlite/sqlite3.h, sqlite3ext.h
  Extracted from sqlite-amalgamation-3530400.zip, SQLite 3.53.4.
  Source: https://sqlite.org/download.html
  License: Public Domain (https://sqlite.org/copyright.html).
  The SQLite source is in the public domain and requires no license file.

third_party/bin/{x64,x86}/sqlite3.dll, sqlite3.def, sqlite3.lib
  Official precompiled binaries from sqlite.org (sqlite-dll-win-*-3530400.zip),
  used ONLY for testing (test harness loads the extension through a real host
  SQLite). Not bundled into the VirtualMSSQL release artifact. The release
  virtualmssql.dll links against whatever SQLite the host application provides.
