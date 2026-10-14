# Vendored SQLite amalgamation

This directory contains the official SQLite amalgamation source, downloaded
from <https://www.sqlite.org/download.html> and committed verbatim so the
browser builds self-contained on platforms that don't ship a SQLite development
package (notably Windows CI runners).

- `sqlite3.c`, `sqlite3.h`, `sqlite3ext.h` — the SQLite 3.53.4 amalgamation
  (public domain; see the header banner in `sqlite3.h`)

CMake uses the system SQLite (`libsqlite3-dev`) when available and falls back
to this vendored copy automatically — see the `SQLite` block in the top-level
`CMakeLists.txt`. To refresh, download the latest `sqlite-amalgamation-*.zip`
from sqlite.org and drop the three files here.
