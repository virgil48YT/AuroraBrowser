# Releasing Aurora (Windows, macOS, Linux, Android)

All four editions are built by `.github/workflows/release.yml`.

## Cutting a release

```bash
git tag v0.2.0
git push origin v0.2.0
```

The workflow builds every platform in parallel and attaches the packages,
each with a `.sha256` file, to a **draft** GitHub release named
`Aurora 0.2.0`. Nothing is public until a maintainer reviews the draft and
presses **Publish release**. A tag containing `-` (e.g. `v0.2.0-rc.1`) is
marked as a pre-release. Running the workflow by hand (Actions > Release >
Run workflow) builds the same packages as workflow artifacts without creating
a release.

| Platform | Package | Built on |
|---|---|---|
| Windows x64 | `Aurora-VERSION-windows-x64-setup.exe` (single-exe SFX: CEF + Qt, build-ID verified) | `windows-latest` |
| Linux x86_64 | `Aurora-VERSION-linux-x86_64.AppImage`, `Aurora-VERSION-linux-x86_64.tar.gz` | `ubuntu-22.04` (glibc 2.35+) |
| macOS arm64 | `Aurora-VERSION-macos-arm64.dmg` | `macos-14` |
| Android | `Aurora-VERSION-android.apk` (signed) or `Aurora-VERSION-android-unsigned.apk` | `ubuntu-latest` |

## Signing (optional repository secrets)

Packages are produced without any secrets, but unsigned packages carry the
usual operating-system warnings. No keys or certificates are stored in the
repository.

| Platform | Secrets | Without them |
|---|---|---|
| macOS | `MACOS_CERTIFICATE_P12_BASE64`, `MACOS_CERTIFICATE_PASSWORD`, `MACOS_SIGN_IDENTITY` (e.g. `Developer ID Application: Name (TEAMID)`); notarization additionally `APPLE_ID`, `APPLE_TEAM_ID`, `APPLE_APP_PASSWORD` | Ad-hoc signed DMG. Gatekeeper reports an unidentified developer: right-click > Open, or `xattr -dr com.apple.quarantine /Applications/Aurora.app` |
| Android | `ANDROID_KEYSTORE_BASE64`, `ANDROID_KEYSTORE_PASSWORD`, `ANDROID_KEY_ALIAS`, `ANDROID_KEY_PASSWORD` | `-unsigned.apk`, which Android will not install until it is signed with `apksigner` (see `android/README.md`) |
| Windows | not wired yet | Unsigned SFX; SmartScreen warns on first run. Authenticode signing needs a code-signing certificate |
| Linux | none needed | AppImage and tarball are not signed (SHA-256 files are published) |

## Per-platform packaging scripts

- Windows: `packaging/windows/build_sfx.sh` (see `docs/build-id.md`).
- Linux: `packaging/linux/package_linux.sh BUILD_DIR CEF_ROOT OUT_DIR VERSION`
  — AppDir with the browser and the CEF runtime in `usr/bin`, Qt bundled by
  linuxdeploy-plugin-qt, `libcef.so` stripped, then tarball + AppImage.
- macOS: `packaging/macos/package_macos.sh APP OUT_DIR VERSION [ARCH]` —
  macdeployqt, inside-out codesign (Developer ID with hardened runtime and
  `packaging/macos/entitlements.plist`, or ad-hoc), DMG, optional
  notarization + stapling.
- Android: `cd android && ./gradlew assembleRelease` with the signing
  environment variables documented in `android/README.md`.

## Platform notes

- **Linux** runs the Qt chrome on X11 (XWayland in a Wayland session) because
  CEF's windowed browsers are parented to an X11 window; Aurora sets this up
  itself. The AppImage needs FUSE 2 (`libfuse2`), or run it with
  `--appimage-extract-and-run`; the tarball needs nothing extra.
- **macOS** ships an `Aurora.app` bundle with the Chromium Embedded Framework
  and the five `Aurora Helper*.app` subprocess bundles in
  `Contents/Frameworks`. Only Apple-silicon (arm64) packages are built; an
  Intel build needs a `macos-13` runner and `PLATFORM=macosx64`.
- **Android** uses the system WebView, not CEF (CEF has no Android port).
- The Chromium sandbox is disabled on every desktop platform (see
  `docs/security.md`).
