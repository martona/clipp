# Building Clipp From Source

This document covers building Clipp for Windows, macOS, and iOS from a source checkout. For most cases, the build is a one-liner from the [`scripts/`](scripts) directory — the rest of this document covers prerequisites, environment overrides, and the cases where you need to bypass the scripts. Cutting and publishing a release — versioning, the signing and notarization pipeline, and App Store submission — is covered separately in [RELEASING.md](RELEASING.md).

## Contents

- [Tested platforms](#tested-platforms)
- [Prerequisites](#prerequisites)
- [Building](#building)
- [Build outputs](#build-outputs)
- [Versioning](#versioning)
- [Environment variables](#environment-variables)
- [Code signing](#code-signing)
- [Troubleshooting](#troubleshooting)

## Tested platforms

| Platform | Architecture        | Status                    | Build entry point                                                            |
|----------|---------------------|---------------------------|------------------------------------------------------------------------------|
| Windows  | amd64               | tested in CI              | `scripts\build_windows.ps1`                                                  |
| Windows  | arm64               | tested in CI              | `scripts\build_windows.ps1 -VcVarsArch arm64 -Triplet arm64-windows-static`  |
| macOS    | arm64               | tested in CI              | `scripts/build_macos.sh`                                                     |
| macOS (App Store) | arm64      | manual                    | `scripts/build_macos_mas.sh`                                                 |
| iOS      | arm64 simulator     | tested in CI              | `scripts/build_ios.sh`                                                       |
| iOS      | arm64 device        | manual Xcode only         | open `ios/Clipp.xcodeproj` in Xcode                                          |

Intel Macs are not supported: the build's minimum deployment target is macOS 14.0, which Apple ships for Apple Silicon only.

## Prerequisites

### All platforms

`git` is the only universal prerequisite you install by hand. CMake (≥ 3.20) and vcpkg are pulled in by the platform setup below.

vcpkg is provisioned per platform:

- **macOS / iOS**: the build scripts clone vcpkg into a per-user cache directory (default: `~/Library/Caches/clipp/vcpkg`) and bootstrap it on first run. Set `$CLIPP_CACHE_DIR` if you want the cache somewhere else.
- **Windows**: the script reuses an existing vcpkg — the copy bundled with the Visual Studio C++ workload by default. Set `$VCPKG_ROOT` to point at a different checkout.

The vcpkg manifest at [`src/vcpkg.json`](src/vcpkg.json) pulls in `libsodium`, `xxhash`, and `zstd`. Versions are pinned by the manifest's `builtin-baseline` — vcpkg's manifest mode handles version selection automatically; no manual baseline step is required. If you reuse an older vcpkg checkout that pre-dates the baseline commit, `git pull` inside that checkout to refresh it.

### Windows

- **Visual Studio** (2026, 2022 or 18, any edition) with the **Desktop development with C++** workload. This single workload provides CMake, vcpkg, Ninja, the MSVC toolchain, and the Windows SDK — everything the build script needs.
- For arm64 cross-compilation, also install the **MSVC v143 — VS 2022 C++ ARM64 build tools** component (or its VS18/VS2026 equivalent). The default workload installs the amd64 toolset only.

### macOS

- **Xcode Command Line Tools** (for the Ninja-based build path) or **full Xcode** (for the Xcode-generator build path). The script picks Xcode if `xcode-select -p` resolves to an `Xcode.app` install, otherwise Ninja.
- Homebrew or MacPorts. The libsodium vcpkg port requires autotools:
  ```sh
  brew install autoconf autoconf-archive automake libtool ninja
  ```
  or
  ```sh
  sudo port install autoconf autoconf-archive automake libtool
  ```
- Minimum deployment target: **macOS 14.0** (set in [`CMakeLists.txt`](CMakeLists.txt)).

### iOS (in addition to the macOS prerequisites)

- **Full Xcode** with `xcodebuild` on `PATH`
- iOS SDK (bundled with Xcode)

The iOS scripts target `arm64-ios-simulator` only and verify the host is Apple Silicon — Intel Macs are not currently supported for iOS builds via these scripts.

## Building

Each script is idempotent. They will reconfigure CMake or rerun vcpkg as needed and respect a previously populated cache directory.

### Windows

From a regular PowerShell prompt (the script imports the Visual Studio environment itself):

```powershell
.\scripts\build_windows.ps1
```

Parameters (all optional):

| Parameter      | Default                       | Description                                                                  |
|----------------|-------------------------------|------------------------------------------------------------------------------|
| `-BuildType`   | `Release`                     | `Release` or `Debug`                                                         |
| `-Triplet`     | `x64-windows-static`          | Any vcpkg triplet — e.g. `arm64-windows-static`                              |
| `-VcVarsAll`   | auto-located                  | Path to `vcvarsall.bat` if `vswhere` fails to find it                        |
| `-VcVarsArch`  | `amd64`                       | First argument to `vcvarsall.bat` — `amd64`, `arm64`, `amd64_arm64`, …       |
| `-VcpkgRoot`   | auto-located                  | vcpkg root directory                                                         |
| `-Generator`   | auto (Ninja → NMake)          | CMake generator override                                                     |
| `-Parallel`    | `[Environment]::ProcessorCount` | Parallel build jobs                                                        |
| `-Version`     | (unset → CMake default)       | Stamp the binary with this version (`W.X.Y.Z`). See [Versioning](#versioning). |
| `-DisableCodeSigning` | (off)                  | Skip artifact signing even if all `ARTIFACT_SIGNING_*` env vars are set. CI uses this. |

### macOS

```sh
./scripts/build_macos.sh                       # Release build (default)
./scripts/build_macos.sh --debug               # Debug build
./scripts/build_macos.sh --clean               # wipe build/ first
./scripts/build_macos.sh --version 1.2.3.4     # stamp the bundle with this version
```

Flags: `--debug`, `--release`, `--clean`, `--version W.X.Y.Z`, `--notarize`. All optional. See [Versioning](#versioning) for the version flag's behavior.

The script:

1. Verifies Xcode Command Line Tools are installed.
2. Installs missing tools via Homebrew if available.
3. Clones and bootstraps vcpkg under [`$CLIPP_CACHE_DIR`](#environment-variables) if absent.
4. Configures CMake using the Xcode generator (if full Xcode is selected) or Ninja, then builds.
5. Optionally signs the bundle if [`APPLE_CODESIGN_IDENTITY`](#code-signing) is set.

### macOS (Mac App Store)

A Mac App Store build must be **sandboxed** — a different bundle than the Developer ID one `build_macos.sh` produces. To build and exercise the sandboxed app locally, with no certificates required, use the separate script:

```sh
./scripts/build_macos_mas.sh
```

With no flags it builds and ad-hoc signs against [`Clipp.mas.entitlements`](src/platform/macos/Clipp.mas.entitlements) (app sandbox + client/server networking) into `build-mas/`, so you can confirm the app behaves under the sandbox. It uses a separate build directory from `build_macos.sh`, so the two coexist without CMake reconfigure thrash. `--debug`, `--release`, `--clean`, and `--version W.X.Y.Z` behave as for `build_macos.sh`.

Signing for distribution and uploading to the App Store (`--sign`, `--package`, `--upload`), the certificates and provisioning profile involved, and the submission flow are covered in [RELEASING.md](RELEASING.md).

### iOS simulator

```sh
./scripts/build_ios.sh                        # Release build (default), includes vcpkg setup
./scripts/build_ios.sh --debug                # Debug build
./scripts/build_ios.sh --skip-vcpkg           # reuse previously installed deps
./scripts/build_ios.sh --disable-code-signing # pass CODE_SIGNING_ALLOWED=NO
./scripts/build_ios.sh --clean                # remove build/ios first
```

Flags: `--debug`, `--release`, `--disable-code-signing`, `--skip-vcpkg`, `--clean`. All optional.

The script delegates dependency setup to [`scripts/setup_ios_vcpkg.sh`](scripts/setup_ios_vcpkg.sh), then builds the `Clipp` target in `ios/Clipp.xcodeproj` via `xcodebuild`.

### iOS device

CI does not currently build for physical devices; device builds are produced manually from Xcode:

1. Run dependency setup once:
   ```sh
   ./scripts/setup_ios_vcpkg.sh --device-only
   ```
2. Open `ios/Clipp.xcodeproj` in Xcode.
3. Select your development team under **Signing & Capabilities** for the `Clipp` target.
4. Choose a connected device (or "Any iOS Device") and **Product → Build / Archive**.

Use XCode -> Product -> Archive after selecting "Any iOS Device" in Run Destinations. Click Distribute, then "App Store Connect" for the store upload, or "Release Testing" for ad-hoc devices. For the latter, you can deploy them with XCode -> Window -> Devices and Simulators (same place where you get the device UDIDs).

### Regenerating the device-type symbol font

Each peer row shows two small glyphs — an OS-family mark and a device-type mark —
chosen from the peer's reported `OsType`. They are drawn from **`ClippSymbols.ttf`**,
a tiny ([~2–5 KB](src/resources/ClippSymbols.ttf)) subset of [Nerd Fonts](https://www.nerdfonts.com/)
containing only the handful of glyphs we use. The subset and its codepoint header
are **generated and committed**, so a normal build needs no extra tooling — Python
is *not* a build dependency. You only run the generator when changing the glyph set.

To add, remove, or swap a glyph:

1. Install the one-time tool: `pip install fonttools`.
2. Edit [`tools/symbols/manifest.json`](tools/symbols/manifest.json) — the `mapping`
   table pairs each `OsType` with an OS-family and a device-type glyph, named by their
   [Nerd Fonts cheat-sheet](https://www.nerdfonts.com/cheat-sheet) names (e.g.
   `nf-md-cellphone`). Use `null` for `device` to render no device glyph.
3. Regenerate:
   ```sh
   python tools/symbols/build_symbols_font.py
   ```
   This resolves each name to a codepoint against the pinned Nerd Fonts
   `glyphnames.json` (failing loudly if a name doesn't exist), subsets the font,
   renames the family to `Clipp Symbols`, and rewrites:
   - [`src/resources/ClippSymbols.ttf`](src/resources/ClippSymbols.ttf) — embedded in the
     Windows exe; bundled in the macOS `.app` resources.
   - `ios/Clipp/Resources/ClippSymbols.ttf` — a copy for the iOS bundle (Xcode can't
     reference the shared `src/resources` file, so the generator keeps this in sync).
   - [`src/OsGlyphs.h`](src/OsGlyphs.h) — the `OsType → {family, device}` codepoint map
     consumed by the renderers.
4. **Commit all three regenerated files.**

The script downloads the pinned Symbols Nerd Font and `glyphnames.json` by default;
pass `--symbols-font <path>` / `--glyphnames <path>` to use local copies offline. Both
generated files are required to build on Windows (the `.ttf` is compiled into the exe);
a fresh checkout already contains them.

## Build outputs

| Platform / generator | Output                                                                     |
|----------------------|----------------------------------------------------------------------------|
| Windows              | `build\windows-<config>\clipp.exe` + `clipp.com` + `clipp.pdb`             |
| macOS (Ninja)        | `build/clipp.app`                                                          |
| macOS (Xcode)        | `build/<Config>/clipp.app` (e.g. `build/Release/clipp.app`)                |
| macOS (App Store)    | `build-mas/<Config>/Clipp.app` (sandboxed; ad-hoc signed for local testing) + `build-mas/Clipp.pkg` (signed installer, `--package`) |
| iOS simulator        | `build/ios/Build/<Config>-iphonesimulator/Clipp.app`                       |

`<config>` is the lower-cased build type on Windows (`release`, `debug`) and the literal Xcode configuration name on macOS/iOS (`Release`, `Debug`).

`clipp.com` is a small console shim that re-launches `clipp.exe` with stdio attached — useful when running from `cmd.exe` / PowerShell where the GUI subsystem detaches by default.

## Versioning

The project version is a 4-part `W.X.Y.Z` string. The canonical default lives in the `set(CLIPP_VERSION "...")` line near the top of [`CMakeLists.txt`](CMakeLists.txt); the build scripts pass that through to the compiler so all Windows/macOS builds pick it up automatically. Override per-invocation with `--version W.X.Y.Z` on macOS or `-Version W.X.Y.Z` on Windows.

Where it ends up:

| Platform | Build-time source                                    | Stamped into                                                          |
|----------|------------------------------------------------------|-----------------------------------------------------------------------|
| Windows  | `-DCLIPP_VERSION=...` from build script              | `clipp.exe` / `clipp.com` `VERSIONINFO` resource; `version.h`         |
| macOS    | same                                                 | `Clipp.app/Contents/Info.plist` `CFBundle*Version`; `version.h`       |
| iOS      | `ios/Info.plist` (read at runtime via `Bundle.main`) | The two iOS bundles' `CFBundleShortVersionString` / `CFBundleVersion` |

iOS doesn't share the CMake pipeline (Xcode drives the iOS build), so its version is held separately in [`ios/Info.plist`](ios/Info.plist) and [`ios/ClippShareExtension/Info.plist`](ios/ClippShareExtension/Info.plist). The iOS app reads `CFBundleShortVersionString` at runtime via `Bundle.main`, so updating the plist is all that's required to refresh the About screen.

The Mac App Store build (`build_macos_mas.sh`) is an exception: App Store Connect rejects a `CFBundleVersion` with more than three integers, so the script rewrites it down to the 4th component alone (`105` from `1.0.4.105`) — a value that still increments per release. `CFBundleShortVersionString` (the user-visible `1.0.4`) is left untouched. The Developer ID build from `build_macos.sh` keeps the full 4-part `CFBundleVersion`.

### Bumping for a release

Setting the version across every file (`./scripts/bump_version.sh W.X.Y.Z`), tagging, and the pipeline a tag triggers are covered in [RELEASING.md](RELEASING.md#versioning-and-bumping).

## Environment variables

Most users won't need to set any of these; defaults work out of the box.

| Variable                                | Used by    | Default                                                                                            | Purpose                                                                                          |
|-----------------------------------------|------------|----------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------|
| `CLIPP_CACHE_DIR`                       | macOS, iOS | `$HOME/Library/Caches/clipp`                                                                       | Cache root. Subsumes the vcpkg checkout, binary cache, and (on macOS) the install dir.           |
| `VCPKG_ROOT`                            | all        | macOS/iOS: `$CLIPP_CACHE_DIR/vcpkg` (auto-cloned). Windows: auto-located via `vswhere` and common paths; **not** auto-cloned. | vcpkg checkout to use (vcpkg's own env var).                                                     |
| `VCPKG_DEFAULT_BINARY_CACHE`            | macOS, iOS | `$CLIPP_CACHE_DIR/vcpkg-binary-cache`                                                              | Binary cache for prebuilt vcpkg ports (vcpkg's own env var).                                     |
| `VCPKG_BINARY_SOURCES`                  | iOS        | `clear;files,$VCPKG_DEFAULT_BINARY_CACHE,readwrite`                                                | vcpkg binary cache configuration string (vcpkg's own env var).                                   |
| `APPLE_CODESIGN_IDENTITY`               | macOS, iOS | (unset)                                                                                            | Codesign identity hash or common name. Triggers `codesign` in the Ninja path; toggles Xcode signing in the Xcode path. |
| `APPLE_TEAM_ID`                         | macOS, iOS | (unset)                                                                                            | Apple Developer Team ID; paired with `APPLE_CODESIGN_IDENTITY` for the Xcode-generator path.     |
| `ARTIFACT_SIGNING_ENDPOINT`             | Windows    | (unset)                                                                                            | TrustedSigning endpoint URL passed to `sign.exe`.                                                |
| `ARTIFACT_SIGNING_ACCOUNT`              | Windows    | (unset)                                                                                            | TrustedSigning account name.                                                                     |
| `ARTIFACT_SIGNING_CERTIFICATE_PROFILE`  | Windows    | (unset)                                                                                            | TrustedSigning certificate profile.                                                              |

Windows signing is skipped silently unless all three `ARTIFACT_SIGNING_*` variables are set; a warning is emitted if some but not all are present. macOS signing is skipped if `APPLE_CODESIGN_IDENTITY` is unset.

The iOS vcpkg install root is fixed at `$REPO_ROOT/vcpkg-installed` because the Xcode project references that path directly in its header and library search paths.

## Code signing

Local builds are unsigned by default and run fine for development. On macOS an unsigned bundle just needs `xattr -dr com.apple.quarantine path/to/clipp.app` after copying (see [Troubleshooting](#troubleshooting)); for iOS simulator builds, pass `--disable-code-signing` to `build_ios.sh`, and iOS device builds use Xcode's signing configured through `ios/Clipp.xcodeproj`.

The build scripts *do* sign when the relevant credentials are present in the environment — `APPLE_CODESIGN_IDENTITY` (macOS Developer ID), the `ARTIFACT_SIGNING_*` variables (Windows Trusted Signing), or the `APPLE_*_3RDPARTY` set (Mac App Store). That signing infrastructure — Trusted Signing, Developer ID plus notarization, and Mac App Store certificates — along with the release process that drives it, is documented in [RELEASING.md](RELEASING.md).

## Troubleshooting

- **vcpkg builds fail on Windows with "path too long" or libsodium build errors.**
  vcpkg's internal build trees can exceed Windows' 260-character path limit if `VCPKG_ROOT` is deeply nested. Move `VCPKG_ROOT` to a short path (e.g. `C:\v`) or `subst` a drive letter for the build.

- **macOS build fails with "Missing autotools required by libsodium's vcpkg port".**
  Install via Homebrew (`brew install autoconf autoconf-archive automake libtool`) or MacPorts (`sudo port install autoconf autoconf-archive automake libtool`).

- **iOS build fails on Intel Macs.**
  Only `arm64-ios-simulator` is supported in the current scripts. Use an Apple Silicon machine or build manually from Xcode.

- **Windows `dumpbin /dependents` shows unexpected runtime DLLs.**
  The Release build is meant to be statically linked. If `VCRUNTIME`, `MSVCP`, `ucrtbase`, or any of `libsodium`, `xxhash`, `zstd`, `lodepng` appear as imports, your triplet is wrong — use `x64-windows-static` (or `arm64-windows-static`), not `x64-windows`.

- **macOS `.app` is built but won't launch on another Mac.**
  Unsigned bundles need `xattr -dr com.apple.quarantine path/to/clipp.app` after the first copy, or proper Developer ID signing plus notarization for distribution.

- **`dnsapi`-related link or runtime errors on Windows arm64.**
  `dnsapi.lib` is part of the Windows SDK and ships for all architectures, so a missing symbol typically means the wrong SDK is selected. Verify the Windows SDK component is installed for the target arch and that `vcvarsall.bat <arch>` succeeded — the build script prints the imported environment for inspection.
