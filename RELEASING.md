# Releasing Clipp

This document covers cutting a release: versioning, the CI pipeline that builds, signs, and publishes GitHub Releases and uploads the Mac App Store and iOS builds to App Store Connect, and the signing infrastructure behind all of it. For building from a source checkout (prerequisites, the build scripts, local development), see [BUILDING.md](BUILDING.md).

## Contents

- [Release channels](#release-channels)
- [Versioning](#versioning)
- [Cutting a GitHub release](#cutting-a-github-release)
- [The CI pipeline](#the-ci-pipeline)
- [Mac App Store](#mac-app-store)
- [iOS](#ios)
- [Signing infrastructure](#signing-infrastructure)
- [Secrets and variables](#secrets-and-variables)
- [Producing a signed build locally](#producing-a-signed-build-locally)

## Release channels

Clipp ships through three channels, all riding the same release pipeline ([`_release.yml`](.github/workflows/_release.yml), triggered by tag push or manual dispatch). The App Store jobs skip cleanly with a notice until their secrets are configured.

| Channel | Platforms | Driven by | Signing |
|---------|-----------|-----------|---------|
| **GitHub Releases** | Windows amd64 + arm64, macOS arm64, Linux amd64 + arm64 | CI — `build-*` jobs | Windows: Azure Trusted Signing; macOS: Developer ID + notarized; Linux: unsigned (attestation covers integrity) |
| **Mac App Store** | macOS arm64 (sandboxed) | CI — `appstore-macos` job (locally: `./scripts/build_macos_mas.sh --upload`) | Apple Distribution `.app` + Installer-signed `.pkg`, embedded provisioning profile |
| **iOS App Store / TestFlight** | iOS arm64 | CI — `appstore-ios` job (locally: `./scripts/build_ios_appstore.sh --upload`) | Dev-signed archive re-signed at export with Apple Distribution + two App Store profiles |

The GitHub Releases and Mac App Store macOS builds are *different binaries* from the same source: the former is a Developer ID, non-sandboxed bundle for direct download; the latter is sandboxed and signed for the store. Both carry the bundle ID `net.clipp.ios` (shared with iOS so the store listing is one Universal Purchase).

## Versioning

The version is a 4-part `W.X.Y.Z` string and it is **tag-canonical**: no file in the tree carries it. It exists only at release time — derived from the `v`-prefixed tag by [`release-tag.yml`](.github/workflows/release-tag.yml), or typed into the manual dispatch form — and the pipeline injects it into every build (`-DCLIPP_VERSION=...` for the CMake platforms; `MARKETING_VERSION` / `CURRENT_PROJECT_VERSION` `xcodebuild` overrides for iOS). Each build job then asserts the built artifact actually carries the requested version, so a plumbing regression fails the release instead of shipping a mis-stamped binary.

Unstamped builds — plain `cmake` runs, the build scripts without a version flag, local Xcode builds — come out as `0.0.0.0` (the CMake fallback, mirrored by the iOS project's placeholder build settings). A `0.0.0.0` binary is by definition a dev build. How the version reaches each platform's binary is documented in [BUILDING.md](BUILDING.md#versioning).

Two conventions to keep when picking a version:

- The 4th component (`Z`) is a build counter that increases **monotonically across all releases and never resets** when `W.X.Y` bumps. The Mac App Store build collapses `CFBundleVersion` down to that component alone (e.g. `105`) because App Store Connect rejects a 4-integer value there — so the store's "build number must increase" check is judged on `Z` in isolation.
- `CFBundleShortVersionString` everywhere is the 3-part `W.X.Y` (Apple rejects 4-part values there); everything else gets the full 4-part string.

## Cutting a GitHub release

Two entry points, both ending in the same reusable pipeline, and **both default to a draft** so you can review assets before they go public.

### By tag (the usual path)

```sh
git tag v1.2.3.4
git push --tags
```

No version-bump commit exists — the tag itself is the version (see [Versioning](#versioning)), so tag whichever commit you want to release.

[`release-tag.yml`](.github/workflows/release-tag.yml) fires on any `v*` tag, derives the version from the tag name (strips the leading `v`), and always builds a **draft**.

### By manual dispatch

Actions → **Release (manual)** → Run workflow. Supply the version (no `v` prefix) and optionally flip **Publish immediately**; left off, it's a draft. See [`release-manual.yml`](.github/workflows/release-manual.yml). Handy for re-running a release without moving a tag.

### Review and publish

The pipeline lands a draft GitHub release with all assets attached and auto-generated release notes. Review, then publish:

```sh
gh release edit v1.2.3.4 --draft=false
```

…or hit **Publish release** in the UI. (Manual dispatch with *Publish immediately* skips the draft step entirely.)

## The CI pipeline

[`_release.yml`](.github/workflows/_release.yml) is a reusable workflow (`workflow_call`, not directly triggerable) taking `version`, `tag`, and `draft`. The two callers above compute those and pass `secrets: inherit`. Six jobs:

| Job | Runner(s) | Produces |
|-----|-----------|----------|
| `build-windows` (matrix: amd64, arm64) | `windows-latest`, `windows-11-arm` | Built unsigned, then Trusted-Signing-signed; `clipp-windows-<arch>.zip` (exe + com), `clipp-<ver>-windows-<arch>-symbols.zip` (PDBs), and `clipp-windows-<arch>.msix` (signed, sideloadable) |
| `build-macos` (arm64) | `macos-latest` | `build_macos.sh --notarize` → Developer ID-signed, notarized, stapled `clipp-macos-arm64.zip` |
| `build-linux` (matrix: amd64, arm64) | `ubuntu-latest`, `ubuntu-24.04-arm` | Built in a `debian:11` (glibc 2.31) container, static libstdc++; `clipp-linux-<arch>.{deb,rpm,pkg.tar.zst}` via nfpm + the raw `clipp-linux-<arch>` binary. Unsigned (attestation covers integrity) |
| `appstore-macos` | `macos-latest` | `build_macos_mas.sh --upload` → sandboxed, Installer-signed `.pkg` uploaded to App Store Connect (no release asset) |
| `appstore-ios` | `macos-latest` | `build_ios_appstore.sh --upload` → signed `.ipa` (app + share extension) uploaded to App Store Connect / TestFlight (no release asset) |
| `publish` | `ubuntu-latest` | Downloads artifacts, writes a build-provenance attestation, creates the GitHub release |

Installable assets are deliberately **version-less** (`clipp-windows-amd64.zip`, not `clipp-1.2.3.4-windows-amd64.zip`) so the README can link stable `releases/latest/download/<name>` URLs that survive every release. Only the Windows **symbols** zip keeps the version (it's a build-tied debug artifact, never linked). The version still travels in `clipp --version`, the macOS `Info.plist`, and Linux package metadata; the Sigstore attestation binds by SHA256, so the filename carrying no version costs nothing.

The Windows `.msix` is the same package `scripts/package_windows_msix.ps1` builds — packed unsigned (`-NoSign`) after the exe/com are signed (so it embeds the signed binaries and derives its `Publisher` from them), then signed by a second Trusted Signing step. Because Trusted Signing chains to a public root, users can `Add-AppxPackage` it directly with no certificate import.

Both build jobs run in the **`release-signing`** GitHub Environment, which scopes the OIDC token's `subject` claim (`repo:martona/clipp:environment:release-signing`). The Azure federated credential matches on that claim, so tag and manual triggers authenticate identically without a stored client secret. Each build job verifies its output before signing — the Windows job additionally asserts a static dependency closure (fails if `clipp.exe` imports `VCRUNTIME`/`MSVCP`/`ucrtbase`/`libsodium`/… instead of linking them statically).

The two `appstore-*` jobs are deliberately **not** in `publish`'s `needs`: a store-side hiccup never blocks the GitHub release — re-run just the failed job. They upload at build time **even when the release is a draft**; the binary lands in App Store Connect processing, and releasing to the store remains a manual App Store Connect action either way. Both jobs skip cleanly with a notice until their secrets are configured, so forks and secret-less checkouts still release cleanly.

The non-release CI workflows ([`windows-ci.yml`](.github/workflows/windows-ci.yml), [`macos-ci.yml`](.github/workflows/macos-ci.yml), [`ios-ci.yml`](.github/workflows/ios-ci.yml)) build **unsigned** on every push and PR to `master` to catch breakage early; they never sign or publish.

## Mac App Store

The build/flag mechanics of `build_macos_mas.sh` (`--sign` / `--package` / `--upload`, the sandbox entitlements, the local-disk staging) are documented in [BUILDING.md](BUILDING.md). This section covers the release-specific pieces.

**One-time portal setup** (developer.apple.com — walking through the UI is out of scope here):

- An explicit App ID `net.clipp.ios` enabled for macOS. The macOS app intentionally shares the iOS bundle ID so the two are a single **Universal Purchase** in App Store Connect.
- A **3rd Party Mac Developer Application** certificate (signs the `.app`) and a **3rd Party Mac Developer Installer** certificate (signs the `.pkg`).
- A **Mac App Store** distribution provisioning profile for `net.clipp.ios`.

**Submitting a build** — CI's `appstore-macos` job runs this on every release (secrets permitting). To run the same flow locally, set the environment, then:

```sh
export APPLE_TEAM_ID=XXXXXXXXXX
export APPLE_CODESIGN_IDENTITY_3RDPARTY="3rd Party Mac Developer Application: … (XXXXXXXXXX)"
export APPLE_CODESIGN_INSTALLER_IDENTITY_3RDPARTY="3rd Party Mac Developer Installer: … (XXXXXXXXXX)"
export APPLE_MAS_CLIPP_PROVISIONING_PROFILE=/path/to/Clipp_Mac_App_Store.provisionprofile
export APPLE_API_KEY_PATH=/path/to/AuthKey_XXXXXXXXXX.p8
export APPLE_API_KEY_ID=XXXXXXXXXX
export APPLE_API_ISSUER_ID=xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx

rm -rf build-mas && ./scripts/build_macos_mas.sh --version 1.2.3.4 --upload
```

`--version` is mandatory for `--package`/`--upload`: the version is tag-canonical ([Versioning](#versioning)), so without it the store artifact would ship as `0.0.0.0` — the script refuses.

The script signs the sandboxed app with the Apple Distribution identity, embeds the provisioning profile, injects the `application-identifier` / `team-identifier` entitlements, wraps it in an Installer-signed `.pkg`, and uploads via `xcrun altool`. On success the build appears under the macOS platform in App Store Connect after processing — attach it to the version and submit for review.

**Export-compliance / encryption:** `Info.plist` declares `ITSAppUsesNonExemptEncryption = false`. Clipp encrypts with libsodium (standard, published algorithms), which qualifies for the export exemption — Apple's questionnaire confirms no documents are required. This is consistent **only with France excluded** from availability (App Store Connect → app → Pricing and Availability); shipping to France would instead require the French encryption declaration form.

## iOS

CI's `appstore-ios` job archives, signs, and uploads the iOS app — share extension included — on every release via [`scripts/build_ios_appstore.sh`](scripts/build_ios_appstore.sh): fully headless, no Xcode GUI or Apple-ID session. The same script is the local flow.

**One-time portal setup** (developer.apple.com):

- Explicit App IDs for **both** bundle ids: `net.clipp.ios` (shared with macOS for Universal Purchase) and `net.clipp.ios.ShareExtension`. The extension is its own bundle id, and App Groups on both targets rules out a wildcard — so each App ID needs its own **App Store** distribution provisioning profile. (Xcode's automatic signing registered the App IDs during past manual submissions; the two distribution profiles are the part created by hand.)
- An **Apple Distribution** certificate (the modern unified name — also valid for Mac App Store signing).

**Signing model** (deliberate, encoded in the script): the *archive* signs with automatic development signing — the team is baked into the pbxproj, and the API key only lets `xcodebuild` refresh dev profiles, which any key role may. The *export* re-signs manually with the Apple Distribution identity plus the two profiles; cloud signing is never attempted, so the API key can stay low-privilege. Before building, the script validates each profile against the bundle id it is supposed to sign (a swapped pair dies in seconds, not as an ITMS error after a full build); after archiving, it asserts the app and the extension actually carry the requested version.

**Submitting a build locally** — same env contract as CI:

```sh
export APPLE_IOS_CLIPP_PROVISIONING_PROFILE=/path/to/Clipp_iOS_App_Store.mobileprovision
export APPLE_IOS_CLIPPSE_PROVISIONING_PROFILE=/path/to/Clipp_ShareExt_App_Store.mobileprovision
export APPLE_API_KEY_PATH=/path/to/AuthKey_XXXXXXXXXX.p8
export APPLE_API_KEY_ID=XXXXXXXXXX
export APPLE_API_ISSUER_ID=xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx

./scripts/build_ios_appstore.sh --version 1.2.3.4 --upload
```

Omit `--upload` to export the signed `.ipa` to `build/ios-appstore/export/` instead (upload it later with Transporter, or re-run with `--skip-vcpkg --upload`). On success the build appears under TestFlight after processing — submit for review or distribute to testers in App Store Connect.

iOS shares C++ with the desktop build but drives its own Xcode project; see [BUILDING.md](BUILDING.md) for the iOS build details.

## Signing infrastructure

### Windows — Azure Trusted Signing

Release builds are signed with [Azure Trusted Signing](https://learn.microsoft.com/azure/trusted-signing/) — there is no certificate file anywhere in the repo or runner. CI authenticates to Azure via **OIDC** (`azure/login`), then `azure/trusted-signing-action` signs `clipp.exe` and `clipp.com` with an RFC-3161 timestamp. The Azure-side federated credential is matched to the `release-signing` environment's OIDC subject claim, so no client secret is stored — `AZURE_CLIENT_ID` / `AZURE_TENANT_ID` / `AZURE_SUBSCRIPTION_ID` are identifiers, not credentials.

Locally, `build_windows.ps1` reaches the same service through `sign.exe` (the Trusted Signing .NET global tool) when the `ARTIFACT_SIGNING_*` variables are set — see [Producing a signed build locally](#producing-a-signed-build-locally).

### macOS — Developer ID + notarization

`build_macos.sh --notarize` signs with a **Developer ID Application** identity (hardened runtime, [`Clipp.entitlements`](src/platform/macos/Clipp.entitlements)), submits the bundle to Apple's notary service via `xcrun notarytool --wait`, requires a `status: Accepted` verdict, then `xcrun stapler staple`s the ticket and re-packs the distributable zip. In CI the Developer ID cert is imported from the `APPLE_CERTIFICATE_P12` secret into a throwaway keychain (`apple-actions/import-codesign-certs`, auto-cleaned on job exit); the notary credentials come from an App Store Connect API key (`APPLE_API_KEY_P8` secret, written to a temp `.p8`, plus the key-id/issuer variables).

This is distinct from Mac App Store signing (Apple Distribution + `.pkg`, *not* notarized), which uses the two "3rd Party Mac Developer" certs — see [Mac App Store](#mac-app-store).

## Secrets and variables

Configured on the GitHub repo (`martona/clipp`) and inherited by `_release.yml` via `secrets: inherit`. **Secrets** hold private key material; **variables** (`vars.`) hold non-secret identifiers.

### Secrets

| Name | Used for |
|------|----------|
| `AZURE_CLIENT_ID`, `AZURE_TENANT_ID`, `AZURE_SUBSCRIPTION_ID` | OIDC login to Azure Trusted Signing (Windows) |
| `APPLE_CERTIFICATE_P12` | Base64 of the Developer ID Application certificate (`.p12`) |
| `APPLE_CERTIFICATE_P12_PASSWORD` | Password protecting that `.p12` |
| `APPLE_KEYCHAIN_PASSWORD` | Password for the temporary CI keychain the certs are imported into |
| `APPLE_API_KEY_P8` | App Store Connect API key (`.p8` contents): notarization + App Store uploads |
| `APPLE_APPSTORE_P12` | Base64 of **one** `.p12` bundling every App Store identity + private key: Apple Distribution, 3rd Party Mac Developer Installer, 3rd Party Mac Developer Application (if used), Apple Development (the iOS archive's dev signing). Export them together from Keychain Access |
| `APPLE_APPSTORE_P12_PASSWORD` | Password protecting that `.p12` |
| `APPLE_MAS_CLIPP_PROVISIONING_PROFILE_B64` | Base64 of the Mac App Store provisioning profile (`net.clipp.ios`) |
| `APPLE_IOS_CLIPP_PROVISIONING_PROFILE_B64` | Base64 of the iOS App Store profile for the app (`net.clipp.ios`) |
| `APPLE_IOS_CLIPPSE_PROVISIONING_PROFILE_B64` | Base64 of the iOS App Store profile for the share extension (`net.clipp.ios.ShareExtension`) |

### Variables

| Name | Value |
|------|-------|
| `ARTIFACT_SIGNING_ENDPOINT`, `ARTIFACT_SIGNING_ACCOUNT`, `ARTIFACT_SIGNING_CERTIFICATE_PROFILE` | Trusted Signing endpoint / account / certificate profile |
| `APPLE_CODESIGN_IDENTITY` | Developer ID Application identity name (must match the imported cert) |
| `APPLE_TEAM_ID` | Apple Developer Team ID |
| `APPLE_API_KEY_ID`, `APPLE_API_ISSUER_ID` | App Store Connect API key ID + issuer UUID (notarization + App Store uploads) |
| `APPLE_CODESIGN_IDENTITY_3RDPARTY` | Mac App Store app-signing identity name |
| `APPLE_CODESIGN_INSTALLER_IDENTITY_3RDPARTY` | Mac App Store installer (`.pkg`) identity name |
| `APPLE_CODESIGN_IDENTITY_IOS` | iOS distribution identity name (optional; the script defaults to `Apple Distribution`) |

The same env names, pointed at local files instead of CI-decoded ones, drive the scripts outside CI — see [Mac App Store](#mac-app-store) and [iOS](#ios).

## Producing a signed build locally

You normally don't need to — CI signs releases — but to reproduce a signed/notarized build on your own machine:

- **macOS (Developer ID + notarized):**
  ```sh
  export APPLE_CODESIGN_IDENTITY="Developer ID Application: … (XXXXXXXXXX)"
  export APPLE_TEAM_ID=XXXXXXXXXX
  export APPLE_API_KEY_PATH=/path/AuthKey_XXXXXXXXXX.p8 APPLE_API_KEY_ID=XXXXXXXXXX APPLE_API_ISSUER_ID=…
  ./scripts/build_macos.sh --version 1.2.3.4 --notarize
  ```
- **macOS (Mac App Store):** see [Mac App Store](#mac-app-store).
- **iOS (App Store / TestFlight):** see [iOS](#ios).
- **Windows (Trusted Signing):** install the tool (`dotnet tool install --global --prerelease sign`), `az login` to an account with signing access, set `ARTIFACT_SIGNING_ENDPOINT` / `ARTIFACT_SIGNING_ACCOUNT` / `ARTIFACT_SIGNING_CERTIFICATE_PROFILE`, then run `build_windows.ps1` *without* `-DisableCodeSigning`.
