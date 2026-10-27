# Build ID and per-version CEF extraction

Aurora ships as one self-extracting `.exe` whose payload contains a full CEF
binary distribution. The failure mode this document is about is specific and
nasty: after an update, a resource bundle (`chrome_100_percent.pak`,
`icudtl.dat`, `v8_context_snapshot.bin`, `locales/*.pak`) from the *old*
Chromium is left next to the *new* `libcef.dll`. Chromium either refuses to
start or starts and misrenders, the error text points at nothing actionable, and
the only known fix — "delete this folder" — is not something a user discovers.

The fix is to name the extraction directory after the bytes it contains.

## The build ID

```
build_id = SHA-256( canonical manifest text )   truncated to 16 hex chars
```

The canonical text (see `src/core/BuildId.cpp`) is deterministic: LF newlines, a
fixed key order, one line per runtime file sorted by path, `\` normalized to `/`.

```
aurora-build-manifest 1
app-version 0.1.0
cef-version 141.0.5+g1234567+chromium-141.0.7390.55
platform windows64
file resource 3444416 3f7c…  chrome_100_percent.pak
file resource 10620128 91ab…  icudtl.dat
file identity 214093824 5cd1…  libcef.dll
file resource 1046544 77ea…  locales/en-US.pak
…
build-id 657784e4fb8fbb8c
```

Two roles:

| Role | Meaning |
| --- | --- |
| `resource` | Materialized into the per-version directory and handed to CEF through `resources_dir_path` / `locales_dir_path`. |
| `identity` | Hashed into the build ID but loaded from the application directory by the OS loader (`libcef.dll`, ANGLE, SwiftShader). |

Everything that can change how the engine behaves is in the hash: the file
contents, their sizes, their roles, the CEF version string, the Aurora version,
the platform tag and the manifest format version. Rebuilding Aurora against the
same CEF yields the same ID, so the extracted tree is reused; touching one byte
of CEF yields a different ID, so a fresh tree is built and the old one cannot be
consulted by accident.

The `build-id` line in the file is informational. Consumers recompute it from
the content and refuse to run on a mismatch — a manifest edited after packaging
is a tampered payload, not a configuration.

## Layout

```
C:\Program Files\Aurora Browser\       <- default setup wizard install folder
    aurora_bootstrap.exe               <- shortcut/launcher target
    aurora_browser.exe, Qt6*.dll, libcef.dll, *.pak, locales\, ...
    aurora_build.manifest

%LOCALAPPDATA%\Aurora\
    app\                               <- legacy per-user SFX install (upgraded in place)
    cef\
        657784e4fb8fbb8c\             <- per-version resource tree
            chrome_100_percent.pak, icudtl.dat, v8_context_snapshot.bin
            locales\en-US.pak, ...
            .aurora-build-id          <- completion stamp, written last
        657784e4fb8fbb8c.lock         <- held only during extraction
```

On POSIX the base directory is `$XDG_DATA_HOME/Aurora` (or
`~/.local/share/Aurora`); `AURORA_HOME` overrides it anywhere, which is what
makes the unit tests and a portable install possible.

User data is unaffected: the profile stays in `%APPDATA%\Aurora\Aurora`
(`QStandardPaths::AppDataLocation`), so wiping the extraction directories never
loses history, bookmarks or logins.

## The launcher (`aurora_bootstrap`)

Links `aurora_core` only — no Qt, no CEF, no SQLite — because it runs on the
critical path of every launch.

1. Read `aurora_build.manifest` from its own directory and recompute the ID.
   Mismatch → exit 2 with a "reinstall Aurora" message.
2. Take `cef\<build_id>.lock` (exclusive create; a lock older than five minutes
   belongs to a crashed launcher and is stolen, so one crash cannot make Aurora
   permanently unstartable).
3. Materialize `cef\<build_id>\`:
   * stamp matches → presence + size check over the manifest, then done;
   * otherwise, per file: hard link from the application directory (no extra
     disk space, same volume) with a copy fallback, written as
     `<name>.aurora-tmp` and renamed into place;
   * verify hashes of everything that was written;
   * write `.aurora-build-id` **last**. A tree without a matching stamp is
     always rebuilt, so an interrupted extraction is never trusted.
4. Remove per-version directories from other build IDs. Only directories whose
   name is a valid build ID are ever candidates. Failures are expected while an
   old browser is still shutting down and are ignored — the directory is
   collected on a later launch.
5. Launch `aurora_browser.exe` with `AURORA_CEF_DIR` set to the per-version
   directory, forwarding every argument.

Flags the launcher consumes (everything else is forwarded):

| Flag | Effect |
| --- | --- |
| `--shell` | Attach a console, log at debug level, wait for the browser and return its exit code. Also forwarded, so the browser shares the console. |
| `--aurora-force-extract` | Re-materialize even when the stamp matches. |
| `--aurora-full-verify` | Hash every file instead of checking sizes. |
| `--aurora-verify-only` | Do the work, print the report, do not launch. |
| `--aurora-print-build-id` | Print the build ID and exit. |
| `--aurora-keep-stale` | Do not collect old per-version directories. |

## The browser side

`src/app/main.cpp` resolves the resource directory before `CefInitialize`, in
this order:

1. `--cef-dir=PATH` (bisecting an engine by hand)
2. `AURORA_CEF_DIR` (what the launcher sets)
3. `%LOCALAPPDATA%\Aurora\cef\<build_id>\` when a manifest sits next to the
   executable *and* that directory carries a matching stamp
4. the application directory — a plain CMake build, which has no manifest and
   therefore behaves exactly as it did before any of this existed

The browser never extracts anything. If the per-version directory is incomplete
it falls back to the application directory instead of doing half of the
launcher's job.

## Packaging (`aurora_buildid`)

Built from the same core library as the launcher, which is the only reason the ID
written at packaging time and the ID verified at runtime can be trusted to agree.

```bash
# after windeployqt and after copying third_party/cef/{Release,Resources} in
aurora_buildid generate --root package --cef-root third_party/cef --platform windows64
aurora_buildid show   --manifest package/aurora_build.manifest
aurora_buildid verify --manifest package/aurora_build.manifest --root package --full
```

`generate` refuses to write a manifest when it finds no CEF resource files at
all, so a Qt-only package cannot end up claiming an empty tree is valid — it
ships with no manifest, and the launcher runs the browser flat.

## Tests

CEF cannot be downloaded or run in CI, so the whole scheme is covered by unit
tests over real files in a temporary directory:

* `tests/test_sha256.cpp` — NIST vectors, padding boundaries, chunked streaming.
* `tests/test_build_id.cpp` — reproducibility, sensitivity to every hashed
  input, serialize/parse round trips, tamper detection, glob classification,
  verification of missing / resized / corrupt files, stamp validation.
* `tests/test_cef_layout.cpp` — path derivation on both platforms, resolution
  order, first extraction, reuse, repair, interrupted extraction, the copy
  fallback, two CEF versions coexisting, and collection of the old one.
