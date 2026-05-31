# Releasing Clipp

This document covers cutting a release: versioning, the CI pipeline that builds, signs, and publishes GitHub Releases, the manual Mac App Store and iOS submissions, and the signing infrastructure behind all of them. For building from a source checkout (prerequisites, the build scripts, local development), see [BUILDING.md](BUILDING.md).

## Contents

- [Release channels](#release-channels)
- [Versioning and bumping](#versioning-and-bumping)
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

## Versioning and bumping

The version is a 4-part `W.X.Y.Z` string whose canonical default is the `set(CLIPP_VERSION "...")` line in [`CMakeLists.txt`](CMakeLists.txt). To set it everywhere at once, run from macOS:

```sh
./scripts/bump_version.sh 1.2.3.4
```

This rewrites the `CLIPP_VERSION` default in `CMakeLists.txt` and stamps `CFBundleShortVersionString` (3-part `1.2.3`) and `CFBundleVersion` (full 4-part) in [`ios/Info.plist`](ios/Info.plist) and [`ios/ClippShareExtension/Info.plist`](ios/ClippShareExtension/Info.plist). It needs macOS for `/usr/libexec/PlistBuddy`, and it does **not** commit or tag — review first:

```sh
git diff
git commit -am "Bump version to 1.2.3.4"
git tag v1.2.3.4
git push && git push --tags
```

Pushing the tag triggers the release pipeline (below). How the version reaches each platform's binary is documented in [BUILDING.md](BUILDING.md); note that the Mac App Store build rewrites `CFBundleVersion` down to the 4th component alone (e.g. `105`) because App Store Connect rejects a 4-integer value.

## Cutting a GitHub release

Two entry points, both ending in the same reusable pipeline, and **both default to a draft** so you can review assets before they go public.

### By tag (the usual path)

```sh
./scripts/bump_version.sh 1.2.3.4
git commit -am "Bump version to 1.2.3.4"
git tag v1.2.3.4
git push && git push --tags
```

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
export APPLE_MAS_PROVISIONING_PROFILE=/path/to/Clipp_Mac_App_Store.provisionprofile
export APPLE_API_KEY_PATH=/path/to/AuthKey_XXXXXXXXXX.p8
export APPLE_API_KEY_ID=XXXXXXXXXX
export APPLE_API_ISSUER_ID=xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx

rm -rf build-mas && ./scripts/build_macos_mas.sh --upload
```

The script signs the sandboxed app with the Apple Distribution identity, embeds the provisioning profile, injects the `application-identifier` / `team-identifier` entitlements, wraps it in an Installer-signed `.pkg`, and uploads via `xcrun altool`. On success the build appears under the macOS platform in App Store Connect after processing — attach it to the version and submit for review.

**Export-compliance / encryption:** `Info.plist` declares `ITSAppUsesNonExemptEncryption = false`. Clipp encrypts with libsodium (standard, published algorithms), which qualifies for the export exemption — Apple's questionnaire confirms no documents are required. This is consistent **only with France excluded** from availability (App Store Connect → app → Pricing and Availability); shipping to France would instead require the French encryption declaration form.

## iOS

iOS releases are out of band; the CI pipeline does not touch them. The app and its share extension build only through Xcode:

1. `./scripts/bump_version.sh W.X.Y.Z` — stamps the two iOS plists.
2. Open `ios/Clipp.xcodeproj`, select a device destination, **Product → Archive**.
3. From the Organizer, distribute to App Store Connect (or export the archive and upload with Transporter).
4. Submit for review or push to TestFlight in App Store Connect.

iOS shares C++ with the desktop build but drives its own Xcode project and holds its version separately in the iOS plists; see [BUILDING.md](BUILDING.md) for the iOS build details.

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

The Mac App Store credentials (`APPLE_CODESIGN_IDENTITY_3RDPARTY`, `APPLE_CODESIGN_INSTALLER_IDENTITY_3RDPARTY`, `APPLE_MAS_PROVISIONING_PROFILE`) are **not** in CI — that flow is manual and reads them from your shell.

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
