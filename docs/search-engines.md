# Aurora — search engines

The omnibox sends anything that is not a URL to the **default search
engine**. You choose the default in `aurora://settings` → Search. The
default engine's home page is also what new tabs and the Home button open.

## Built-in engines

DuckDuckGo (the default), Google, Bing and Startpage. Their URLs live in
`core::search_engine_template` / `core::search_engine_home`
(`src/core/Url.cpp`). They are not database rows and cannot be edited or
deleted.

## Custom engines

Settings → Search → *Custom search engines* lets you **add**, **edit** and
**delete** engines. Each one has:

- **Name.** 1–64 bytes, no control characters, and unique ignoring case. The
  names of the built-in engines are reserved.
- **Search URL.** A template that must:
  - start with `https://` (plain `http://`, `javascript:` and other schemes
    are rejected);
  - have a valid host, with no user name or password in the URL;
  - contain **exactly one** `%s`, in the path, query or fragment and not in
    the host. Aurora replaces it with the URL-encoded search terms (a space
    becomes `+`).

  Example: `https://kagi.com/search?q=%s`

The dialog validates as you type. The service checks again before saving,
and the `search_engines` table has `CHECK` constraints as a final guard. The
home page of a custom engine is the root of its site
(`https://<host>[:port]/`).

Deleting the engine that is currently the default sets the default back to
DuckDuckGo. If the preference names an engine that no longer exists (for
example one deleted from another window), the omnibox also uses DuckDuckGo.

Private (incognito) windows search with the profile's default engine,
including a custom one. They cannot add, edit or delete engines. Picking a
different default in a private window only affects that window and is not
saved, which is how every preference behaves there.

## How it is built

| Piece | Where | Notes |
|---|---|---|
| Template/name validation, expansion, default resolution | `src/searchengines/SearchEngine.{hpp,cpp}` | Pure core: no Qt, CEF or SQLite |
| `search_engines` table | `src/storage/Migration.cpp`, schema **v7** | `id`, `name` (UNIQUE NOCASE), `url_template`, `created_at` |
| `SearchEngineStore` | `src/searchengines/SearchEngineService.cpp` | Synchronous CRUD over a borrowed `Database` (what the tests drive) |
| `SearchEngineService` | same file | Owns a **worker thread** with its **own** `Database` connection; the UI reads only an in-memory snapshot |
| Settings UI | `src/ui/pages/SettingsPage.cpp` | Posts mutations to the worker; results are marshalled back to the UI thread |
| Omnibox resolution | `MainWindow::resolved_search_engine()` → `core::resolve_navigation` | Substitutes `%s`; built-in prefix templates still append |

The default engine is still stored in the `search.engine` preference. Its
value is a built-in id or `custom:<row id>`, so existing profiles keep their
setting without a preference migration.

At startup `main.cpp` waits for the service's first load, at most 500 ms and
before any window exists, so a custom default applies from the first
navigation. If loading takes longer, the omnibox uses DuckDuckGo until the
snapshot arrives.

Tests: `tests/test_search_engines.cpp` (validation, expansion, resolution,
migration from v6, store CRUD, and the worker service).
