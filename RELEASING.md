# Releasing Clipp

This document covers cutting a release: versioning, the CI pipeline that builds, signs, and publishes GitHub Releases, the manual Mac App Store and iOS submissions, and the signing infrastructure behind all of them. For building from a source checkout (prerequisites, the build scripts, local development), see [BUILDING.md](BUILDING.md).

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

Clipp ships through three independent channels. Only the first is automated.

| Channel | Platforms | Driven by | Signing |
|---------|-----------|-----------|---------|
| **GitHub Releases** | Windows amd64 + arm64, macOS arm64 | CI — [`_release.yml`](.github/workflows/_release.yml), via tag push or manual dispatch | Windows: Azure Trusted Signing; macOS: Developer ID + notarized |
| **Mac App Store** | macOS arm64 (sandboxed) | Manual — `./scripts/build_macos_mas.sh --upload` | Apple Distribution `.app` + Installer-signed `.pkg`, embedded provisioning profile |
| **iOS App Store / TestFlight** | iOS arm64 | Manual — Xcode archive (out of band) | Apple Distribution |

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

[`_release.yml`](.github/workflows/_release.yml) is a reusable workflow (`workflow_call`, not directly triggerable) taking `version`, `tag`, and `draft`. The two callers above compute those and pass `secrets: inherit`. Three jobs:

| Job | Runner(s) | Produces |
|-----|-----------|----------|
| `build-windows` (matrix: amd64, arm64) | `windows-latest`, `windows-11-arm` | Built unsigned, then Trusted-Signing-signed; `clipp-windows-<arch>.zip` (exe + com), `clipp-<ver>-windows-<arch>-symbols.zip` (PDBs), and `clipp-windows-<arch>.msix` (signed, sideloadable) |
| `build-macos` (arm64) | `macos-latest` | `build_macos.sh --notarize` → Developer ID-signed, notarized, stapled `clipp-macos-arm64.zip` |
| `build-linux` (matrix: amd64, arm64) | `ubuntu-latest`, `ubuntu-24.04-arm` | Built in a `debian:11` (glibc 2.31) container, static libstdc++; `clipp-linux-<arch>.{deb,rpm,pkg.tar.zst}` via nfpm + the raw `clipp-linux-<arch>` binary. Unsigned (attestation covers integrity) |
| `publish` | `ubuntu-latest` | Downloads artifacts, writes a build-provenance attestation, creates the GitHub release |

Installable assets are deliberately **version-less** (`clipp-windows-amd64.zip`, not `clipp-1.2.3.4-windows-amd64.zip`) so the README can link stable `releases/latest/download/<name>` URLs that survive every release. Only the Windows **symbols** zip keeps the version (it's a build-tied debug artifact, never linked). The version still travels in `clipp --version`, the macOS `Info.plist`, and Linux package metadata; the Sigstore attestation binds by SHA256, so the filename carrying no version costs nothing.

The Windows `.msix` is the same package `scripts/package_windows_msix.ps1` builds — packed unsigned (`-NoSign`) after the exe/com are signed (so it embeds the signed binaries and derives its `Publisher` from them), then signed by a second Trusted Signing step. Because Trusted Signing chains to a public root, users can `Add-AppxPackage` it directly with no certificate import.

Both build jobs run in the **`release-signing`** GitHub Environment, which scopes the OIDC token's `subject` claim (`repo:martona/clipp:environment:release-signing`). The Azure federated credential matches on that claim, so tag and manual triggers authenticate identically without a stored client secret. Each build job verifies its output before signing — the Windows job additionally asserts a static dependency closure (fails if `clipp.exe` imports `VCRUNTIME`/`MSVCP`/`ucrtbase`/`libsodium`/… instead of linking them statically).

What this pipeline does **not** do: the Mac App Store `.pkg` and the iOS app are separate manual flows (below). The non-release CI workflows ([`windows-ci.yml`](.github/workflows/windows-ci.yml), [`macos-ci.yml`](.github/workflows/macos-ci.yml), [`ios-ci.yml`](.github/workflows/ios-ci.yml)) build **unsigned** on every push and PR to `master` to catch breakage early; they never sign or publish.

## Mac App Store

The build/flag mechanics of `build_macos_mas.sh` (`--sign` / `--package` / `--upload`, the sandbox entitlements, the local-disk staging) are documented in [BUILDING.md](BUILDING.md). This section covers the release-specific pieces.

**One-time portal setup** (developer.apple.com — walking through the UI is out of scope here):

- An explicit App ID `net.clipp.ios` enabled for macOS. The macOS app intentionally shares the iOS bundle ID so the two are a single **Universal Purchase** in App Store Connect.
- A **3rd Party Mac Developer Application** certificate (signs the `.app`) and a **3rd Party Mac Developer Installer** certificate (signs the `.pkg`).
- A **Mac App Store** distribution provisioning profile for `net.clipp.ios`.

**Submitting a build** — set the environment, then run the upload:

```sh
export APPLE_TEAM_ID=XXXXXXXXXX
export APPLE_CODESIGN_IDENTITY_3RDPARTY="3rd Party Mac Developer Application: … (XXXXXXXXXX)"
export APPLE_CODESIGN_INSTALLER_IDENTITY_3RDPARTY="3rd Party Mac Developer Installer: … (XXXXXXXXXX)"
export APPLE_MAS_CLIPP_PROVISIONING_PROFILE=/path/to/Clipp_Mac_App_Store.provisionprofile
export APPLE_API_KEY_PATH=/path/to/AuthKey_XXXXXXXXXX.p8
export APPLE_API_KEY_ID=XXXXXXXXXX
export APPLE_API_ISSUER_ID=xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx

rm -rf build-mas && ./scripts/build_macos_mas.sh --upload
```

The script signs the sandboxed app with the Apple Distribution identity, embeds the provisioning profile, injects the `application-identifier` / `team-identifier` entitlements, wraps it in an Installer-signed `.pkg`, and uploads via `xcrun altool`. On success the build appears under the macOS platform in App Store Connect after processing — attach it to the version and submit for review.

**Export-compliance / encryption:** `Info.plist` declares `ITSAppUsesNonExemptEncryption = false`. Clipp encrypts with libsodium (standard, published algorithms), which qualifies for the export exemption — Apple's questionnaire confirms no documents are required. This is consistent **only with France excluded** from availability (App Store Connect → app → Pricing and Availability); shipping to France would instead require the French encryption declaration form.

## iOS

iOS releases are out of band; the CI pipeline does not touch them. The two iOS plists reference `$(MARKETING_VERSION)` / `$(CURRENT_PROJECT_VERSION)`, so the version is supplied at archive time — an archive made without overrides is a `0.0.0.0` dev build and App Store Connect would reject it:

1. Archive with the version passed as build-setting overrides (one override stamps the app and the share extension in lockstep):

   ```sh
   xcodebuild -project ios/Clipp.xcodeproj -scheme Clipp \
       -destination 'generic/platform=iOS' \
       MARKETING_VERSION=1.2.3 CURRENT_PROJECT_VERSION=1.2.3.4 \
       archive
   ```

   `Clipp` is Xcode's auto-created scheme for the app target. Without `-archivePath` the archive lands in Xcode's default location and appears in the Organizer. (Setting the two values on both targets in Xcode and using **Product → Archive** works too — just don't commit them; the tree stays at the `0.0.0` / `0.0.0.0` placeholders.)
2. From the Organizer, distribute to App Store Connect (or export the archive and upload with Transporter).
3. Submit for review or push to TestFlight in App Store Connect.

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
| `APPLE_KEYCHAIN_PASSWORD` | Password for the temporary CI keychain the cert is imported into |
| `APPLE_API_KEY_P8` | App Store Connect API key (`.p8` contents) used for notarization |

### Variables

| Name | Value |
|------|-------|
| `ARTIFACT_SIGNING_ENDPOINT`, `ARTIFACT_SIGNING_ACCOUNT`, `ARTIFACT_SIGNING_CERTIFICATE_PROFILE` | Trusted Signing endpoint / account / certificate profile |
| `APPLE_CODESIGN_IDENTITY` | Developer ID Application identity name (must match the imported cert) |
| `APPLE_TEAM_ID` | Apple Developer Team ID |
| `APPLE_API_KEY_ID`, `APPLE_API_ISSUER_ID` | App Store Connect API key ID + issuer UUID (notarization) |

The Mac App Store credentials (`APPLE_CODESIGN_IDENTITY_3RDPARTY`, `APPLE_CODESIGN_INSTALLER_IDENTITY_3RDPARTY`, `APPLE_MAS_CLIPP_PROVISIONING_PROFILE`) are **not** in CI — that flow is manual and reads them from your shell.

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
- **Windows (Trusted Signing):** install the tool (`dotnet tool install --global --prerelease sign`), `az login` to an account with signing access, set `ARTIFACT_SIGNING_ENDPOINT` / `ARTIFACT_SIGNING_ACCOUNT` / `ARTIFACT_SIGNING_CERTIFICATE_PROFILE`, then run `build_windows.ps1` *without* `-DisableCodeSigning`.
